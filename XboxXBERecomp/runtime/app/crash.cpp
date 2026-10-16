// the crash report: what the host faulted on, in guest terms, to the log; and a minidump in CrashDumps beside the exe
//
// Host addresses are logged as "Game.exe+RVA [VA]": the VA is the one the unrelocated exe has, so
// `llvm-addr2line -fiC -e Game.exe VA` (or tools/crash-symbolize.sh on the log) names the source line. A frame in a
// recompiled function also names the guest function, sub_XXXXXXXX in generated/*/x86_recomp.*.cpp.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <typeinfo>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace dispatch {
uint32_t GuestAt(uintptr_t host, bool exact);
bool LogTrail(const X86Context& ctx);
bool TrailOn();
}  // namespace dispatch

namespace app {

namespace {

std::atomic<DWORD> g_reporting{0};  // the host thread writing a report

const char* CodeName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_IN_PAGE_ERROR: return "in page error";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_STACK_OVERFLOW: return "host stack overflow";
        case EXCEPTION_BREAKPOINT: return "breakpoint (x86_trap or __debugbreak)";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case 0xE06D7363: return "C++ exception (MSVC)";
        case 0x20474343: return "C++ exception (GCC)";
        case 0xC0000409: return "stack buffer overrun / fail fast";
        case 0x40000015: return "abort";
        default: return "exception";
    }
}

// "Game.exe+RVA [VA]" or "module+offset", and the guest function it is in
void Where(uintptr_t pc, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &mod)) {
        std::snprintf(out, n, "%p (no module)", reinterpret_cast<void*>(pc));
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    uintptr_t rva = pc - reinterpret_cast<uintptr_t>(mod);
    if (mod != GetModuleHandleW(nullptr)) {
        std::snprintf(out, n, "%ls+%llX", name, (unsigned long long)rva);
        return;
    }
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(mod) + dos->e_lfanew);
    unsigned long long va = nt->OptionalHeader.ImageBase + rva;
    // the recompiled function: the .pdata entry's start is exactly a mapped body, else the nearest one below
    DWORD64 image = 0;
    uint32_t guest = 0;
    bool exact = false;
    if (auto* fe = RtlLookupFunctionEntry(pc, &image, nullptr)) {
        guest = dispatch::GuestAt(uintptr_t(image + fe->BeginAddress), true);
        exact = guest != 0;
    }
    if (!guest) guest = dispatch::GuestAt(pc, false);
    if (guest && exact) std::snprintf(out, n, "%ls+%llX [%llX] in guest sub_%08X", name, (unsigned long long)rva, va, guest);
    else if (guest) std::snprintf(out, n, "%ls+%llX [%llX] (runtime, or near guest sub_%08X)", name, (unsigned long long)rva, va, guest);
    else std::snprintf(out, n, "%ls+%llX [%llX]", name, (unsigned long long)rva, va);
}

// a host value inside the guest's 4 GB window is a guest address
void Guestish(uint64_t v, char* out, size_t n) {
    uint64_t b = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
    if (b && v >= b && v - b < (1ull << 32)) {
        uint32_t g = uint32_t(v - b);
        std::snprintf(out, n, " =g%08X%s", g, mem::IsCommitted(g) ? "" : "(uncommitted)");
    } else {
        out[0] = 0;
    }
}

void LogHostRegisters(const CONTEXT& c) {
    struct {
        const char* n;
        DWORD64 v;
    } r[] = {{"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi}, {"rdi", c.Rdi},
             {"rbp", c.Rbp}, {"rsp", c.Rsp}, {"r8", c.R8},   {"r9", c.R9},   {"r10", c.R10}, {"r11", c.R11},
             {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15}};
    for (size_t i = 0; i < std::size(r); i += 2) {
        char a[40], b[40];
        Guestish(r[i].v, a, sizeof(a));
        Guestish(r[i + 1].v, b, sizeof(b));
        LOGE("crash:   %-3s %016llX%-24s %-3s %016llX%s", r[i].n, (unsigned long long)r[i].v, a, r[i + 1].n,
             (unsigned long long)r[i + 1].v, b);
    }
}

// the host call chain: with the recompiled functions calling each other directly, it is the guest's call chain too
void LogHostStack(CONTEXT c) {
    for (int i = 0; i < 64 && c.Rip; i++) {
        char w[160];
        Where(uintptr_t(c.Rip), w, sizeof(w));
        LOGE("crash:   #%02d %s", i, w);
        DWORD64 image = 0;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fe) {  // a leaf: the return address is on top
            if (!c.Rsp || IsBadReadPtr(reinterpret_cast<void*>(c.Rsp), 8)) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &frame, nullptr);
    }
}

