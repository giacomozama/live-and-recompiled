// The vignette: a screen overlay the game draws over the 3D picture each frame, in the levels and the pub, a
// 320 x 240 quad at the origin with two layers, a copy of the screen (texture FFFFFFFE) and the mask of the screen's
// edges (texture FE), in a dark grey (FF0F0F0F) that darkens the picture where the mask has alpha.
//
// 0x15CAF0 draws such an overlay (cdecl: its object first on the stack, called through the overlay types' tables at
// 0x587880 and on): the object keeps its type in byte +4C, its colour as B G R A in +4D to +50, its rectangle at +2C
// (x y w h, floats), its layer count at +68 and each layer's texture from +6C. With the setting off the hook skips
// that one overlay, and only while its colour is a neutral grey: one tinted (red, when hurt) is drawn as ever.
#include "common/vignette.h"

#include <cstdlib>
#include <cstring>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

bool vignette::g_on = true;

namespace {

float F32(uint32_t va) {
    const uint32_t bits = kernel::R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool IsVignette(uint32_t obj) {
    using kernel::R32;
    if (!obj || R32(obj + 0x68) != 2 || R32(obj + 0x6C) != 0xFFFFFFFE || R32(obj + 0x70) != 0xFE) return false;
    if (F32(obj + 0x2C) != 0 || F32(obj + 0x30) != 0 || F32(obj + 0x34) != 320 || F32(obj + 0x38) != 240) return false;
    const uint32_t colour = R32(obj + 0x4C) >> 8;  // B G R from +4D
    const uint32_t b = colour & 0xFF, g = colour >> 8 & 0xFF, r = colour >> 16 & 0xFF;
    return r == g && g == b;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0015CAF0);
X86_FUNC(sub_0015CAF0) {
    static const bool env_off = [] {
        const char* e = std::getenv("XBE_VIGNETTE");
        return e && *e == '0';
    }();
    if ((!vignette::g_on || env_off) && IsVignette(kernel::R32(ctx.esp + 4))) {
        kernel::Return(ctx, ctx.eax, 0);  // cdecl: the caller pops
        return;
    }
    __imp__sub_0015CAF0(ctx, base);
}
