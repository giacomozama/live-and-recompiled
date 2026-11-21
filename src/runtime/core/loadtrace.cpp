#include "core/loadtrace.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <time.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
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

namespace {

// the process's memory as the system counts it, MB: resident, of which anonymous (heap and mappings with no file
// behind them, where a host leak grows), its threads and open files (Linux); resident and committed private memory
// and handles (Windows)
struct Process {
    double resident = 0, anon = 0;
    long threads = 0, files = 0;
};

Process ReadProcess() {
    Process p;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
        p.resident = double(pm.WorkingSetSize) / 1048576.0;
        p.anon = double(pm.PrivateUsage) / 1048576.0;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) p.files = long(handles);
#else
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        long kb;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) p.resident = double(kb) / 1024.0;
            else if (std::sscanf(line, "RssAnon: %ld kB", &kb) == 1) p.anon = double(kb) / 1024.0;
            else std::sscanf(line, "Threads: %ld", &p.threads);
        }
        std::fclose(f);
    }
    if (DIR* d = opendir("/proc/self/fd")) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') p.files++;
        closedir(d);
    }
#endif
    return p;
}

}  // namespace

bool MemOn() {
    static const bool on = diag::Env("XBE_TRACE_MEM") != nullptr;
    return on;
}

bool MemFrame(uint32_t frame, double ms) {
    using clock = std::chrono::steady_clock;
    static clock::time_point due = clock::now() + std::chrono::seconds(10);
    static uint32_t loads = 0;
    auto now = clock::now();
    // at the load's end, and 10 s on (the level settled: its streaming and lightmaps done) unless another load came
    // first: a quick warp loop still gives a line per load, each at the same point
    const char* when;
    if (ms > 250 && frame > 2) {
        loads++;
        due = now + std::chrono::seconds(10);
        when = "load end";
    } else {
        if (now < due) return false;
        due = now + std::chrono::minutes(2);
        when = "settled";
    }
    Process p = ReadProcess();
    mem::Stats s = mem::Query();
    kernel::ObjectCounts o = kernel::CountObjects();
    constexpr double kPageMb = 4096.0 / 1048576.0;
    LOGR("mem: load %u %s (frame %u): process resident %.0f MB, anonymous %.0f MB (Windows: committed private), "
         "threads %ld, files %ld (Windows: handles)",
         loads, when, frame, p.resident, p.anon, p.threads, p.files);
    LOGR("mem:  console contiguous %.1f MB, system %.1f MB, virtual committed %.1f MB, free %.1f MB; kernel handles "
         "%zu, objects by address %zu, names %zu (%zu gone)",
         s.contiguous_used * kPageMb, s.system_used * kPageMb, s.virtual_committed * kPageMb,
         s.available_pages * kPageMb, o.handles, o.by_pointer, o.names, o.names_dead);
    return true;
}

}  // namespace loadtrace
