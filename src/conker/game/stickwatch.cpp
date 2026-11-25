// XBE_STICK_WATCH=1: a read watchpoint on player 1's right stick, to find the code that turns each camera.
//
// The stick reaches each player's pad ([0x847104], filled by 0x4E0D0): steps of -63..63 at +0x18 (x) and +0x1C (y),
// floats at +0x28 and +0x2C. A first watch on the pad found only the input layer reading it (0x4CE80): its control
// scheme (0x4D470, or 0x4D6E0 for the others 0x4D1F0 picks) maps the pad into the player's actions, 0x1B0 bytes at
// 0x763260 + player * 0x1B0, the right stick at +0x14 (x step), +0x18 (y step, up negative), +0x118 and +0x11C (the
// same as floats). Those fields are what this watches. Rather than follow their readers by hand, it protects their
// page and lets each access fault: the handler notes the faulting host code (a
// recompiled guest function, sub_XXXXXXXX) when the access touches a stick field, opens the page and single-steps
// the access (the trap flag), and the trap closes the page again. Host stack frames above it are the guest calls that
// led there. Other threads touching the page while it is open are missed, which a search can live with.
//
// Use: in a level, F11, then turn the camera with the stick in each mode (third person, first person, a weapon
// out), F11 again. Each reader is logged once with its access count, fields and callers. Linux (the native build)
// only: the Windows build logs that it has no watch.
//
// XBE_STICK_WATCH_AT=7FA274,7FA275 (hex guest addresses, up to 8 bytes in one page): those bytes instead, writes
// only, and each write that changes one is logged in order with the bytes' values after it and its callers
// ("stickwatch: change"), up to 256. Found what sets the pause flag [0x7FA275].
#include "common/stickwatch.h"
#include "core/diag.h"

#if !defined(XBE_HEADLESS)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"

#if defined(__linux__)

#include <elf.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" char __executable_start;

namespace stickwatch {

namespace {

// player 1's actions, and the right stick's fields in them
constexpr uint32_t kActions = 0x763260;
constexpr uint32_t kFields[] = {0x14, 0x18, 0x118, 0x11C};
constexpr int kVkF11 = 0x7A;

// what is watched: guest addresses and lengths (the stick's fields, or XBE_STICK_WATCH_AT's bytes)
constexpr int kMaxWatch = 8;
uint32_t g_watch[kMaxWatch], g_len[kMaxWatch];
int g_watches = 0;
bool g_bytes = false;  // XBE_STICK_WATCH_AT: writes only, each change logged in order

constexpr int kDepth = 8;  // callers kept for each reader
struct Site {
    uintptr_t pc;  // offset in the executable
    uintptr_t callers[kDepth];
    uint32_t fields;  // bit per kFields entry touched
    int32_t offset;  // the guest address the access starts at
    bool write;
    std::atomic<uint32_t> count;
};
constexpr int kMaxSites = 512;
Site g_sites[kMaxSites];
std::atomic<int> g_site_count{0};
std::atomic_flag g_site_lock = ATOMIC_FLAG_INIT;

std::atomic<bool> g_armed{false};
uintptr_t g_page_lo = 0, g_page_hi = 0;  // host pages protected
thread_local bool t_stepping = false;
thread_local int t_site = -1;     // the write being stepped (g_bytes), its site
thread_local uint8_t t_before[kMaxWatch];

// g_bytes: the writes that changed a watched byte, in order
struct Change {
    int site;
    uint8_t before[kMaxWatch], after[kMaxWatch];
};
constexpr int kMaxChanges = 256;
Change g_changes[kMaxChanges];
std::atomic<int> g_change_count{0};
struct sigaction g_old_segv{}, g_old_trap{};

void Chain(const struct sigaction& old, int sig, siginfo_t* si, void* uc) {
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction) old.sa_sigaction(sig, si, uc);
        return;
    }
    if (old.sa_handler == SIG_IGN) return;
    if (old.sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);  // returning faults again and ends the process as it would have
        return;
    }
    old.sa_handler(sig);
}

int Note(uintptr_t pc, uint32_t fields, int32_t offset, bool write) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(&__executable_start);
    pc -= base;
    const int n = g_site_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_sites[i].pc == pc && g_sites[i].offset == offset && g_sites[i].write == write) {
            g_sites[i].fields |= fields;
            g_sites[i].count.fetch_add(1, std::memory_order_relaxed);
            return i;
        }
    while (g_site_lock.test_and_set(std::memory_order_acquire)) {
    }
    int i = g_site_count.load(std::memory_order_relaxed);
    if (i < kMaxSites) {
        Site& s = g_sites[i];
        s.pc = pc, s.fields = fields, s.offset = offset, s.write = write;
        s.count.store(1, std::memory_order_relaxed);
        void* frames[kDepth + 3] = {};
        // this handler, the signal trampoline, then the faulting function's callers
        const int got = backtrace(frames, kDepth + 3);
        for (int k = 0; k < kDepth; k++)
            s.callers[k] = k + 3 < got ? reinterpret_cast<uintptr_t>(frames[k + 3]) - base : 0;
        g_site_count.store(i + 1, std::memory_order_release);
    } else {
        i = -1;
    }
    g_site_lock.clear(std::memory_order_release);
    return i;
}

