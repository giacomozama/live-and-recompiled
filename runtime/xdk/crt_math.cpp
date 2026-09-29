// The CRT's x87 acos and asin check the argument in one call and branch on the ZF it returns in the next.
// Recompiled code does not carry flags across call and ret, so they read stale flags and returned NaN at
// random, which pinned look-at heads to their limit and twisted skinned bones. Computed on the host instead.
#include <cmath>

#include "xdk/hle.h"

X86_FUNC(GUEST(_CIacos)) {
    (void)base;
    X87_ST(0) = std::acos(X87_ST(0));
    ctx.esp += 4;
}

X86_FUNC(GUEST(_CIasin)) {
    (void)base;
    X87_ST(0) = std::asin(X87_ST(0));
    ctx.esp += 4;
}
