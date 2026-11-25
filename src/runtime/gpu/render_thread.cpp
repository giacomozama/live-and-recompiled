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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
// the room the pooled batches keep for their bytes, all told: past it a batch goes back to the pool without its
// room. A frame's batches take ~40 KB each; a loading screen's draws (state changed throughout) several times that,
// and as the pool went round each batch kept that room for good: ~85 MB in thirty loads, ~0.6 MB more a load
// (2026-11-21). Play keeps ~15 MB
constexpr size_t kPoolBytes = 32u << 20;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring
// guest memory for the 2D draws frame generation moves part way (interp::MoveVertices): their vertices, their headers
constexpr uint32_t kMoveRing = 1u << 20, kMoveHeaders = 1u << 16;

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Redisplay, Refresh, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport, Note, PausePicture };

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
        if (RecordAside()) std::thread([this] { RecordLoop(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (RecordAside()) {
            {
                std::lock_guard lock(rec_m_);
                rec_stop_ = true;
            }
            rec_cv_.notify_all();
            std::unique_lock lock(rec_m_);
            rec_idle_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return rec_exited_; });
        }
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

    void Redisplay(bool blur) override {
        Cmd c{Op::Redisplay};
        c.a = blur;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::Frame, nullptr);
    }

    bool Refresh() override {
        cur_->cmds.push_back(Cmd{Op::Refresh});
        Wait(Submit(), WaitFor::Frame, nullptr);
        return refreshed_.load();
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

    void PausePicture(bool keep, const Surface& target) override {
        Cmd c{Op::PausePicture};
        c.a = keep;
        c.s1 = target;
        cur_->cmds.push_back(c);
    }
    bool PausePictures() const override { return inner_->PausePictures(); }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    // the batches made so far, those waiting in the pool and the memory their vectors keep (the pool's are idle)
    void MemoryReport() override {
        size_t made, pooled, queued, bytes = 0;
        {
            std::lock_guard lock(m_);
            made = batches_made_;
            pooled = free_.size();
            queued = queue_.size();
            for (const Batch* b : free_)
                bytes += b->cmds.capacity() * sizeof(Cmd) + b->bytes.capacity() +
                         (b->vs.capacity() + b->ps.capacity()) * sizeof(std::shared_ptr<void>);
        }
        LOGR("mem: render thread batches %zu made (%zu B a command, %u commands a batch), %zu in the pool keeping "
             "%.1f MB, %zu queued, the rest held by a frame",
             made, sizeof(Cmd), kBatchCommands, pooled, double(bytes) / 1048576.0, queued);
        inner_->MemoryReport();
    }
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
                batches_made_++;
            } else {
                b = free_.back();
                free_.pop_back();
                pooled_bytes_ -= b->bytes.capacity();
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

    static void NameThread(const wchar_t* wide, const char* name) {
#ifdef _WIN32
        (void)name;
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), wide);
#else
        (void)wide;
        pthread_setname_np(pthread_self(), name);