void OnSegv(int sig, siginfo_t* si, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
    if (!g_armed.load(std::memory_order_acquire) || a < g_page_lo || a >= g_page_hi) {
        Chain(g_old_segv, sig, si, ucv);
        return;
    }
    // an access of up to 16 bytes at a (a vector load or a copy's chunk)
    const uintptr_t mem_base = reinterpret_cast<uintptr_t>(mem::Base());
    uint32_t fields = 0;
    for (int i = 0; i < g_watches; i++) {
        const uintptr_t f = mem_base + g_watch[i];
        if (f + g_len[i] > a && f < a + 16) fields |= 1u << i;
    }
    const bool write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (fields && (write || !g_bytes)) {
        const int site = Note(uintptr_t(uc->uc_mcontext.gregs[REG_RIP]), fields, int32_t(a - mem_base), write);
        if (g_bytes) {
            t_site = site;
            for (int i = 0; i < g_watches; i++) t_before[i] = *reinterpret_cast<const uint8_t*>(mem_base + g_watch[i]);
        }
    }
    mprotect(reinterpret_cast<void*>(g_page_lo), g_page_hi - g_page_lo, PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap after the access
    t_stepping = true;
}

void OnTrap(int sig, siginfo_t* si, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    if (!t_stepping) {
        Chain(g_old_trap, sig, si, ucv);
        return;
    }
    t_stepping = false;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (t_site >= 0) {
        const uintptr_t mem_base = reinterpret_cast<uintptr_t>(mem::Base());
        uint8_t after[kMaxWatch];
        bool changed = false;
        for (int i = 0; i < g_watches; i++) {
            after[i] = *reinterpret_cast<const uint8_t*>(mem_base + g_watch[i]);
            changed |= after[i] != t_before[i];
        }
        const int n = changed ? g_change_count.fetch_add(1, std::memory_order_relaxed) : kMaxChanges;
        if (n < kMaxChanges) {
            Change& c = g_changes[n];
            c.site = t_site;
            std::memcpy(c.before, t_before, sizeof c.before);
            std::memcpy(c.after, after, sizeof c.after);
        }
        t_site = -1;
    }
    if (g_armed.load(std::memory_order_acquire))
        mprotect(reinterpret_cast<void*>(g_page_lo), g_page_hi - g_page_lo, PROT_NONE);
}

// the executable's own function symbols (the recompiled sub_XXXXXXXX keep theirs), for the log
struct Symbol {
    uintptr_t addr, size;
    std::string name;
};
std::vector<Symbol> LoadSymbols() {
    std::vector<Symbol> out;
    const int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return out;
    struct stat st{};
    fstat(fd, &st);
    void* map = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return out;
    const auto* bytes = static_cast<const uint8_t*>(map);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(bytes);
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(bytes + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        const auto* syms = reinterpret_cast<const Elf64_Sym*>(bytes + sh[i].sh_offset);
        const char* names = reinterpret_cast<const char*>(bytes + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Sym); k++)
            if (ELF64_ST_TYPE(syms[k].st_info) == STT_FUNC && syms[k].st_value)
                out.push_back({uintptr_t(syms[k].st_value), uintptr_t(syms[k].st_size), names + syms[k].st_name});
    }
    munmap(map, size_t(st.st_size));
    std::sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
    return out;
}

std::string Name(const std::vector<Symbol>& syms, uintptr_t off) {
    char buf[64];
    auto it = std::upper_bound(syms.begin(), syms.end(), off, [](uintptr_t v, const Symbol& s) { return v < s.addr; });
    if (it == syms.begin()) {
        std::snprintf(buf, sizeof buf, "+0x%zx", size_t(off));
        return buf;
    }
    --it;
    std::snprintf(buf, sizeof buf, "+0x%zx", size_t(off - it->addr));
    return it->name + buf;
}

std::string Bytes(const uint8_t* v) {
    std::string out;
    for (int i = 0; i < g_watches; i++) {
        char b[24];
        std::snprintf(b, sizeof b, " [%06X]=%u", g_watch[i], v[i]);
        out += b;
    }
    return out;
}

