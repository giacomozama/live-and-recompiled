// The renderer on a thread of its own. The title's render thread records each call with what the console
// takes at call time (state changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory
// are read when the draw runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks
// and locks wait for that.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <smmintrin.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/interp.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
constexpr uint32_t kBatchCommands = 64;
// batches of a frame kept for Redisplay without frame generation; a frame past this (a long load with no Present) is
// not kept
constexpr size_t kKeptBatches = 4096;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring
// guest memory for the 2D draws frame generation moves part way (interp::MoveVertices): their vertices, their headers
constexpr uint32_t kMoveRing = 1u << 20, kMoveHeaders = 1u << 16;

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Redisplay, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport, Note };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    Surface s1, s2;
    interp::Tag tag;  // a draw's, as the title tagged it
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t> bytes;
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off, len;
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
std::vector<Range> StateRanges() {
    static const State s;
    return {
        // simple, rs, tss, vsc and psc go by their dirty blocks (SendTracked)
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.viewport), At(s, &s.rt) - At(s, &s.viewport)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), sizeof(s.transforms[0])},
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

// where State's vertex program constants are, in bytes
const size_t kVscAt = [] {
    static const State s;
    return At(s, s.vsc.data());
}();
constexpr size_t kVscBytes = sizeof(State{}.vsc.v), kVscBlockBytes = decltype(State::vsc)::kBlock * sizeof(State{}.vsc.v[0]);

// equal bytes, 16 at a time: most of the state is compared on every draw
bool Same(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i = 0;
    __m128i diff = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
    return i == n || !std::memcmp(a + i, b + i, n - i);
}

// what a 32 bit word of State is to Backend::Changed: anything else, a vertex source, a vertex program constant
enum : uint8_t { kWordState, kWordVertex, kWordConstant };

// the methods in simple the renderer never reads: vertex data and where it comes from, and constant uploads, which
// it takes from vsc
constexpr std::pair<uint32_t, uint32_t> kVertexMethods[] = {
    {0x0B80, 0x0C00},  // NV097_SET_TRANSFORM_CONSTANT
    {0x1720, 0x17A0},  // NV097_SET_VERTEX_DATA_ARRAY_OFFSET, _FORMAT
    {0x17FC, 0x1800},  // NV097_SET_BEGIN_END
    {0x1800, 0x1B00},  // NV097_ARRAY_ELEMENT16 to the inline vertex data
    {0x1EA4, 0x1EA8},  // NV097_SET_TRANSFORM_CONSTANT_LOAD
};

std::vector<uint8_t> WordKinds() {
    static const State s;
    std::vector<uint8_t> k(sizeof(State) / 4, kWordState);
    auto mark = [&](const void* p, size_t n, uint8_t kind) {
        for (size_t w = At(s, p) / 4, e = (At(s, p) + n) / 4; w < e; w++) k[w] = kind;
    };
    for (auto [lo, hi] : kVertexMethods) mark(&s.simple.v[lo >> 2], hi - lo, kWordVertex);
    mark(&s.streams, sizeof(s.streams), kWordVertex);
    mark(&s.index_buffer, sizeof(s.index_buffer), kWordVertex);
    mark(&s.index_data, sizeof(s.index_data), kWordVertex);
    mark(&s.base_vertex, sizeof(s.base_vertex), kWordVertex);
    mark(&s.vsc.v, sizeof(s.vsc.v), kWordConstant);
    return k;
}

using Clock = std::chrono::steady_clock;
// XBE_PERF timing, on PerfClock (gpu/draw.h): read only with XBE_PERF, as the title's thread takes it twice a call
uint64_t Ns(PerfClock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - t).count()); }

