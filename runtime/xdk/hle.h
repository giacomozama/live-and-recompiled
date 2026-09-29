#pragma once
#include "image_symbols.h"
#include "kernel/kernel.h"

// Symbols are named in config/<image>.toml; the recompiler writes their addresses to image_symbols.h
#define X86_CAT_(a, b) a##b
#define X86_CAT(a, b) X86_CAT_(a, b)
#define SYM(name) X86_CAT(0x, SYM_##name)                 // address
#define GUEST(name) X86_CAT(sub_, SYM_##name)             // the function, a host override if there is one
#define GUEST_ORIG(name) X86_CAT(__imp__sub_, SYM_##name)  // the recompiled body

// Host implementation of a guest function: the strong definition replaces the recompiled weak one.
#define HLE_STDCALL(name, nargs)                                          \
    static uint32_t hle_##name(X86Context& ctx, const ::kernel::Args& a); \
    X86_FUNC(GUEST(name)) {                                               \
        (void)base;                                                       \
        ::kernel::Args args{ctx};                                         \
        uint32_t r = hle_##name(ctx, args);                               \
        ::kernel::Return(ctx, r, nargs);                                  \
    }                                                                     \
    static uint32_t hle_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)

// __fastcall: first two arguments in ecx and edx, the rest on the stack
#define HLE_FASTCALL(name, stack_args)                                    \
    static uint32_t hle_##name(X86Context& ctx, const ::kernel::Args& a); \
    X86_FUNC(GUEST(name)) {                                               \
        (void)base;                                                       \
        ::kernel::Args args{ctx};                                         \
        uint32_t r = hle_##name(ctx, args);                               \
        ::kernel::Return(ctx, r, stack_args);                             \
    }                                                                     \
    static uint32_t hle_##name([[maybe_unused]] X86Context& ctx, [[maybe_unused]] const ::kernel::Args& a)

// __cdecl: the caller pops the arguments
#define HLE_CDECL(name) HLE_STDCALL(name, 0)

// float arguments passed on the stack
inline float ArgF(const ::kernel::Args& a, int i) {
    uint32_t v = a[i];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline float GF(uint32_t va) {
    float f;
    std::memcpy(&f, ::kernel::HostPtr(va), 4);
    return f;
}
