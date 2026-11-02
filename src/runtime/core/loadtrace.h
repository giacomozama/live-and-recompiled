#pragma once
// XBE_TRACE_LOAD set: a frame that took over a quarter second (a level load) is logged with where its time went:
// file opens and reads, waits and sleeps, allocations, vertical blank waits, loading screen draws and their
// presents, and the CPU time of the thread that presents. Waits and sleeps count on that thread only (the audio
// and streaming threads wait all the time); reads, opens and allocations count on every thread, the presenting
// thread's share shown beside them.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace loadtrace {

inline bool On() {
    static const bool on = std::getenv("XBE_TRACE_LOAD") != nullptr;
    return on;
}

enum Kind {
    kRead,        // NtReadFile/Scatter, every thread
    kReadMain,    // the same on the presenting thread
    kOpen,        // NtCreateFile/NtOpenFile, every thread
    kOpenMain,
    kAlloc,       // kernel allocations and frees (contiguous, pool, virtual), every thread
    kAllocMain,
    kWait,        // waits on objects, presenting thread
    kDelay,       // KeDelayExecutionThread, presenting thread
    kYield,       // NtYieldExecution, presenting thread
    kVblank,      // D3DDevice_BlockUntilVerticalBlank, presenting thread
    kScreen,      // the loading screen draw (Conker 0x2ACA70), presents included
    kShowFront,   // presenting the front buffer the loading screen drew
    kGuest,       // a title hook's own share, named by the title
    kKinds
};

struct Counter {
    std::atomic<uint64_t> ns{0}, n{0}, bytes{0};
};
inline Counter g[kKinds];
inline thread_local bool g_main = false;  // set on the thread that presents

inline bool Main() { return g_main; }

inline uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void Add(Kind k, uint64_t ns, uint64_t bytes = 0) {
    g[k].ns.fetch_add(ns, std::memory_order_relaxed);
    g[k].n.fetch_add(1, std::memory_order_relaxed);
    if (bytes) g[k].bytes.fetch_add(bytes, std::memory_order_relaxed);
}

// times a scope into kind (and its main-thread twin when main_kind isn't kKinds); main_only skips other threads,
// kind kKinds times nothing
class Scope {
public:
    Scope(Kind k, Kind main_kind = kKinds, bool main_only = false)
        : k_(k), mk_(main_kind), on_(k != kKinds && On() && (!main_only || g_main)), t0_(on_ ? Now() : 0) {}
    ~Scope() {
        if (!on_) return;
        uint64_t ns = Now() - t0_;
        Add(k_, ns, bytes);
        if (mk_ != kKinds && g_main) Add(mk_, ns, bytes);
    }
    uint64_t bytes = 0;

private:
    Kind k_, mk_;
    bool on_;
    uint64_t t0_;
};

// called at each present with the time since the last: on the first call the calling thread becomes the presenting
// thread; a frame over 250 ms is logged with what the counters gathered, then they start over
void Frame(uint32_t frame, double ms);

}  // namespace loadtrace
