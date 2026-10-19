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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
constexpr uint32_t kBatchCommands = 64;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport, Note };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    Surface s1, s2;
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
uint64_t Ns(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner)
        : inner_(std::move(inner)), ranges_(StateRanges()), word_kinds_(WordKinds()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
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
        auto t = Clock::now();
        static const bool perf = diag::Perf();
        uint64_t tsc[6];
        if (perf) tsc[0] = __rdtsc();
        SendState(s, perf ? &tsc[1] : nullptr);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        cur_->cmds.push_back(c);
        if (perf) tsc[3] = __rdtsc();
        for (uint32_t tex : s.textures) Use(tex);
        for (uint32_t pal : s.palettes) Use(pal);
        for (const auto& st : s.streams) Use(st.vb);
        Use(s.rt.header);
        Use(s.zs.header);
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
        auto t = Clock::now();
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
        auto t = Clock::now();
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
        auto t = Clock::now();
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
    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        uint8_t now[kHeaderBytes] = {};
        std::memcpy(now, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
        Recent<HeaderCopy>& r = recent_headers_[(header >> 3) & 63];
        HeaderCopy& c = r.key == header ? *r.value : headers_[header];
        r = {header, &c};
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
        for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i]);
        Delta(&sent_.textures, &textures, sizeof(textures));
        std::array<uint32_t, 4> palettes;
        for (size_t i = 0; i < 4; i++) palettes[i] = Header(s.palettes[i]);
        Delta(&sent_.palettes, &palettes, sizeof(palettes));
        auto streams = s.streams;
        for (auto& st : streams) st.vb = Header(st.vb);
        Delta(&sent_.streams, &streams, sizeof(streams));
        Surface rt = s.rt, zs = s.zs;
        rt.header = Header(rt.header);
        zs.header = Header(zs.header);
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
    void Use(uint32_t header) {
        if (!header) return;
        Recent<uint64_t>& r = recent_uses_[(header >> 3) & 63];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
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
        auto t = Clock::now();
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
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            for (const Cmd& c : b->cmds) Execute(*b, c);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
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

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        // anything but state and draws may change what the backend holds (targets, frames): the next draw is new
        if (c.op != Op::State && c.op != Op::Draw) changed_ = true;
        switch (c.op) {
            case Op::State: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                for (const uint8_t* p = data; p < data + c.size;) {
                    uint32_t head[2];
                    std::memcpy(head, p, 8);
                    if (!changed_) changed_ = ChangesState(s + head[0], p + 8, head[0], head[1]);
                    std::memcpy(s + head[0], p + 8, head[1]);
                    p += 8 + head[1];
                }
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                if (c.vs >= 0 || c.ps >= 0) changed_ = true;
                break;
            }
            case Op::Draw:
                inner_->Changed(changed_);
                changed_ = false;
                inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: inner_->Present(c.s1); break;
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
    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;  // ring_pos_ after it was written
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    template <typename T>
    struct Recent {
        uint32_t key = 0;
        T* value = nullptr;
    };
    std::array<Recent<HeaderCopy>, 64> recent_headers_{};
    std::array<Recent<uint64_t>, 64> recent_uses_{};
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