#endif
    }

    // Frame generation records the frame's draws (interp::Draw) on a thread of its own, beside the backend drawing
    // them: a batch's commands up to its Present, applied to a State of its own as Execute applies them to state_,
    // each draw recorded with the same changed flag Execute would give it. The Present waits for it before the draws
    // are matched; in the bloodbath recording was 2.5 ms a frame on the render thread. Nothing else touches
    // frame generation's own data until then, and the batches are kept to the Present while it keeps a frame.
    // XBE_INTERP_INLINE=1 records them on the render thread as before
    static bool RecordAside() {
        static const bool on = [] {
            const char* e = diag::Env("XBE_INTERP_INLINE");
            return !e || *e == '0';
        }();
        return on;
    }

    void RecordLoop() {
        NameThread(L"interp", "interp");
        for (;;) {
            std::pair<const Batch*, size_t> job;
            {
                std::unique_lock lock(rec_m_);
                rec_cv_.wait(lock, [&] { return rec_stop_ || !rec_jobs_.empty(); });
                if (rec_jobs_.empty()) break;
                job = rec_jobs_.front();
                rec_jobs_.pop_front();
            }
            auto start = PerfClock::now();
            const Batch& b = *job.first;
            for (size_t i = 0; i < job.second; i++) {
                const Cmd& c = b.cmds[i];
                switch (c.op) {
                    case Op::State: Apply(rec_state_, b, c, &rec_changed_); break;
                    case Op::Draw: {
                        const uint8_t* data = b.bytes.data() + c.data;
                        interp::Draw(rec_state_, !rec_changed_, c.tag, c.a, c.b, c.c, c.d ? data : nullptr);
                        rec_changed_ = false;
                        break;
                    }
                    default: rec_changed_ = true; break;  // as Execute: the next draw is new
                }
            }
            rec_ns_.fetch_add(Ns(start), std::memory_order_relaxed);
            {
                std::lock_guard lock(rec_m_);
                if (rec_jobs_.empty()) rec_busy_ = false;
            }
            rec_idle_cv_.notify_all();
        }
        {
            std::lock_guard lock(rec_m_);
            rec_exited_ = true;
        }
        rec_idle_cv_.notify_all();
    }

    // the commands of b before its Present (all of them without one) to the recorder; false when it takes none
    bool PostRecord(const Batch* b) {
        if (!keeping_ || !RecordAside() || interp::Tracing()) return false;
        size_t end = 0;
        while (end < b->cmds.size() && b->cmds[end].op != Op::Present) end++;
        if (!end) return false;
        {
            std::lock_guard lock(rec_m_);
            rec_jobs_.emplace_back(b, end);
            rec_busy_ = true;
        }
        rec_cv_.notify_one();
        rec_end_ = end;
        return true;
    }

    // the recorder done with everything posted to it
    void WaitRecord() {
        if (!RecordAside()) return;
        std::unique_lock lock(rec_m_);
        rec_idle_cv_.wait(lock, [&] { return !rec_busy_; });
    }

    void Run() {
        NameThread(L"render", "render");
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                const auto wait_start = Clock::now();
                // a held picture is shown at its time, between batches or while there are none
                for (Clock::time_point at; queue_.empty() && !stop_ && inner_->Due(at);) {
                    if (cv_.wait_until(lock, at, [&] { return stop_ || !queue_.empty(); })) break;
                    lock.unlock();
                    inner_->Show();
                    lock.lock();
                }
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                g_render_wait_ns.fetch_add(
                    uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wait_start).count()),
                    std::memory_order_relaxed);
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            rec_end_ = 0;
            const bool posted = PostRecord(b);
            for (exec_at_ = 0; exec_at_ < b->cmds.size(); exec_at_++) Execute(*b, b->cmds[exec_at_]);
            rec_end_ = 0;
            ShowDue();
            uint64_t id = b->id;
            // a frame's batches are kept to its Present while frame generation draws it again, and past it while the
            // game is frozen (held_frame_, DrawFrozen draws it again)
            std::vector<Batch*> freed;
            const bool alone = b->cmds.size() == 1 && (b->cmds[0].op == Op::Redisplay || b->cmds[0].op == Op::Refresh);
            if (frame_over_) {
                frame_.push_back(b);
                freed.swap(held_frame_);
                if (holding_ && g_freeze.load()) {
                    held_frame_.swap(frame_);
                    held_start_ = frame_start_;
                    held_front_ = last_front_;
                } else {
                    freed.insert(freed.end(), frame_.begin(), frame_.end());
                    frame_.clear();
                }
                frame_over_ = false;
                BeginFrame();
            } else if (holding_ && !alone) {
                frame_.push_back(b);
            } else {
                if (posted) WaitRecord();  // a held frame's Redisplay, read by the recorder
                freed.push_back(b);
            }
            for (Batch* f : freed) {
                f->cmds.clear();
                f->bytes.clear();
                f->vs.clear();
                f->ps.clear();
            }
            std::vector<std::vector<uint8_t>> dropped;  // rooms past kPoolBytes, let go outside the lock
            {
                std::lock_guard lock(m_);
                for (Batch* f : freed) {
                    if (pooled_bytes_ + f->bytes.capacity() > kPoolBytes) {
                        dropped.push_back(std::move(f->bytes));  // moved from: empty, no room
                        f->bytes = std::vector<uint8_t>();
                    }
                    pooled_bytes_ += f->bytes.capacity();
                    free_.push_back(f);
                }
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
    // frames when it is on, and the motion passes draw it again for the motion vectors
    void BeginFrame() {
        motion_ = inner_->Motion();
        keeping_ = app::Get().frame_generation > 1 || motion_;
        holding_ = keeping_ || g_freeze.load();  // a frame frozen on is kept whole
        frame_start_ = state_;
        if (keeping_) {
            // the recorder starts the frame where the render thread is (idle: the Present waited for it)
            WaitRecord();
            rec_state_ = state_;
            rec_changed_ = changed_;
        }
        frame_draws_ = 0;
    }

    // the frame's commands again, up to its Present in last, with the draws' constants at t from the frame before's;
    // only what draws: clears, copies, state, draws. Plain: the frozen frame (held_frame_, its Present in its last
    // batch) as it was drawn, nothing blended
    void Replay(const Batch* last, float t, bool plain = false) {
        State& rs = replay_state_;
        rs = plain ? held_start_ : frame_start_;
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
                        static const bool check = interp::CheckingVertices();
                        if (check) interp::CheckVertices(n, rs, c.a, c.b, c.c, c.d ? data : nullptr);
                        static const bool perf = diag::Perf();
                        // a draw a run drawn again took with it (most of the bloodbath's): nothing to blend, the
                        // backend only counts it
                        interp::g_run_tag = c.tag;  // the HUD's placement reads it (gpu::g_hud_split)
                        if (plain) {
                            inner_->Changed(changed);
                            changed = false;
                            inner_->Draw(rs, c.a, c.b, c.c, c.d ? data : nullptr);
                            break;
                        }
                        if (inner_->Skips()) {
                            n++;
                            inner_->Changed(changed);
                            changed = false;
                            inner_->Draw(rs, c.a, c.b, c.c, c.d ? data : nullptr);
                            if (perf) gen_draws_++;
                            // the run drew it with the frame's indices: the frame before's too when they differ
                            const uint8_t* extra = nullptr;
                            uint32_t extra_count = 0, extra_prim = 0;
                            if (c.d && interp::Extra(n - 1, interp::Blended::kYes, extra_prim, extra, extra_count)) {
                                const interp::Blended how = interp::Blend(n - 1, t, rs, undo);
                                if (how == interp::Blended::kYes || how == interp::Blended::kBorrowed) {
                                    inner_->Uncounted();
                                    inner_->Draw(rs, extra_prim, c.b, extra_count, extra);
                                    changed = true;
                                }
                                undo.Restore(rs);
                            }
                            break;
                        }
                        auto blend_start = perf ? PerfClock::now() : PerfClock::time_point{};
                        interp::Blended how = interp::Blend(n, t, rs, undo);
                        if (perf) gen_blend_how_ns_ += Ns(blend_start);
                        uint32_t start = c.b;
                        // a 2D draw placed on the CPU reads a copy of its vertices moved part way instead
                        const bool moved = interp::MoveVertices(n, t, how, rs, start, undo);
                        if (moved) changed = true;
                        n++;
                        if (perf) {
                            gen_blend_ns_ += Ns(blend_start);
                            gen_blended_++;
                        }
                        if (!interp::Hidden(how)) {
                            inner_->Changed(changed);
                            changed = false;
                            // the frame's own draw but for its constants, unless its vertices were moved
                            inner_->AsDrawn(!moved);
                            auto draw_start = perf ? PerfClock::now() : PerfClock::time_point{};
                            inner_->Draw(rs, c.a, start, c.c, c.d ? data : nullptr);
                            // and the frame before's indices, when the title left chunks of it out this frame
                            const uint8_t* extra = nullptr;
                            uint32_t extra_count = 0, extra_prim = 0;
                            if (c.d && interp::Extra(n - 1, how, extra_prim, extra, extra_count)) {
                                inner_->Uncounted();
                                inner_->Draw(rs, extra_prim, start, extra_count, extra);
                                changed = true;
                            }
                            if (perf) {
                                gen_draw_ns_ += Ns(draw_start);
                                gen_draws_++;
                            }
                        } else {
                            inner_->Pass();  // the frame drew it: the backend's count of draws stays the frame's
                        }
                        auto undo_start = perf ? PerfClock::now() : PerfClock::time_point{};
                        undo.Restore(rs);  // the draw's own constants and transforms, for the commands after
                        if (perf) gen_undo_ns_ += Ns(undo_start);
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
                    case Op::PausePicture:  // the kept picture back under the pause drawn again; no keeping twice
                        changed = true;
                        if (!c.a) inner_->PausePicture(false, c.s1);
                        break;
                    default: changed = true; break;
                }
            }
        };
        for (const Batch* b : plain ? held_frame_ : frame_) run(*b);
        if (last) run(*last);
    }

    // the HUD at the output's size (Backend::SetHudDraw): the frame's draws from from on, and the clears after, drawn
    // again at the time of the picture in slot (hud_t_), the draws before stepped over, the state between them all
    // applied; no copies (the backend takes no picture with one after from). Nothing when the frame's draws number
    // other than the backend counted
    static bool HudDrawThunk(void* self, uint32_t slot, uint32_t from, uint32_t total) {
        return static_cast<Threaded*>(self)->ReplayHud(slot, from, total);
    }
    bool ReplayHud(uint32_t slot, uint32_t from, uint32_t total) {
        if (!hud_last_) return false;
        const size_t batches = frame_.size() + 1;
        auto batch = [&](size_t i) -> const Batch& { return i < frame_.size() ? *frame_[i] : *hud_last_; };
        HudStart& hs = hud_start_;
        if (!hs.counted) {
            hs.draws = 0;
            for (size_t i = 0; i < batches; i++)
                for (const Cmd& c : batch(i).cmds) hs.draws += c.op == Op::Draw;
            hs.counted = true;
        }
        if (hs.draws != total) {
            static uint32_t told = 0;
            if (told++ < 8) LOGR("hud native: picture slot %u laid over instead: %u draws recorded, the backend counted %u", slot, hs.draws, total);
            return false;
        }
        const float t = slot < std::size(hud_t_) ? hud_t_[slot] : 1.0f;
        const bool blend = t < 1.0f;
        State& rs = replay_state_;
        // the frame's state up to draw from, walked once a Present: the draws before it are stepped over, the clears
        // before it too (but from 0's), so each picture's pass from there is the same. XBE_HUD_START=0: walked by
        // each picture, as before, to compare against
        static const bool keep_start = [] {
            const char* e = diag::Env("XBE_HUD_START");
            return !(e && *e == '0');
        }();
        if (!keep_start) hs.ok = false;
        if (from && !(hs.ok && hs.from == from)) {
            hs.ok = false;
            rs = frame_start_;
            uint32_t k = 0;
            for (size_t i = 0; i < batches && !hs.ok; i++) {
                const Batch& b = batch(i);
                for (size_t j = 0; j < b.cmds.size(); j++) {
                    const Cmd& c = b.cmds[j];
                    if (c.op == Op::State) Apply(rs, b, c, nullptr);
                    if (c.op != Op::Draw) continue;
                    if (k++ < from) continue;
                    hs.st = rs;
                    hs.from = from;
                    hs.batch = i;
                    hs.cmd = j;
                    hs.ok = true;
                    break;
                }
            }
            if (!hs.ok) return false;  // fewer draws than from: as counted, not reached
        }
        size_t first_batch = 0, first_cmd = 0;
        if (from) {
            rs = hs.st;
            first_batch = hs.batch;
            first_cmd = hs.cmd;
        } else {
            rs = frame_start_;
        }
        uint32_t n = from;
        bool changed = true;
        interp::Undo undo;
        if (blend) interp::BeginPass(t);  // the followers' kept blends are this pass's
        auto run = [&](const Batch& b, size_t at_cmd) {
            ShowDue();
            for (size_t j = at_cmd; j < b.cmds.size(); j++) {
                const Cmd& c = b.cmds[j];
                const uint8_t* data = b.bytes.data() + c.data;
                if (c.op == Op::State) {
                    Apply(rs, b, c, &changed);
                    continue;
                }
                // a clear after the copy (taken as draw from began; from 0: the whole picture, its first clears too)
                if (c.op == Op::Clear) {
                    if (n > from || from == 0) {
                        changed = true;
                        inner_->Clear(rs, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d);
                    }
                    continue;
                }
                if (c.op != Op::Draw) continue;
                const uint32_t at = n++;
                if (at < from) continue;
                inner_->HudDrawAt(at);
                interp::g_run_tag = c.tag;  // the HUD's placement reads it (gpu::g_hud_split)
                uint32_t start = c.b;
                bool shown = true;
                if (blend) {
                    const interp::Blended how = interp::Blend(at, t, rs, undo);
                    if (interp::MoveVertices(at, t, how, rs, start, undo)) changed = true;
                    shown = !interp::Hidden(how);
                }
                if (shown) {
                    inner_->Changed(changed);
                    changed = false;
                    inner_->Draw(rs, c.a, start, c.c, c.d ? data : nullptr);
                }
                undo.Restore(rs);
            }
        };
        for (size_t i = first_batch; i < batches; i++) run(batch(i), i == first_batch ? first_cmd : 0);
        return true;
    }

    // the frozen frame drawn again and presented, the settings read as they are now; without one kept, the picture
    // shown last again
    void DrawFrozen() {
        if (held_frame_.empty()) {
            inner_->Redisplay(false);
            refreshed_ = false;
            return;
        }
        Replay(nullptr, 1.0f, true);
        inner_->Present(held_front_);
        changed_ = true;  // the backend's state is the frozen frame's end
        refreshed_ = true;
    }

    // the frame's draws again for the motion vectors of its picture slot, drawn at t, each with its constants as the
    // picture before (at before) had them; the state between them, nothing else
    void Motion(const Batch& last, uint32_t slot, float t, float before) {
        static const bool perf = diag::Perf();
        auto motion_start = perf ? PerfClock::now() : PerfClock::time_point{};
        State& rs = replay_state_;
        rs = frame_start_;
        uint32_t n = 0;
        interp::Undo undo;
        Before was;
        interp::BeginMotion();
        ShadingReplay(true, t);
        inner_->MotionPass(true, slot);
        bool changed = true;  // state between the draws the backend works out (MotionChanged)
        auto run = [&](const Batch& b) {
            ShowDue();
            for (const Cmd& c : b.cmds) {
                const uint8_t* data = b.bytes.data() + c.data;
                if (c.op == Op::State) Apply(rs, b, c, &changed);
                else if (c.op != Op::Draw) changed = true;
                if (c.op != Op::Draw) continue;
                if (inner_->MotionSkips()) {
                    n++;
                    continue;
                }
                inner_->MotionChanged(changed);
                changed = false;
                // the picture before's constants and transforms, then the draw's at t
                was.vsc.clear();
                was.xf.clear();
                auto at_start = perf ? PerfClock::now() : PerfClock::time_point{};
                interp::At(n, before, 0, rs, undo);
                for (const auto& [r, v] : undo.vsc) was.vsc.emplace_back(r, rs.vsc.v[r]);
                for (const auto& [i, m] : undo.xf) was.xf.emplace_back(i, rs.transforms[i]);
                undo.Restore(rs);
                if (t < 1) interp::At(n, t, 1, rs, undo);
                n++;
                if (perf) gen_motion_at_ns_ += Ns(at_start);
                inner_->DrawMotion(rs, was, c.a, c.b, c.c, c.d ? data : nullptr);
                undo.Restore(rs);
            }
        };
        for (const Batch* b : frame_) run(*b);
        run(last);
        inner_->MotionPass(false, slot);
        ShadingReplay(false);
        if (perf) gen_motion_ns_ += Ns(motion_start);
    }

    // the frame's draws again once for the motion vectors of all its pictures (Backend::MotionAll): each draw recorded
    // for each picture in turn, at its t (ts) with its constants as the picture before it had them (befores), its
    // setup worked out once. Shading (fur) is tracked once, at the frame's own t
    void MotionAll(const Batch& last, uint32_t n, const uint32_t* in_slots, const float* in_ts,
                   const float* in_befores) {
        static const bool perf = diag::Perf();
        auto motion_start = perf ? PerfClock::now() : PerfClock::time_point{};
        // in the order of their times: a picture's picture before is then the one just drawn, its registers then this
        // one's before (chain), not blended again
        uint32_t slots[Backend::kMostPictures + 1];
        float ts[Backend::kMostPictures + 1], befores[Backend::kMostPictures + 1];
        {
            uint32_t order[Backend::kMostPictures + 1];
            for (uint32_t p = 0; p < n; p++) order[p] = p;
            std::sort(order, order + n, [&](uint32_t x, uint32_t y) { return in_ts[x] < in_ts[y]; });
            for (uint32_t p = 0; p < n; p++)
                slots[p] = in_slots[order[p]], ts[p] = in_ts[order[p]], befores[p] = in_befores[order[p]];
        }
        static const bool chain_on = [] {
            const char* e = diag::Env("XBE_MOTION_CHAIN");
            return !(e && *e == '0');
        }();
        State& rs = replay_state_;
        rs = frame_start_;
        uint32_t k = 0;
        interp::Undo undo;
        Before was, now;
        interp::BeginMotion();
        ShadingReplay(true, 1);
        inner_->MotionPasses(true, slots, n);
        bool changed = true;  // state between the draws the backend works out (MotionChanged)
        auto run = [&](const Batch& b) {
            ShowDue();
            for (const Cmd& c : b.cmds) {
                const uint8_t* data = b.bytes.data() + c.data;
                if (c.op == Op::State) Apply(rs, b, c, &changed);
                else if (c.op != Op::Draw) changed = true;
                if (c.op != Op::Draw) continue;
                if (inner_->MotionSkips()) {
                    k++;
                    continue;
                }
                inner_->MotionChanged(changed);
                changed = false;
                bool chained = false;  // now holds the picture before's registers
                for (uint32_t p = 0; p < n; p++) {
                    auto at_start = perf ? PerfClock::now() : PerfClock::time_point{};
                    if (chain_on && chained && p && befores[p] == ts[p - 1]) {
                        std::swap(was, now);
                    } else {
                        was.vsc.clear();
                        was.xf.clear();
                        interp::At(k, befores[p], 2 * p, rs, undo);
                        for (const auto& [r, v] : undo.vsc) was.vsc.emplace_back(r, rs.vsc.v[r]);
                        for (const auto& [i, m] : undo.xf) was.xf.emplace_back(i, rs.transforms[i]);
                        undo.Restore(rs);
                    }
                    now.vsc.clear();
                    now.xf.clear();
                    chained = ts[p] < 1;
                    if (chained) {
                        interp::At(k, ts[p], 2 * p + 1, rs, undo);
                        for (const auto& [r, v] : undo.vsc) now.vsc.emplace_back(r, rs.vsc.v[r]);
                        for (const auto& [i, m] : undo.xf) now.xf.emplace_back(i, rs.transforms[i]);
                    }
                    if (perf) gen_motion_at_ns_ += Ns(at_start);
                    inner_->DrawMotionAt(p, rs, was, c.a, c.b, c.c, c.d ? data : nullptr);
                    undo.Restore(rs);
                }
                k++;
            }
        };
        for (const Batch* b : frame_) run(*b);
        run(last);
        inner_->MotionPasses(false, slots, n);
        ShadingReplay(false);
        if (perf) gen_motion_ns_ += Ns(motion_start);
    }

    // a frame's Present: with frame generation, the pictures between the frame before and it drawn first; with motion
    // vectors, each picture's after it is drawn
    void Present(const Batch& b, const Cmd& c) {
        {
            auto wait_start = PerfClock::now();
            WaitRecord();  // the frame's draws all recorded
            gen_match_ns_ += Ns(wait_start);
            gen_rec_wait_ns_ += Ns(wait_start);
        }
        interp::Presented(c.s1.data);
        last_front_ = c.s1;
        // the frame's pictures' times: in step with the display's refresh they may number fewer than the most
        // (Pictures) and end short of the frame's own (t 1)
        float times[Backend::kMostPictures] = {1.0f};
        float step = 1.0f;
        uint32_t pictures = keeping_ ? std::clamp(inner_->PictureTimes(times, step), 1u, Backend::kMostPictures) : 1;
        // a frame drawn light shows no picture of its own: one at 1 (its own foreseen wrongly) is drawn again too
        const bool light = inner_->Light() && frame_draws_;
        const bool own = times[pictures - 1] >= 1.0f && !light;
        const bool gen = (pictures > 1 || !own) && frame_draws_;
        gen_light_ += light;
        // the time of the picture shown before picture i (from 0): the one before in this frame, or the frame before's
        // last, before this frame's start (below 0: the blend, linear, carries the frame's motion back to it). In
        // step with the refresh a frame's own may stand in for a time near it, so it is the times', not a step back
        const float last_t = shown_t_;
        auto before = [&](uint32_t i) { return i ? times[i - 1] : last_t - 1.0f; };
        shown_t_ = times[pictures - 1];
        bool motion = motion_ && frame_draws_;
        if ((gen || motion) && frame_draws_) {
            auto match_start = PerfClock::now();
            interp::Match();
            gen_match_ns_ += Ns(match_start);
            gen_matched_ns_ += Ns(match_start);
        }
        auto replay_start = PerfClock::now();
        // XBE_PERF: the backend's draws drawn again by the way they took, from its counters before and after
        const uint64_t was_draws = g_perf.draws, was_fast = g_perf.fast, was_reused = g_perf.reused,
                       was_redrawn = g_perf.redrawn, was_changed = g_perf.redraw_changed,
                       was_unkept = g_perf.redraw_unkept, was_refused = g_perf.redraw_refused,
                       was_textures = g_perf.redraw_textures, was_mslow = g_perf.motion_slow,
                       was_mreused = g_perf.motion_reused, was_mskipped = g_perf.motion_skipped;
        const double was_mslow_ms = g_perf.motion_slow_ms, was_mreused_ms = g_perf.motion_reused_ms;
        // the pictures' times by slot for the HUD drawn again at the output's size (ReplayHud): 1, the frame's own,
        // but for those drawn again
        for (float& h : hud_t_) h = 1.0f;
        if (gen) {
            // the frame's own picture takes the last slot, those drawn again the first ones in turn
            const uint32_t own_slot = std::max(inner_->Pictures(), 1u) - 1;
            inner_->Keep(c.s1, own);
            // all the pictures' motion in one pass after the last is drawn (each picture's depth kept as it is drawn,
            // MotionDepth), else each picture's pass after it
            const bool all = motion && inner_->MotionAll();
            uint32_t all_n = 0, all_slots[Backend::kMostPictures + 1];
            float all_t[Backend::kMostPictures + 1], all_before[Backend::kMostPictures + 1];
            if (motion && own) {
                if (all) {
                    inner_->MotionDepth();
                    all_slots[all_n] = own_slot, all_t[all_n] = 1, all_before[all_n++] = before(pictures - 1);
                } else {
                    Motion(b, own_slot, 1, before(pictures - 1));
                }
            }
            const uint32_t again = own ? pictures - 1 : pictures;
            for (uint32_t i = 1; i <= again; i++) {
                float t = times[i - 1];
                if (i - 1 < std::size(hud_t_)) hud_t_[i - 1] = t;
                inner_->Replaying(true);
                ShadingReplay(true, t);
                interp::BeginPass(t);
                Replay(&b, t);
                interp::EndPass(i - 1);
                ShadingReplay(false);
                inner_->Replaying(false);
                auto between_start = PerfClock::now();
                inner_->Between(c.s1, i);
                gen_between_ns_ += Ns(between_start);
                if (motion && all) {
                    inner_->MotionDepth();
                    all_slots[all_n] = i - 1, all_t[all_n] = t, all_before[all_n++] = before(i - 1);
                } else if (motion) {
                    Motion(b, i - 1, t, before(i - 1));
                }
            }
            if (all && all_n) MotionAll(b, all_n, all_slots, all_t, all_before);
            changed_ = true;
        } else if (motion) {
            // its own picture alone: in its own slot, from the picture shown before (the whole frame without frame
            // generation)
            Motion(b, std::max(inner_->Pictures(), 1u) - 1, 1, before(pictures - 1));
            changed_ = true;
        }
        gen_replay_ns_ += Ns(replay_start);
        if (g_perf.draws >= was_draws && g_perf.fast >= was_fast && g_perf.reused >= was_reused &&
            g_perf.redrawn >= was_redrawn) {
            gen_backend_draws_ += g_perf.draws - was_draws;
            gen_fast_ += g_perf.fast - was_fast;
            gen_reused_ += g_perf.reused - was_reused;
            gen_redrawn_ += g_perf.redrawn - was_redrawn;
            gen_changed_ += g_perf.redraw_changed - was_changed;
            gen_unkept_ += g_perf.redraw_unkept - was_unkept;
            gen_refused_ += g_perf.redraw_refused - was_refused;
            gen_textures_ += g_perf.redraw_textures - was_textures;
            gen_mslow_ += g_perf.motion_slow - was_mslow;
            gen_mreused_ += g_perf.motion_reused - was_mreused;
            gen_mskipped_ += g_perf.motion_skipped - was_mskipped;
            gen_mslow_ms_ += g_perf.motion_slow_ms - was_mslow_ms;
            gen_mreused_ms_ += g_perf.motion_reused_ms - was_mreused_ms;
        }
        // XBE_PERF: what frame generation costs the render thread, a frame
        if (diag::Perf() && keeping_ && ++gen_frames_ >= 120) {
            LOGR("perf: frame generation: matching %.2f ms, drawing again %.2f ms a frame (blending %.2f: %.0f draws "
                 "blended, %.2f us each, %.2f picking how, %.2f moving vertices; putting back %.2f), %u draws, "
                 "%u pictures, %u of %u frames drawn light",
                 gen_match_ns_ / 1e6 / gen_frames_, gen_replay_ns_ / 1e6 / gen_frames_, gen_blend_ns_ / 1e6 / gen_frames_,
                 double(gen_blended_) / gen_frames_, gen_blended_ ? gen_blend_ns_ / 1e3 / double(gen_blended_) : 0.0,
                 gen_blend_how_ns_ / 1e6 / gen_frames_, (gen_blend_ns_ - gen_blend_how_ns_) / 1e6 / gen_frames_,
                 gen_undo_ns_ / 1e6 / gen_frames_, frame_draws_, pictures, gen_light_, gen_frames_);
            gen_blended_ = gen_blend_how_ns_ = gen_undo_ns_ = 0;
            // matching: the frame's draws recorded (interp::Draw) and their match to the frame before's; drawing
            // again: the backend's draws (slow: the whole way, fast: kept state, reused: a run's draws as one, the
            // rest skipped with it), the pictures handed out, and what's left: the commands' state, the loop (the motion
            // passes apart, on a line of their own)
            const double f = double(gen_frames_);
            const double rest =
                double(gen_replay_ns_) - double(gen_blend_ns_ + gen_draw_ns_ + gen_between_ns_ + gen_motion_ns_);
            // recording: on the render thread, the recorder's draws waited for at the Present included; the
            // recorder's own time beside it
            LOGR("perf: frame generation split: recording %.2f (waited %.2f, its thread %.2f) matching %.2f | backend "
                 "draws %.2f ms for %.0f draws (%.0f slow, %.0f of them the frame's again; not: %.0f changed, %.0f not kept, "
                 "%.0f refused, %.0f other textures; %.0f fast, %.0f runs reused), pictures out %.2f, state and loop %.2f",
                 (gen_match_ns_ - gen_matched_ns_) / 1e6 / f, gen_rec_wait_ns_ / 1e6 / f,
                 rec_ns_.exchange(0, std::memory_order_relaxed) / 1e6 / f, gen_matched_ns_ / 1e6 / f,
                 gen_draw_ns_ / 1e6 / f,
                 gen_draws_ / f, (gen_backend_draws_ - gen_fast_) / f, gen_redrawn_ / f, gen_changed_ / f, gen_unkept_ / f, gen_refused_ / f, gen_textures_ / f, (gen_fast_ - gen_reused_) / f,
                 gen_reused_ / f,
                 gen_between_ns_ / 1e6 / f, rest / 1e6 / f);
            // the motion passes (DLSS's motion vectors), one a picture: their time, and their draws by the way they took
            if (motion_)
                LOGR("perf: motion passes: %.2f ms a frame, %.0f draws (%.0f worked out %.2f ms, %.0f taken from the "
                     "frame's first pass %.2f ms, %.0f left out) | constants blended %.2f ms, the rest %.2f",
                     gen_motion_ns_ / 1e6 / f, (gen_mslow_ + gen_mreused_ + gen_mskipped_) / f, gen_mslow_ / f,
                     gen_mslow_ms_ / f, gen_mreused_ / f, gen_mreused_ms_ / f, gen_mskipped_ / f,
                     gen_motion_at_ns_ / 1e6 / f,
                     (double(gen_motion_ns_ - gen_motion_at_ns_) / 1e6 - gen_mslow_ms_ - gen_mreused_ms_) / f);
            gen_frames_ = gen_light_ = 0;
            gen_match_ns_ = gen_replay_ns_ = gen_blend_ns_ = 0;
            gen_matched_ns_ = gen_draw_ns_ = gen_between_ns_ = gen_rec_wait_ns_ = 0;
            gen_draws_ = gen_backend_draws_ = gen_fast_ = gen_reused_ = gen_redrawn_ = 0;
            gen_changed_ = gen_unkept_ = gen_refused_ = gen_textures_ = 0;
            gen_motion_ns_ = gen_mslow_ = gen_mreused_ = gen_mskipped_ = gen_motion_at_ns_ = 0;
            gen_mslow_ms_ = gen_mreused_ms_ = 0;
        }
        // the frame's commands kept: the HUD can be drawn again over the upscaled pictures
        hud_last_ = keeping_ && frame_draws_ ? &b : nullptr;
        inner_->SetHudDraw(hud_last_ ? &HudDrawThunk : nullptr, this);
        hud_start_.counted = hud_start_.ok = false;
        inner_->Present(c.s1);
        inner_->SetHudDraw(nullptr, nullptr);
        hud_last_ = nullptr;
        hud_start_.counted = hud_start_.ok = false;
        interp::EndFrame();  // a frame not tracked leaves none to find draws in
        frame_over_ = true;
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        // anything but state and draws may change what the backend holds (targets, frames): the next draw is new
        if (c.op != Op::State && c.op != Op::Draw) changed_ = true;
        switch (c.op) {
            case Op::State: Apply(state_, b, c, &changed_); break;
            case Op::Draw:
                if (keeping_ || interp::Tracing()) {
                    if (exec_at_ >= rec_end_) {  // not the recorder's: after the Present, or recorded here
                        auto t = PerfClock::now();
                        interp::Draw(state_, !changed_, c.tag, c.a, c.b, c.c, c.d ? data : nullptr);
                        gen_match_ns_ += Ns(t);
                    }
                    frame_draws_++;
                }
                inner_->Changed(changed_);
                changed_ = false;
                interp::g_run_tag = c.tag;
                inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: Present(b, c); break;
            case Op::Redisplay: inner_->Redisplay(c.a != 0); break;
            case Op::Refresh: DrawFrozen(); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
            case Op::Note: inner_->Note(reinterpret_cast<const char*>(data)); break;
            case Op::PausePicture: inner_->PausePicture(c.a != 0, c.s1); break;
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
    // frame generation: while keeping_, the frame's batches run so far (the one running is not among them) and the
    // state it started with; frame_over_ once its Present ran
    std::vector<Batch*> frame_;
    State frame_start_, replay_state_;
    bool keeping_ = false, frame_over_ = false;
    bool motion_ = false;  // the backend wants motion vectors for the frame's pictures (Backend::Motion)
    // ReplayHud: the Present's batch while the backend presents, and each picture slot's time
    const Batch* hud_last_ = nullptr;
    float hud_t_[Backend::kMostPictures + 1] = {};
    // ReplayHud's walk to the copy, once a Present: the frame's draws counted, and the state at draw from with where
    // that draw is (batch: frame_'s index, frame_.size() for hud_last_; cmd: its index there), for the other pictures
    struct HudStart {
        bool counted = false, ok = false;
        uint32_t draws = 0, from = 0;
        size_t batch = 0, cmd = 0;
        State st;
    };
    HudStart hud_start_;
    // the freeze: holding_, the frame is kept whole (frame generation's or g_freeze at its start); the last frame kept
    // past its Present while frozen, the state it started with and its front buffer; DrawFrozen drew it
    bool holding_ = false;
    std::vector<Batch*> held_frame_;
    State held_start_;
    Surface last_front_{}, held_front_{};
    std::atomic<bool> refreshed_{false};
    uint32_t frame_draws_ = 0;
    uint64_t gen_match_ns_ = 0, gen_replay_ns_ = 0, gen_blend_ns_ = 0;  // XBE_PERF, over gen_frames_
    // inside blending: the draws blended, interp::Blend's part; interp::Undo's putting back after each draw
    uint64_t gen_blended_ = 0, gen_blend_how_ns_ = 0, gen_undo_ns_ = 0;
    uint64_t gen_matched_ns_ = 0, gen_draw_ns_ = 0, gen_between_ns_ = 0;  // Match alone, backend draws, Between
    uint64_t gen_draws_ = 0, gen_backend_draws_ = 0, gen_fast_ = 0, gen_reused_ = 0, gen_redrawn_ = 0;
    uint64_t gen_changed_ = 0, gen_unkept_ = 0, gen_refused_ = 0, gen_textures_ = 0;
    uint64_t gen_motion_ns_ = 0, gen_mslow_ = 0, gen_mreused_ = 0, gen_mskipped_ = 0;  // Motion's time, its draws
    uint64_t gen_motion_at_ns_ = 0;                  // Motion's constants at the two times (interp::At)
    double gen_mslow_ms_ = 0, gen_mreused_ms_ = 0;  // the backend's time for its draws by kind
    uint64_t gen_rec_wait_ns_ = 0;  // the Present's wait for the recorder
    uint32_t gen_frames_ = 0;
    uint32_t gen_light_ = 0;
    float shown_t_ = 1.0f;  // the time of the last picture shown, in its frame (Present)  // of them, drawn light (Backend::Light)
    size_t exec_at_ = 0;  // the command of the batch Execute runs
    size_t rec_end_ = 0;  // the batch running: its commands before this one are the recorder's (RecordAside)

    // the recorder (RecordAside): its jobs, a batch and how many of its commands; busy from a post to the last done
    std::mutex rec_m_;
    std::condition_variable rec_cv_, rec_idle_cv_;
    std::deque<std::pair<const Batch*, size_t>> rec_jobs_;
    bool rec_busy_ = false, rec_stop_ = false, rec_exited_ = false;
    State rec_state_;          // the recorder's, as state_ is the render thread's
    bool rec_changed_ = true;  // as changed_
    std::atomic<uint64_t> rec_ns_{0};

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    size_t batches_made_ = 0;  // under m_, for XBE_TRACE_MEM
    size_t pooled_bytes_ = 0;  // under m_: the room free_'s batches keep for their bytes (kPoolBytes)
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