void LogGuestThread(kernel::Thread& t, bool current) {
    X86Context& g = *t.ctx;
    LOGE("crash: guest thread %X%s, last indirect call %08X", t.id, current ? " (crashed)" : "", g.last_icall);
    kernel::LogGuestStack(g, current ? 256 : 48);
    if (current) {
        // the raw stack, for the arguments the return addresses above were called with
        for (int i = 0; i < 32; i += 8) {
            uint32_t a = g.esp + uint32_t(i) * 4;
            if (!mem::IsCommitted(a) || !mem::IsCommitted(a + 31)) break;
            LOGE("crash:   %08X: %08X %08X %08X %08X %08X %08X %08X %08X", a, kernel::R32(a), kernel::R32(a + 4),
                 kernel::R32(a + 8), kernel::R32(a + 12), kernel::R32(a + 16), kernel::R32(a + 20), kernel::R32(a + 24),
                 kernel::R32(a + 28));
        }
    }
    if (dispatch::TrailOn()) {
        LOGE("crash:   trail, newest last:");
        if (!dispatch::LogTrail(g)) LOGE("crash:     (none)");
    }
}

void WriteDump(EXCEPTION_POINTERS* e) {
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    LOGE("crash: minidump CrashDumps\\%ls", name);
}

// why: a line on what ended the process; e: the exception, null for an abort or terminate (the stack is taken here)
void Report(const char* why, EXCEPTION_POINTERS* e) {
    DWORD self = GetCurrentThreadId(), none = 0;
    if (!g_reporting.compare_exchange_strong(none, self)) {
        // another thread is reporting (or this one crashed inside the report): let it finish
        if (none != self) Sleep(INFINITE);
        return;
    }
    kernel::Thread* cur = kernel::Current();
    CONTEXT here{};
    const CONTEXT* c = e ? e->ContextRecord : nullptr;
    if (!c) {
        RtlCaptureContext(&here);
        c = &here;
    }
    LOGE("crash: ==== %s on host thread %lu, %s ====", why, self, cur ? "a guest thread" : "a host thread");
    if (e) {
        auto* r = e->ExceptionRecord;
        char w[160];
        Where(uintptr_t(r->ExceptionAddress), w, sizeof(w));
        LOGE("crash: %s (%08lX) at %s", CodeName(r->ExceptionCode), r->ExceptionCode, w);
        if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1];
            char g[40];
            Guestish(va, g, sizeof(g));
            const char* op = r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read";
            LOGE("crash: %s of %016llX%s", op, (unsigned long long)va, g[0] ? g : " (outside guest memory)");
        }
    }
    LOGE("crash: host registers (=g: points into guest memory)");
    LogHostRegisters(*c);
    LOGE("crash: host stack");
    LogHostStack(*c);
    // the recompiled code keeps guest registers in host ones between calls: ctx can lag the faulting instruction
    if (cur && cur->ctx) LogGuestThread(*cur, true);
    for (auto& t : kernel::Threads())
        if (t.get() != cur && t->ctx) LogGuestThread(*t, false);
    logx::Flush();
    WriteDump(e);
    LOGE("crash: ==== end of report ====");
    logx::Flush();
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* e) {
    Report("unhandled exception", e);
    return EXCEPTION_CONTINUE_SEARCH;
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
    Report(what, nullptr);
    std::_Exit(3);
}

}  // namespace

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(OnUnhandled);
    std::signal(SIGABRT, [](int) { Report("abort", nullptr); });
    std::set_terminate(OnTerminate);
    // room for the report to run in when the host stack itself overflowed
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

}  // namespace app
