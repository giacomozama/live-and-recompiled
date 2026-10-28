// What NVIDIA's NGX library for Windows (nvsdk_ngx_d.lib: Microsoft's compiler, its C runtime as a DLL) takes from
// Microsoft's C++ runtime and a MinGW build has not: given here over the Universal CRT both share. The rest it takes
// (the CRT's functions, C++ exception dispatch, __CxxFrameHandler3, _CxxThrowException) the UCRT exports itself, and the
// libraries its objects name by default (MSVCRT, OLDNAMES, msvcprt, uuid) are empty stand-ins (cmake/Dlss.cmake).
#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstdlib>

extern "C" {

// a program using floating point says so
int _fltused = 0x9875;

// /GS: the stack cookie, checked as functions return; the unwinding handlers of functions with a cookie, which only
// pass the exception on (the cookie is checked on the normal path alone)
uintptr_t __security_cookie = 0x2B992DDFA232ull;

void __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD, void*, PCONTEXT, void*) { return ExceptionContinueSearch; }
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(PEXCEPTION_RECORD, void*, PCONTEXT, void*) { return ExceptionContinueSearch; }

// control flow guard's indirect call: straight to the target in rax, unchecked
void XbeGuardDispatch() __asm__("xbe_guard_dispatch");
__asm__(".text\n.globl xbe_guard_dispatch\nxbe_guard_dispatch:\n    jmp *%rax\n");
void* __guard_dispatch_icall_dummy = reinterpret_cast<void*>(&XbeGuardDispatch);

// /Zc:threadSafeInit: a function's static is guarded by an int, 0 until a thread starts its initialiser, -1 while one
// runs it, then the epoch it was done in. A thread enters the header only for a guard past the last epoch it saw
__thread int _Init_thread_epoch = INT_MIN;

}  // extern "C"

namespace {

SRWLOCK g_init_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_init_done = CONDITION_VARIABLE_INIT;
int g_init_epoch = INT_MIN;

[[noreturn]] void Fail() { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }

}  // namespace

extern "C" {

// the guard -1 for the caller to initialise, or as another thread left it once it is done
void _Init_thread_header(int* guard) {
    AcquireSRWLockExclusive(&g_init_lock);
    while (*guard == -1) SleepConditionVariableSRW(&g_init_done, &g_init_lock, INFINITE, 0);
    if (*guard == 0) *guard = -1;
    else _Init_thread_epoch = g_init_epoch;
    ReleaseSRWLockExclusive(&g_init_lock);
}

void _Init_thread_footer(int* guard) {
    AcquireSRWLockExclusive(&g_init_lock);
    *guard = ++g_init_epoch;
    _Init_thread_epoch = g_init_epoch;
    ReleaseSRWLockExclusive(&g_init_lock);
    WakeAllConditionVariable(&g_init_done);
}

void _Init_thread_abort(int* guard) {
    AcquireSRWLockExclusive(&g_init_lock);
    *guard = 0;
    ReleaseSRWLockExclusive(&g_init_lock);
    WakeAllConditionVariable(&g_init_done);
}

}  // extern "C"

// operator new and delete by Microsoft's names; std::_Xbad_alloc and _Xlength_error, imported from msvcp140 by the
// library, as the import table slots it reads them through
void* MsvcNew(size_t n) __asm__("??2@YAPEAX_K@Z");
void MsvcDelete(void* p) __asm__("??3@YAXPEAX@Z");
void* MsvcNew(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) Fail();
    return p;
}
void MsvcDelete(void* p) { std::free(p); }

[[noreturn]] void XBadAlloc() { Fail(); }
[[noreturn]] void XLengthError(const char*) { Fail(); }
extern "C" {
[[gnu::used]] void (*g_imp_xbad_alloc)() __asm__("__imp_?_Xbad_alloc@std@@YAXXZ") = XBadAlloc;
[[gnu::used]] void (*g_imp_xlength_error)(const char*) __asm__("__imp_?_Xlength_error@std@@YAXPEBD@Z") = XLengthError;
}