// Threaded's copy of a header in the ring (Header)
struct HeaderCopy {
    uint32_t at = 0;
    uint64_t made = 0;  // ring_pos_ after it was written
    uint8_t bytes[kHeaderBytes];
};
// a lookup in one of Threaded's maps, kept to skip the next one of the same key
template <typename T>
struct Recent {
    uint32_t key = 0;
    T* value = nullptr;
};

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner)
        : inner_(std::move(inner)), ranges_(StateRanges()), word_kinds_(WordKinds()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        interp::MoveRing(mem::AllocContiguous(kMoveRing, 0, 0xFFFFFFFF, 0x1000), kMoveRing,
                         mem::AllocSystem(kMoveHeaders, 64), kMoveHeaders);
        cur_ = NewBatch();
        std::thread([this] { Run(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        // at exit the worker may be gone mid-batch: then the inner backend is left, not freed under it
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) (void)inner_.release();
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        auto t = PerfClock::now();
        static const bool perf = diag::Perf();
        uint64_t tsc[6];
        if (perf) tsc[0] = __rdtsc();
        SendState(s, perf ? &tsc[1] : nullptr);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        c.tag = interp::g_draw_tag;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        cur_->cmds.push_back(c);
        if (perf) tsc[3] = __rdtsc();
        for (size_t i = 0; i < 4; i++) Use(s.textures[i], &slot_uses_[kSlotTextures + i]);
        for (size_t i = 0; i < 4; i++) Use(s.palettes[i], &slot_uses_[kSlotPalettes + i]);
        for (size_t i = 0; i < 16; i++) Use(s.streams[i].vb, &slot_uses_[kSlotStreams + i]);
        Use(s.rt.header, &slot_uses_[kSlotTarget]);
        Use(s.zs.header, &slot_uses_[kSlotDepth]);
        Written(s.rt);
        if (perf) tsc[4] = __rdtsc();
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        if (perf) {
            tsc[5] = __rdtsc();
            for (int i = 0; i < 5; i++) g_perf_rec_cycles[i].fetch_add(tsc[i + 1] - tsc[i], std::memory_order_relaxed);
        }
        g_perf_capture_ns += Ns(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        auto t = PerfClock::now();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += Ns(t);
    }

    // rects and points are taken now; the copy itself, CPU or GPU, runs in order like the console's
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override {
        auto t = PerfClock::now();
        Cmd c{Op::Copy};
        c.a = count;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.s1 = src;
        c.s1.header = Header(src.header);
        c.s2 = dst;
        c.s2.header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += Ns(t);
    }

    void Present(const Surface& front) override {
        auto t = PerfClock::now();
        Cmd c{Op::Present};
        c.s1 = front;
        c.s1.header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += Ns(t);
        // the title records one frame while the one before it draws
        Wait(last_present_, WaitFor::Frame, nullptr);
        last_present_ = id;
    }

    void Redisplay() override {
        cur_->cmds.push_back(Cmd{Op::Redisplay});
        Wait(Submit(), WaitFor::Frame, nullptr);
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf()) LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now", data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void Note(const char* text) override {
        Cmd c{Op::Note};
        c.data = Put(text, uint32_t(std::strlen(text) + 1));
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override {
        auto it = last_use_.find(resource);
        // a use recorded in the batch still being built is not on the GPU yet, so it is not busy; a submitted
        // batch that has not completed is. (The title polls this to know when it can touch a resource again, so
        // counting the unsubmitted batch would spin forever — it never reaches Swap to submit it.)
        return it != last_use_.end() && it->second > done_.load() && it->second != cur_->id;
    }
    void WaitResource(uint32_t resource) override {
        auto it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second, WaitFor::Resource, "resource");
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        last_written_ = 0;  // the set may lose it: the next draw inserts again
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data, late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    // slot: the state's place for it (kSlots), whose header is mostly the draw before's: its copy found without a lookup
    uint32_t Header(uint32_t header, Recent<HeaderCopy>* slot = nullptr) {
        if (!header || !ring_) return header;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        // two fixed sizes, not one picked at run time: the copy and the compare after it stay a few loads, not calls
        uint8_t now[kHeaderBytes] = {};
        const void* src = kernel::HostPtr(header);
        if (type < 0x20000 || type == 0x30000)
            std::memcpy(now, src, 12);
        else
            std::memcpy(now, src, kHeaderBytes);
        HeaderCopy* found;
        if (slot && slot->key == header) {
            found = slot->value;
        } else {
            Recent<HeaderCopy>& r = recent_headers_[(header >> 3) & 63];
            found = r.key == header ? r.value : &headers_[header];
            r = {header, found};
            if (slot) *slot = r;
        }
        HeaderCopy& c = *found;
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        std::memcpy(sent, value, n);
        uint32_t head[2] = {uint32_t(At(sent_, field)), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    // what changed since the last command, in 64 byte pieces
    // a Tracked array of s: its changed blocks compared with what was sent, in pieces of up to 64 bytes
    template <typename A>
    void SendTracked(const State& s, const A& a, const uint8_t* sent, const uint8_t* now) {
        const size_t base = At(s, a.data());
        constexpr size_t kBytes = sizeof(a.v), kBlockBytes = A::kBlock * sizeof(a.v[0]);
        for (uint64_t dirty = std::exchange(a.dirty, 0) & A::kAll; dirty; dirty &= dirty - 1) {
            size_t lo = base + size_t(__builtin_ctzll(dirty)) * kBlockBytes;
            size_t hi = std::min(lo + kBlockBytes, base + kBytes);
            if (Same(sent + lo, now + lo, hi - lo)) continue;
            for (size_t off = lo; off < hi; off += 64) Delta(sent + off, now + off, std::min<size_t>(64, hi - off));
        }
    }

    // tsc: XBE_PERF's cycle stamps, after the plain state and after the headers
    void SendState(const State& s, uint64_t* tsc = nullptr) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        // blocks of eight pieces compared at once: most of the state is the same draw to draw
        const uint8_t* sent = reinterpret_cast<const uint8_t*>(&sent_);
        // the tracked arrays only where they changed since: most of them is never touched draw to draw
        SendTracked(s, s.simple, sent, now);
        SendTracked(s, s.rs, sent, now);
        SendTracked(s, s.tss, sent, now);
        SendTracked(s, s.vsc, sent, now);
        SendTracked(s, s.psc, sent, now);
        for (const Range& r : ranges_)
            for (size_t block = r.off, end = r.off + r.len; block < end; block += 512) {
                size_t block_end = std::min<size_t>(block + 512, end);
                if (Same(sent + block, now + block, block_end - block)) continue;
                for (size_t off = block; off < block_end; off += 64)
                    Delta(sent + off, now + off, std::min<size_t>(64, block_end - off));
            }
        if (tsc) tsc[0] = __rdtsc();
        std::array<uint32_t, 4> textures;
        for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i], &slot_headers_[kSlotTextures + i]);
        Delta(&sent_.textures, &textures, sizeof(textures));
        std::array<uint32_t, 4> palettes;
        for (size_t i = 0; i < 4; i++) palettes[i] = Header(s.palettes[i], &slot_headers_[kSlotPalettes + i]);
        Delta(&sent_.palettes, &palettes, sizeof(palettes));
        // the title leaves all sixteen streams bound and draws from a few: the rest go as they are, which the render
        // thread only tells from 0, and a draw that reads one later sends its copy as a change
        auto streams = s.streams;
        for (uint32_t used = StreamsRead(s); used; used &= used - 1) {
            size_t i = size_t(__builtin_ctz(used));
            streams[i].vb = Header(streams[i].vb, &slot_headers_[kSlotStreams + i]);
        }
        Delta(&sent_.streams, &streams, sizeof(streams));
        Surface rt = s.rt, zs = s.zs;
        rt.header = Header(rt.header, &slot_headers_[kSlotTarget]);
        zs.header = Header(zs.header, &slot_headers_[kSlotDepth]);
        Delta(&sent_.rt, &rt, sizeof(rt));
        Delta(&sent_.zs, &zs, sizeof(zs));
        if (tsc) tsc[1] = __rdtsc();
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header, Recent<uint64_t>* slot = nullptr) {
        if (!header) return;
        if (slot && slot->key == header) {
            *slot->value = cur_->id;
            return;
        }
        Recent<uint64_t>& r = recent_uses_[(header >> 3) & 63];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
        if (slot) *slot = r;
    }
    void Written(const Surface& s) {
        if (!s.header) return;
        // a run of draws into one target inserts it once
        uint32_t data = mem::VirtualFromPhysical(s.data);
        if (data == last_written_) return;
        written_.insert(data);
        last_written_ = data;
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = PerfClock::now();
        {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    void Run() {
#ifdef _WIN32
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), L"render");
#else
        pthread_setname_np(pthread_self(), "render");
#endif
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                // a held picture is shown at its time, between batches or while there are none
                for (Clock::time_point at; queue_.empty() && !stop_ && inner_->Due(at);) {
                    if (cv_.wait_until(lock, at, [&] { return stop_ || !queue_.empty(); })) break;
                    lock.unlock();
                    inner_->Show();
                    lock.lock();
                }
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            for (const Cmd& c : b->cmds) Execute(*b, c);
            ShowDue();
            uint64_t id = b->id;
            // a frame's batches are kept to its Present, for frame generation to draw it again, and on to the next
            // frame's for Redisplay; the frame before goes
            std::vector<Batch*> freed;
            if (frame_over_) {
                freed.swap(last_frame_);
                if (frame_whole_) {
                    last_frame_.swap(frame_);
                    last_frame_.push_back(b);
                } else {
                    freed.push_back(b);
                }
                last_start_ = frame_start_;
                frame_over_ = false;
                BeginFrame();
            } else if (b->cmds.size() == 1 && b->cmds[0].op == Op::Redisplay) {
                freed.push_back(b);
            } else if (!keeping_ && (!frame_whole_ || frame_.size() >= kKeptBatches)) {
                freed.insert(freed.end(), frame_.begin(), frame_.end());
                frame_.clear();
                freed.push_back(b);
                frame_whole_ = false;
            } else {
                frame_.push_back(b);
            }
            for (Batch* f : freed) {
                f->cmds.clear();
                f->bytes.clear();
                f->vs.clear();
                f->ps.clear();
            }
            {
                std::lock_guard lock(m_);
                free_.insert(free_.end(), freed.begin(), freed.end());
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    void ShowDue() {
        Clock::time_point at;
        if (inner_->Due(at) && Clock::now() >= at) inner_->Show();
    }

    // a piece about to overwrite old at State offset off: true when a word other than a vertex source or constant differs
    bool ChangesState(const uint8_t* old, const uint8_t* now, size_t off, size_t n) const {
        if (off % 4 || n % 4) return true;
        for (size_t i = 0; i < n; i += 4) {
            if (std::memcmp(old + i, now + i, 4) == 0) continue;
            if (word_kinds_[(off + i) / 4] == kWordState) return true;
        }
        return false;
    }

    // a State command into st; changed, when given, learns whether more than vertex sources and constants changed
    void Apply(State& st, const Batch& b, const Cmd& c, bool* changed) const {
        const uint8_t* data = b.bytes.data() + c.data;
        uint8_t* s = reinterpret_cast<uint8_t*>(&st);
        for (const uint8_t* p = data; p < data + c.size;) {
            uint32_t head[2];
            std::memcpy(head, p, 8);
            if (changed && !*changed) *changed = ChangesState(s + head[0], p + 8, head[0], head[1]);
            // the constants' blocks written, for frame generation (interp.cpp's Stale): the title's dirty bits
            // don't come over, the render thread's are its own
            if (head[0] < kVscAt + kVscBytes && head[0] + head[1] > kVscAt) {
                size_t lo = std::max<size_t>(head[0], kVscAt) - kVscAt;
                size_t hi = std::min<size_t>(head[0] + head[1], kVscAt + kVscBytes) - kVscAt;
                for (size_t b = lo / kVscBlockBytes; b * kVscBlockBytes < hi; b++) st.vsc.dirty |= 1ull << b;
            }
            std::memcpy(s + head[0], p + 8, head[1]);
            p += 8 + head[1];
        }
        if (c.vs >= 0) st.vs = b.vs[size_t(c.vs)];
        if (c.ps >= 0) st.ps = b.ps[size_t(c.ps)];
        if (changed && (c.vs >= 0 || c.ps >= 0)) *changed = true;
    }

    // a frame starts: kept for drawing again, from the state it starts with; frame generation draws it again between
    // frames when it is on
    void BeginFrame() {
        keeping_ = app::Get().frame_generation > 1;
        frame_start_ = state_;
        frame_whole_ = true;
        frame_draws_ = 0;
    }

    // the title held (kernel::Hold): its last frame shown again, and drawn again whole as the settings change, so the
    // menu's graphics settings show on it. Twice: a target remade at a new scale or samples is remade at the end of
    // the first one's Present
    void ShowAgain() {
        std::string now = app::SettingsText();
        if (!redisplaying_) {
            redisplaying_ = true;
            settings_ = now;
        }
        if (now != settings_) {
            settings_ = std::move(now);
            again_ = 2;
        }
        if (!again_ || last_frame_.empty()) return inner_->Redisplay();
        again_--;
        DrawAgain();
    }

    // the last frame's commands again as they were, its own Present last; the live state is left as it is
    void DrawAgain() {
        State& rs = replay_state_;
        rs = last_start_;
        bool changed = true;
        for (const Batch* b : last_frame_)
            for (const Cmd& c : b->cmds) {
                const uint8_t* data = b->bytes.data() + c.data;
                switch (c.op) {
                    case Op::State: Apply(rs, *b, c, &changed); break;
                    case Op::Draw:
                        inner_->Changed(changed);
                        changed = false;
                        inner_->Draw(rs, c.a, c.b, c.c, c.d ? data : nullptr);
                        break;
                    case Op::Clear:
                        changed = true;
                        inner_->Clear(rs, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d);
                        break;
                    case Op::Copy:
                        changed = true;
                        inner_->Copy(c.s1, c.s2, c.a, c.b, c.c);
                        break;
                    case Op::ClearReport: inner_->ClearReport(); break;
                    case Op::GetReport: inner_->GetReport(c.a); break;
                    case Op::Present: inner_->Present(c.s1); return;
                    default: changed = true; break;  // nothing written back to the title, which is not running
                }
            }
    }

    // the frame's commands again, up to its Present in last, with the draws' constants at t from the frame before's;
    // only what draws: clears, copies, state, draws
    void Replay(const Batch& last, float t) {
        State& rs = replay_state_;
        rs = frame_start_;
        uint32_t n = 0;
        // what changed between draws, as Execute tells the backend: the blended constants don't count, so a run of
        // draws in one state keeps the backend's fast path as the frame did
        bool changed = true;
        interp::Undo undo;
        auto run = [&](const Batch& b) {
            ShowDue();  // a picture held from the frame before keeps its time while this one is drawn again
            for (const Cmd& c : b.cmds) {
                const uint8_t* data = b.bytes.data() + c.data;
                switch (c.op) {
                    case Op::State: Apply(rs, b, c, &changed); break;
                    case Op::Draw: {
                        if (interp::Recording()) interp::CheckVertices(n, rs, c.a, c.b, c.c, c.d ? data : nullptr);
                        static const bool perf = diag::Perf();
                        auto blend_start = perf ? PerfClock::now() : PerfClock::time_point{};
                        interp::Blended how = interp::Blend(n, t, rs, undo);
                        uint32_t start = c.b;
                        // a 2D draw placed on the CPU reads a copy of its vertices moved part way instead
                        if (interp::MoveVertices(n, t, how, rs, start, undo)) changed = true;
                        n++;
                        if (perf) gen_blend_ns_ += Ns(blend_start);
                        if (!interp::Hidden(how)) {
                            inner_->Changed(changed);
                            changed = false;
                            inner_->Draw(rs, c.a, start, c.c, c.d ? data : nullptr);
                        }
                        undo.Restore(rs);  // the draw's own constants and transforms, for the commands after
                        break;
                    }
                    case Op::Clear:
                        changed = true;
                        inner_->Clear(rs, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d);
                        break;
                    case Op::Copy:
                        changed = true;
                        inner_->Copy(c.s1, c.s2, c.a, c.b, c.c);
                        break;
                    default: changed = true; break;
                }
            }
        };
        for (const Batch* b : frame_) run(*b);
        run(last);
    }

    // a frame's Present: with frame generation, the pictures between the frame before and it drawn first
    void Present(const Batch& b, const Cmd& c) {
        interp::Presented(c.s1.data);
        uint32_t pictures = keeping_ ? inner_->Pictures() : 1;
        if (pictures > 1 && frame_draws_) {
            auto match_start = PerfClock::now();
            interp::Match();
            gen_match_ns_ += Ns(match_start);
        }
        auto replay_start = PerfClock::now();
        if (pictures > 1 && frame_draws_) {
            inner_->Keep(c.s1);
            for (uint32_t i = 1; i < pictures; i++) {
                float t = float(i) / float(pictures);
                inner_->Replaying(true);
                ShadingReplay(true, t);
                interp::BeginPass(t);
                Replay(b, t);
                interp::EndPass(i - 1);
                ShadingReplay(false);
                inner_->Replaying(false);
                inner_->Between(c.s1, i);
            }
            changed_ = true;
        }
        gen_replay_ns_ += Ns(replay_start);
        // XBE_PERF: what frame generation costs the render thread, a frame
        if (diag::Perf() && keeping_ && ++gen_frames_ >= 120) {
            LOGR("perf: frame generation: matching %.2f ms, drawing again %.2f ms a frame (blending %.2f), %u draws, "
                 "%u pictures",
                 gen_match_ns_ / 1e6 / gen_frames_, gen_replay_ns_ / 1e6 / gen_frames_, gen_blend_ns_ / 1e6 / gen_frames_,
                 frame_draws_, pictures);
            gen_frames_ = 0;
            gen_match_ns_ = gen_replay_ns_ = gen_blend_ns_ = 0;
        }
        inner_->Present(c.s1);
        interp::EndFrame();  // a frame not tracked leaves none to find draws in
        frame_over_ = true;
        redisplaying_ = false;
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        // anything but state and draws may change what the backend holds (targets, frames): the next draw is new
        if (c.op != Op::State && c.op != Op::Draw) changed_ = true;
        switch (c.op) {
            case Op::State: Apply(state_, b, c, &changed_); break;
            case Op::Draw:
                if (keeping_ || interp::Tracing()) {
                    auto t = PerfClock::now();
                    interp::Draw(state_, !changed_, c.tag, c.a, c.b, c.c, c.d ? data : nullptr);
                    gen_match_ns_ += Ns(t);
                    frame_draws_++;
                }
                inner_->Changed(changed_);
                changed_ = false;
                inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: Present(b, c); break;
            case Op::Redisplay: ShowAgain(); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
            case Op::Note: inner_->Note(reinterpret_cast<const char*>(data)); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;
    std::vector<uint8_t> word_kinds_;  // per State word, WordKinds
    bool changed_ = true;              // render thread: state changed since the last Draw (Backend::Changed)

    // the title's thread
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    std::array<Recent<HeaderCopy>, 64> recent_headers_{};
    std::array<Recent<uint64_t>, 64> recent_uses_{};
    // the state's header places, each with the lookup its last header had: a draw's mostly the draw before's
    enum : size_t { kSlotTextures = 0, kSlotPalettes = 4, kSlotStreams = 8, kSlotTarget = 24, kSlotDepth = 25, kSlots };
    std::array<Recent<HeaderCopy>, kSlots> slot_headers_{};
    std::array<Recent<uint64_t>, kSlots> slot_uses_{};
    uint32_t ring_ = 0;
    uint64_t ring_pos_ = 0;
    Batch* cur_ = nullptr;
    State sent_;  // the state as the render thread will have it
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    uint32_t last_written_ = 0;             // in written_ since Written put it there, 0 for none
    std::unordered_set<uint64_t> logged_;   // perf diagnostic: read-backs named once, by address and kind
    uint64_t next_id_ = 0, last_present_ = 0;

    // the render thread
    State state_;
    // the frame's batches run so far (the one running is not among them) and the state it started with, which
    // frame generation draws again while keeping_; frame_over_ once its Present ran
    std::vector<Batch*> frame_;
    State frame_start_, replay_state_;
    bool keeping_ = false, frame_over_ = false;
    // Redisplay: the frame presented last, its batches and the state it started with; the settings it was last drawn
    // with, and how many times more it is to be drawn again
    std::vector<Batch*> last_frame_;
    State last_start_;
    bool frame_whole_ = true, redisplaying_ = false;
    std::string settings_;
    uint32_t again_ = 0;
    uint32_t frame_draws_ = 0;
    uint64_t gen_match_ns_ = 0, gen_replay_ns_ = 0, gen_blend_ns_ = 0;  // XBE_PERF, over gen_frames_
    uint32_t gen_frames_ = 0;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
