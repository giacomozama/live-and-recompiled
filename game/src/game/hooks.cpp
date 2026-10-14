// Conker hooks. The engine was built to squeeze the original Xbox, so it calls the D3D8 library's internals
// directly (LTCG inlining) and submits raw NV2A pushbuffers. The runtime already decodes those pushbuffers
// (d3d::Walk) and implements the D3D8 API at a high level, so the only internals that still need replacing
// are the ones that drive the hardware directly (the per-frame buffer clear, which reads the miniport).
//
// A strong X86_FUNC replaces the recompiled function at that address.
#include "xdk/hle.h"
#include "cpu/x86_ops.h"

namespace {

// device globals Conker's D3D8LTCG uses (see config/game.toml [data])
constexpr uint32_t kD3D_pDevice = 0x5499E8;
constexpr uint32_t kDevBackBuffer = 0x1A14;  // CDevice_BackBuffer
constexpr uint32_t kDevAutoDepth = 0x1A20;   // CDevice_AutoDepth

}  // namespace

// 0x538D50: begin a frame's render target and clear it. It called the library's pushbuffer submit/kickoff
// (0x53F0E0, 0x53F2F0) which read the miniport; do the same work through the host's D3D8 implementation.
// The library used SetRenderTarget(backbuffer, autoDepth) then Clear(target|depth, black, z=1).
X86_FUNC(sub_00538D50) {
    (void)base;
    uint32_t dev = kernel::R32(kD3D_pDevice);
    uint32_t rt = kernel::R32(dev + kDevBackBuffer);
    uint32_t zs = kernel::R32(dev + kDevAutoDepth);
    GUEST_CALL(D3DDevice_SetRenderTarget, ctx, {rt, zs});
    GUEST_CALL(D3DDevice_Clear, ctx, {0, 0, 3, 0, 0x3F800000u, 0});
    kernel::Return(ctx, 0, 0);
}

// 0x543170: the library's deferred state flush, which re-pushes hardware state for the dirty flags it finds.
// The runtime translates the D3D8 API directly and decodes the engine's own pushbuffer writes (d3d::Walk), so
// this path only reads guest texture/sampler structures the library would have kept. Skip it and leave the
// dirty bits it did not handle, exactly as its own tail did.
X86_FUNC(sub_00543170) {
    (void)base;
    kernel::W32(0x5499F0, kernel::R32(0x5499F0) & 0xC0000070u);
    kernel::Return(ctx, 0, 0);
}
