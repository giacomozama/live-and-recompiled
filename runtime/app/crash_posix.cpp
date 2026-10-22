// The crash report everywhere but Windows (app/crash.cpp): the signal and where it hit, the host stack and every guest
// thread, to the log. `addr2line -fiC -e Game <address>` names a host frame's line.
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <typeinfo>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {

std::atomic<bool> g_reporting{false};

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "segmentation fault";
        case SIGBUS: return "bus error";
        case SIGILL: return "illegal instruction";
        case SIGFPE: return "arithmetic exception";
        case SIGABRT: return "abort";
        case SIGTRAP: return "trap (x86_trap or __builtin_trap)";
        default: return "signal";
    }
}

// one report per run: a fault inside the report ends it
void Report(const char* what) {
    if (g_reporting.exchange(true)) return;
    LOGE("crash: ==== %s ====", what);
    LOGE("crash: host stack");
    void* frames[64];
    int n = backtrace(frames, 64);
    if (char** names = backtrace_symbols(frames, n)) {
        for (int i = 0; i < n; i++) LOGE("crash:   %s", names[i]);
        std::free(names);
    }
    // the recompiled code keeps guest registers in host ones between calls: a context can lag its thread
    for (auto& t : kernel::Threads()) {
        if (!t->ctx) continue;
        LOGE("crash: guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 32);
    }
    LOGE("crash: ==== end of report ====");
    logx::Flush();
}

void OnSignal(int sig, siginfo_t* info, void* uc) {
    char what[160];
    unsigned long long ip = 0;
#if defined(__x86_64__) && defined(__GLIBC__)
    ip = static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
#endif
    std::snprintf(what, sizeof(what), "%s at %016llX, address %p", SignalName(sig), ip, info ? info->si_addr : nullptr);
    Report(what);
    // the default action after: a core dump where the system keeps them
    signal(sig, SIG_DFL);
    raise(sig);
}

void OnTerminate() {
    const char* what = "std::terminate";
    char buf[256];
    if (auto p = std::current_exception()) {
        try {
            std::rethrow_exception(p);
        } catch (const std::exception& x) {
            std::snprintf(buf, sizeof(buf), "std::terminate, uncaught %s: %s", typeid(x).name(), x.what());
            what = buf;
        } catch (...) {
            what = "std::terminate, uncaught exception of a non-std type (an X86Resume escaping its function?)";
        }
    }
    Report(what);
    std::_Exit(3);
}

}  // namespace

void InstallCrashHandler() {
    // room for the report when the main thread's stack itself overflowed
    static char alt[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(sig, &sa, nullptr);
    std::set_terminate(OnTerminate);
}

}  // namespace app
