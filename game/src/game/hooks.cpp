// Conker hooks. The engine was built to squeeze the original Xbox, so it calls the D3D8 library's internals
// directly (LTCG inlining) and submits raw NV2A pushbuffers. The runtime already decodes those pushbuffers
// (d3d::Walk) and implements the D3D8 API at a high level, so the only internals that still need replacing
// are the ones that drive the hardware directly (the per-frame buffer clear, which reads the miniport).
//
// A strong X86_FUNC replaces the recompiled function at that address.
#include <cstdlib>
#include <cstring>

#include "xdk/hle.h"
#include "cpu/x86_ops.h"
#include "core/diag.h"
#include "xdk/d3d8.h"

namespace {

// device globals Conker's D3D8LTCG uses (see config/game.toml [data])
constexpr uint32_t kD3D_pDevice = 0x5499E8;
constexpr uint32_t kDevBackBuffer = 0x1A14;  // CDevice_BackBuffer
constexpr uint32_t kDevAutoDepth = 0x1A20;   // CDevice_AutoDepth

}  // namespace

// 0x4E2801: the XMV movie decoder's per-frame update. The transition manager treats a negative return as a
// failed movie: its open loop (0x252470) leaves and its playing state (0x2529xx) closes and releases the
// decoder. With XBE_SKIP_FMV set it always reports that, so the intro FMVs never decode in real time.
X86_EXTERN_FUNC(__imp__sub_004E2801);
X86_FUNC(sub_004E2801) {
    if (diag::SkipFmv()) {
        kernel::Return(ctx, 0xFFFFFFFFu, 4);  // stdcall: the callee pops the 4 arguments
        return;
    }
    __imp__sub_004E2801(ctx, base);
}

// 0x4FF00: may LT end the playing in-engine cutscene (edi its slot)? The title allows it only for cutscenes the
// player has seen (0x4EBE0, a bit per cutscene at 0x765608), never for the intro's first one (mode 0x18, cutscene
// 1), in mode 0x1D (the intro's later scenes) only cutscene 5, and not in the last half second.
// XBE_SKIPPABLE_CUTSCENES set: any cutscene, seen or not, still not in the last half second. Mode 0x21 skips on a
// button of its own and keeps its test.
X86_EXTERN_FUNC(__imp__sub_0004FF00);
X86_FUNC(sub_0004FF00) {
    static const bool skippable = std::getenv("XBE_SKIPPABLE_CUTSCENES") != nullptr;
    using kernel::R32;
    uint32_t mode = R32(0x7FA1F8);
    if (skippable && mode != 0x21) {
        // the player whose skip button counts, its pressed buttons at +0xA0, and the "no skipping" flag
        uint32_t player = 0x763260 + (int8_t(base[0x6BCFEB]) > 0 ? 0 : 0x14 * 0x1B0);
        bool pressed = R32(player + 0xA0) != 0 && !base[0x7655F7];
        float fps;
        std::memcpy(&fps, base + 0x7F9F5C, 4);
        int32_t now = int32_t(R32(0x76F0F8 + ctx.edi * 4)), end = int32_t(R32(0x76F0E8 + ctx.edi * 4));
        kernel::Return(ctx, pressed && now + int32_t(fps) / 2 < end ? 1 : 0, 0);
        return;
    }
    __imp__sub_0004FF00(ctx, base);
}

// 0x4C950: the controller layer's init. It first opens the pads XGetDevices reports (0x3D19C0, which marks each
// one inserted at byte 0x13C of its 0x140-byte record in the table at [0x806FD8]), then replaces that table with a
// zeroed one. The stick filter (0x4CF80) zeroes both sticks of a pad not marked inserted, while buttons pass. On
// the console the pads enumerate later and arrive through XGetDeviceChanges into the new table; the runtime has
// them present from XInitDevices on, so mark the pads that were opened (handles at 0x806A48, record slot per port
// at 0x80692C) in the new table.
X86_EXTERN_FUNC(__imp__sub_0004C950);
X86_FUNC(sub_0004C950) {
    __imp__sub_0004C950(ctx, base);
    using kernel::R32;
    for (uint32_t port = 0; port < 4; port++)
        if (R32(0x806A48 + port * 4)) base[R32(0x806FD8) + base[0x80692C + port] * 0x140 + 0x13C] = 1;
}

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

// 0x4C3F2F: a DSound buffer helper (buffer, flag). It reads a sub-object the library's buffer constructor
// allocates at [buffer+0x10] and sets or clears bit 0 of its byte 0xb. The runtime keeps DSound objects
// host-side (dsound.cpp's Voice map), so a buffer made by its HLE has no such sub-object, and the engine's
// XACT audio path calls this through library code the runtime does not replace. There is nothing to toggle.
X86_FUNC(sub_004C3F2F) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// 0x4C3DFE: the shared body of the library's two voice-descriptor setters (buffer 0x4C51B8 and stream
// 0x4C5235 both reach it, neither is an ordinal import, so the signature scan never named them). It stores a
// descriptor through the console voice's [voice+0xb8] field, which the runtime's host objects do not build.
// The host mixer recomputes voice state from the API calls it HLEs, so the console write is skipped.
X86_FUNC(sub_004C3DFE) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// 0x4C5D4C (reached directly, and through the 0x4C606D export thunk): the library's 3D-parameter apply. It
// pushes a voice's position/velocity/orientation into the console DSP through function pointers the library's
// device init installs at 0x4E1D1C / 0x4E1D28. The runtime HLEs DirectSoundCreate and never runs that init, so
// those pointers stay null and the call traps. The host mixer derives 3D attenuation from the voice's own
// SetPosition/3D state, so the console write is skipped, like the other voice-register paths above.
X86_FUNC(sub_004C5D4C) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// The listener's 3D parameter helpers (0x4C4613 and 0x4C9C86 reach them): each tail forwards to a DSP
// function pointer the library's skipped device init installs in the 0x4E1D20..0x4E1D40 table, so calling it
// traps on null. They are console voice writes the host mixer does not need; skip them.
X86_FUNC(sub_004C37C1) { (void)base; kernel::Return(ctx, 0, 8); }
X86_FUNC(sub_004C37EA) { (void)base; kernel::Return(ctx, 0, 8); }
X86_FUNC(sub_004C3821) { (void)base; kernel::Return(ctx, 0, 5); }
X86_FUNC(sub_004C3841) { (void)base; kernel::Return(ctx, 0, 7); }
X86_FUNC(sub_004C3875) { (void)base; kernel::Return(ctx, 0, 4); }
X86_FUNC(sub_004C3897) { (void)base; kernel::Return(ctx, 0, 4); }
X86_FUNC(sub_004C9C86) { (void)base; kernel::Return(ctx, 0, 4); }

// 0x543170: the library's deferred state flush, which re-pushes hardware state for the dirty flags it finds.
// The runtime translates the D3D8 API directly and decodes the engine's own pushbuffer writes (d3d::Walk), so
// this path only reads guest texture/sampler structures the library would have kept. Skip it and leave the
// dirty bits it did not handle, exactly as its own tail did.
X86_FUNC(sub_00543170) {
    (void)base;
    // what the skipped sub-flushes would have pushed still has to win over the engine's raw writes (fog, the fixed
    // function combiner); the runtime models that from the dirty bits
    d3d::DeferredFlush(kernel::R32(0x5499F0));
    kernel::W32(0x5499F0, kernel::R32(0x5499F0) & 0xC0000070u);
    kernel::Return(ctx, 0, 0);
}
