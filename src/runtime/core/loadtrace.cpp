#include "core/loadtrace.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/memory.h"

namespace loadtrace {
namespace {

// CPU time of the calling thread, ns
uint64_t ThreadCpuNs() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 100; };
    return ft(k) + ft(u);
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

double Ms(Kind k) { return double(g[k].ns.load()) / 1e6; }
unsigned long long N(Kind k) { return g[k].n.load(); }
double Mb(Kind k) { return double(g[k].bytes.load()) / 1048576.0; }

}  // namespace

void Frame(uint32_t frame, double ms) {
    g_main = true;
    static uint64_t cpu_last = ThreadCpuNs();
    uint64_t cpu = ThreadCpuNs(), cpu_ns = cpu - cpu_last;
    cpu_last = cpu;
    if (ms > 250 && frame > 2) {
        LOGR("load: frame %u took %.0f ms, presenting thread CPU %.0f ms", frame, ms, double(cpu_ns) / 1e6);
        LOGR("load:  reads %llu %.1f MB %.0f ms (presenting thread %llu %.1f MB %.0f ms), opens %llu %.0f ms "
             "(presenting thread %llu %.0f ms)",
             N(kRead), Mb(kRead), Ms(kRead), N(kReadMain), Mb(kReadMain), Ms(kReadMain), N(kOpen), Ms(kOpen),
             N(kOpenMain), Ms(kOpenMain));
        LOGR("load:  presenting thread waits %llu %.0f ms, sleeps %llu %.0f ms, yields %llu %.0f ms, vblank waits %llu "
             "%.0f ms",
             N(kWait), Ms(kWait), N(kDelay), Ms(kDelay), N(kYield), Ms(kYield), N(kVblank), Ms(kVblank));
        mem::Counts c = mem::CountRanges();
        LOGR("load:  allocations %llu %.0f ms (presenting thread %llu %.0f ms), free pieces contiguous %zu system %zu "
             "user %zu, reservations %zu",
             N(kAlloc), Ms(kAlloc), N(kAllocMain), Ms(kAllocMain), c.contiguous_free, c.system_free, c.user_free,
             c.reserved);
        LOGR("load:  loading screen draws %llu %.0f ms, their presents %llu %.0f ms; title hooks %llu %.0f ms",
             N(kScreen), Ms(kScreen), N(kShowFront), Ms(kShowFront), N(kGuest), Ms(kGuest));
        // the host functions the presenting thread spent the most in (nested ones counted in each)
        std::vector<Named*> named;
        for (Named* f = g_named.load(std::memory_order_acquire); f; f = f->next)
            if (f->n.load()) named.push_back(f);
        std::sort(named.begin(), named.end(), [](Named* x, Named* y) { return x->ns.load() > y->ns.load(); });
        std::string line;
        for (size_t i = 0; i < named.size() && i < 12; i++) {
            const char* name = named[i]->name;
            if (const char* u = std::strchr(name, '_')) name = u + 1;  // hle_, kimpl_
            char t[128];
            std::snprintf(t, sizeof t, "%s%s %llu %.0f ms", i ? ", " : "", name,
                          (unsigned long long)named[i]->n.load(), double(named[i]->ns.load()) / 1e6);
            line += t;
        }
        if (!line.empty()) LOGR("load:  host functions %s", line.c_str());
    }
    for (Named* f = g_named.load(std::memory_order_acquire); f; f = f->next) {
        f->ns = 0;
        f->n = 0;
    }
    for (Counter& k : g) {
        k.ns = 0;
        k.n = 0;
        k.bytes = 0;
    }
}

}  // namespace loadtrace