bool Arm() {
    g_watches = 0;
    g_bytes = false;
    if (const char* at = diag::Env("XBE_STICK_WATCH_AT"); at && *at) {
        g_bytes = true;
        for (const char* p = at; *p && g_watches < kMaxWatch;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) break;
            g_watch[g_watches] = uint32_t(v), g_len[g_watches] = 1, g_watches++;
            p = *end ? end + 1 : end;
        }
        if (!g_watches) {
            LOGR("stickwatch: XBE_STICK_WATCH_AT has no addresses");
            return false;
        }
        for (int i = 0; i < g_watches; i++)
            if (!mem::IsCommitted(g_watch[i])) return false;
    } else {
        if (!mem::IsCommitted(kActions) || !mem::IsCommitted(kActions + 0x1B0 - 1)) return false;
        for (uint32_t f : kFields) g_watch[g_watches] = kActions + f, g_len[g_watches] = 4, g_watches++;
    }
    static bool installed = false;
    if (!installed) {
        installed = true;
        void* warm[2];
        backtrace(warm, 2);  // loads the unwinder now, not inside the handler
        struct sigaction sa{};
        sa.sa_sigaction = OnSegv;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sa.sa_sigaction = OnTrap;
        sigaction(SIGTRAP, &sa, &g_old_trap);
    }
    g_site_count.store(0);
    g_change_count.store(0);
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    const uintptr_t mem_base = reinterpret_cast<uintptr_t>(mem::Base());
    uintptr_t lo = ~uintptr_t(0), hi = 0;
    for (int i = 0; i < g_watches; i++)
        lo = std::min(lo, mem_base + g_watch[i]), hi = std::max(hi, mem_base + g_watch[i] + g_len[i]);
    g_page_lo = lo & ~(page - 1);
    g_page_hi = (hi + page - 1) & ~(page - 1);
    g_armed.store(true, std::memory_order_release);
    mprotect(reinterpret_cast<void*>(g_page_lo), g_page_hi - g_page_lo, PROT_NONE);
    if (g_bytes) {
        uint8_t now[kMaxWatch];
        for (int i = 0; i < g_watches; i++) now[i] = mem::Base()[g_watch[i]];
        LOGR("stickwatch: watching writes to%s; F11 to stop", Bytes(now).c_str());
    } else {
        LOGR("stickwatch: watching player 1's right stick (actions %08X); turn the camera, F11 to stop", kActions);
    }
    return true;
}

void Disarm() {
    g_armed.store(false, std::memory_order_release);
    mprotect(reinterpret_cast<void*>(g_page_lo), g_page_hi - g_page_lo, PROT_READ | PROT_WRITE);
    const auto syms = LoadSymbols();
    const int n = g_site_count.load();
    std::vector<int> order(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) order[size_t(i)] = i;
    std::sort(order.begin(), order.end(), [](int a, int b) { return g_sites[a].count > g_sites[b].count; });
    auto callers_of = [&](const Site& s) {
        std::string callers;
        for (uintptr_t c : s.callers)
            if (c) callers += " < " + Name(syms, c);
        return callers;
    };
    LOGR("stickwatch: %d %s%s", n, g_bytes ? "writers" : "readers", n == kMaxSites ? " (full)" : "");
    for (int i : order) {
        const Site& s = g_sites[i];
        std::string fields;
        for (int f = 0; f < g_watches; f++)
            if (s.fields & (1u << f)) {
                char b[12];
                std::snprintf(b, sizeof b, " %06X", g_watch[f]);
                fields += b;
            }
        LOGR("stickwatch: %s x%u at %06X (%s ) %s%s", s.write ? "write" : "read", s.count.load(), s.offset,
             fields.c_str(), Name(syms, s.pc).c_str(), callers_of(s).c_str());
    }
    const int changes = std::min(g_change_count.load(), kMaxChanges);
    for (int i = 0; i < changes; i++) {
        const Change& c = g_changes[i];
        if (c.site < 0) continue;
        const Site& s = g_sites[c.site];
        LOGR("stickwatch: change %d:%s ->%s by %s%s", i, Bytes(c.before).c_str(), Bytes(c.after).c_str(),
             Name(syms, s.pc).c_str(), callers_of(s).c_str());
    }
    LOGR("stickwatch: stopped");
}

}  // namespace

void Tick() {
    static const bool enabled = [] {
        const char* e = diag::Env("XBE_STICK_WATCH");
        return e && *e && *e != '0';
    }();
    if (!enabled) return;
    LOG_FIRST(1, "stickwatch: on; F11 starts and stops the watch on player 1's right stick");
    static bool was = false;
    const bool down = app::KeyDown(kVkF11);
    const bool pressed = down && !was;
    was = down;
    if (!pressed) return;
    if (g_armed.load()) Disarm();
    else if (!Arm()) LOGR("stickwatch: player 1's actions are not in memory yet (start a level)");
}

}  // namespace stickwatch

#else  // no watch outside Linux

namespace stickwatch {

void Tick() {
    static const bool enabled = diag::Env("XBE_STICK_WATCH") != nullptr;
    if (enabled) LOG_FIRST(1, "stickwatch: the watch is Linux only (the native build)");
}

}  // namespace stickwatch

#endif

#else  // no watch on the headless harness

namespace stickwatch {

void Tick() {}

}  // namespace stickwatch

#endif
