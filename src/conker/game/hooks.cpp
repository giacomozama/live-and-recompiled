// Conker hooks. The engine was built to squeeze the original Xbox, so it calls the D3D8 library's internals
// directly (LTCG inlining) and submits raw NV2A pushbuffers. The runtime already decodes those pushbuffers
// (d3d::Walk) and implements the D3D8 API at a high level, so the only internals that still need replacing
// are the ones that drive the hardware directly (the per-frame buffer clear, which reads the miniport).
//
// A strong X86_FUNC replaces the recompiled function at that address.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>

#include "xdk/hle.h"
#include "cpu/x86_ops.h"
#include "core/diag.h"
#include "xdk/d3d8.h"
#include "xdk/dsound.h"

#include "common/bulb.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/grass.h"

namespace {

// device globals Conker's D3D8LTCG uses (see src/conker/game.toml [data])
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

// 0x601D0: one step of the hard disk cache copier. The first launch copies wavebanks, packages and textures to Z:
// (0x35950 and its kind queue them at priority 3), and the intro won't take a skip (0x1F6C50) until that queue is
// empty. The file pump (0x2A37B0, once a frame) polls the async I/O (0x1F1360), and with none in flight (0x775F50)
// picks the next job (0x5FFE0) and runs this step, which moves 128 KB between double buffers: one step a frame,
// about 6 MB/s, so the copy took most of the intro FMVs. Keep stepping within a slice of the frame, completing this
// thread's reads at once (they land a moment later otherwise), until a step finds nothing to do or I/O is still
// out. XBE_CONSOLE_CACHE_COPY keeps the one step a frame.
X86_EXTERN_FUNC(__imp__sub_000601D0);
X86_FUNC(sub_000601D0) {
    static const bool console = std::getenv("XBE_CONSOLE_CACHE_COPY") != nullptr;
    static thread_local bool inner = false;
    if (console || inner) {
        __imp__sub_000601D0(ctx, base);
        return;
    }
    using kernel::R32;
    using clock = std::chrono::steady_clock;
    constexpr uint32_t kPollFiles = 0x1F1360, kFilesInFlight = 0x775F50, kQueue = 0x5F5F0, kNextJob = 0x5FFE0;
    const auto end = clock::now() + std::chrono::milliseconds(2);
    const uint32_t step = ctx.eax;  // the copier: its 0x40000 double buffer at +0, freed when idle
    inner = true;
    kernel::CallGuest(ctx, 0x601D0, {});
    const uint32_t queue = kernel::CallGuest(ctx, kQueue, {});
    while (clock::now() < end) {
        kernel::FlushPendingIo();
        kernel::CallGuest(ctx, kPollFiles, {});
        if (R32(kFilesInFlight) || base[queue + 0xC]) break;  // a read still out, or the queue paused
        const bool idle = !R32(step);
        const uint32_t esi = ctx.esi, edi = ctx.edi;
        ctx.edi = R32(queue);
        ctx.esi = R32(queue + 8);
        kernel::CallGuest(ctx, kNextJob, {});
        ctx.eax = step;
        kernel::CallGuest(ctx, 0x601D0, {});
        ctx.esi = esi, ctx.edi = edi;
        if (idle && !R32(step)) break;  // nothing under way and nothing picked up
    }
    inner = false;
    kernel::Return(ctx, ctx.eax, 0);
}

// 0x1F6C50: the intro FMVs' update (eax the intro, [intro] its state, 1 while a movie plays). It runs the file pump
// and takes a skip (A, B or Start: 0x4110 in the pressed bits at +8 of the pad record 0x4E020 returns) only once the
// cache copy queue is empty, so a press while the first launch's copy runs was lost and the movies looked
// unskippable. Remember that press and hand it back each frame until the intro takes it, then put the pad as it was
// so nothing else reads it a second time.
X86_EXTERN_FUNC(__imp__sub_001F6C50);
X86_FUNC(sub_001F6C50) {
    using kernel::R32;
    constexpr uint32_t kSkipButtons = 0x4110;
    static uint32_t intro = 0, held = 0;
    const uint32_t self = ctx.eax;
    if (self != intro) intro = self, held = 0;
    const uint32_t pad = kernel::CallGuest(ctx, 0x4E020, {1});
    const uint32_t pressed = R32(pad + 8);
    held |= pressed & kSkipButtons;
    kernel::W32(pad + 8, pressed | held);
    __imp__sub_001F6C50(ctx, base);
    kernel::W32(pad + 8, pressed);
    if (R32(self) != 1) held = 0;  // skipped, or the movies ended
}

// 0x4FF00: may LT end the playing in-engine cutscene (edi its slot)? The title allows it only for cutscenes the
// player has seen (0x4EBE0, a bit per cutscene at 0x765608), never for the intro's first one (mode 0x18, cutscene
// 1), in mode 0x1D (the intro's later scenes) only cutscene 5, and not in the last half second.
// cutscenes::g_skippable (or XBE_SKIPPABLE_CUTSCENES): any cutscene, seen or not, still not in the last half second. Mode 0x21 skips on a
// button of its own and keeps its test.
bool cutscenes::g_skippable = false;

X86_EXTERN_FUNC(__imp__sub_0004FF00);
X86_FUNC(sub_0004FF00) {
    static const bool env = std::getenv("XBE_SKIPPABLE_CUTSCENES") != nullptr;
    using kernel::R32;
    uint32_t mode = R32(0x7FA1F8);
    if ((env || cutscenes::g_skippable) && mode != 0x21) {
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

// 0x1A9540: draws one grass patch (a node renderer, [patch+0x5C] its fade). A patch at full strength binds the wind
// program (handle at 0x6B89A4: blades bend with time and away from the player, by (1 - v)^2 up the blade); a fading
// one binds a plain transform (0x6B89DC) and fades it with the texture factor and a scaled alpha reference. The two
// declarations read the same stride 12 vertices, the texture coordinate as v7 or v2, and share their constants, so
// grass::g_wind (or XBE_GRASS_WIND=0 off) hands the fading patches the wind program too: only the handle the
// fading path reads changes, for this call, and the rest of its fade stays as it is.
bool grass::g_wind = true;

X86_EXTERN_FUNC(__imp__sub_001A9540);
X86_FUNC(sub_001A9540) {
    static const char* env = std::getenv("XBE_GRASS_WIND");
    constexpr uint32_t kWind = 0x6B89A4, kFade = 0x6B89DC;
    uint32_t wind = kernel::R32(kWind), fade = kernel::R32(kFade);
    if (!(env ? env[0] != '0' : grass::g_wind) || !wind) {
        __imp__sub_001A9540(ctx, base);
        return;
    }
    kernel::W32(kFade, wind);
    __imp__sub_001A9540(ctx, base);
    kernel::W32(kFade, fade);
}

// The patch's detail, in 0x1A9540: d its distance from the player ([esp+0x20] at 0x1A95DD), past 2500 it isn't drawn;
// to 800 it draws all [patch+0x60] blades at full strength; past that f = 1 - (d - 800) / 1700 is both its fade
// (alpha f * 255 at [patch+0x5C], with the plain program) and the share of its blades drawn ([patch+0x64], at least
// 1). grass::g_distant (or XBE_GRASS_DISTANT=0..2): 1 keeps the fade and draws them all, 2 draws every patch as a
// near one, however far.
uint32_t grass::g_distant = grass::kNear;

namespace {
uint32_t GrassDistant() {
    static const char* env = std::getenv("XBE_GRASS_DISTANT");
    return env ? uint32_t(std::atoi(env)) : grass::g_distant;
}
}  // namespace

// [[hook]] at 0x1A95DD, before d is weighed
void ConkerGrassNear(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (GrassDistant() == grass::kNear) kernel::W32(ctx.esp + 0x20, 0);  // 0.0f
}

// [[hook]] at 0x1A968F, where the near and fading paths join with the count set
void ConkerGrassFull(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (GrassDistant() == grass::kFullFading) kernel::W32(ctx.ebx + 0x64, kernel::R32(ctx.ebx + 0x60));
}

// 0x81FC0 (edi the object, its player's index, and where it writes the shells' length scale): how many fur shells an
// object draws ([0x750340], run by 0x3F0270). Most take R = 1000 (type 0x6D 3000) and at most 8; past 0.4 R the count
// falls linearly from that to 1, all but the skin gone by R plus a share for the model's own count. Closer, the full
// count, and the length scale grows from 0.5 at 100 to 1 at 0.4 R. A few types and levels return a fixed count first.
// fur::g_distant (or XBE_FUR_DISTANT=0 off) weighs a farther object as at 0.4 R: every shell, at full length.
bool fur::g_distant = true;

// [[hook]] at 0x8210C, xmm0 the object's distance from the player and xmm3 R
void ConkerFurDistant(X86Context& ctx, uint8_t* base) {
    (void)base;
    static const char* env = std::getenv("XBE_FUR_DISTANT");
    if (!(env ? env[0] != '0' : fur::g_distant)) return;
    ctx.xmm[0].f[0] = std::min(ctx.xmm[0].f[0], ctx.xmm[3].f[0] * 0.4f);
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

// The library's two voice-descriptor setters, stream 0x4C5235 (stream, descriptor; it jumps to 0x4C4EF5) and buffer
// 0x4C51B8 (buffer, descriptor; it calls 0x4C4BCB). Their shared body 0x4C3DFE stores the descriptor through the
// console voice's [voice+0xb8], which the runtime's host objects do not build. XACT fills the descriptor from the
// 3D calculator below (0x4C4613 writes it): +4 distance and +8 cone volume, +0x1C the I3DL2 direct path (occlusion),
// in hundredths of a dB, +0x28 the azimuth in degrees. The fields keep their last values, the flags only say which
// changed. Hand the voice its 3D attenuation and pan; without it every 3D sound played at full volume everywhere.
static void Voice3d(uint32_t obj, uint32_t d) {
    using kernel::R32;
    auto i = [&](uint32_t o) { return int32_t(R32(d + o)); };
    auto f = [&](uint32_t o) {
        float v;
        uint32_t u = R32(d + o);
        std::memcpy(&v, &u, 4);
        return v;
    };
    static const bool trace = std::getenv("XBE_TRACE_DSOUND") != nullptr;
    if (trace)
        LOGR("dsound3d: %08X flags %02X dist %d cone %d pairA %d %d pairB %d %d i3dl2 %d %d pitch %d ang %.1f %.2f", obj,
             R32(d), i(4), i(8), i(0xC), i(0x10), i(0x14), i(0x18), i(0x1C), i(0x20), i(0x24), f(0x28), f(0x2C));
    int32_t att = std::max(i(4) + i(8) + i(0x1C), -10000);
    dsound::SetVoice3d(obj, att, f(0x28));
}
// hooked at the entries the title calls (the recompiler folds 0x4C4EF5 into 0x4C5235's jump), both (object, descriptor)
X86_FUNC(sub_004C5235) {
    (void)base;
    Voice3d(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    kernel::Return(ctx, 0, 2);
}
X86_FUNC(sub_004C51B8) {
    (void)base;
    Voice3d(kernel::R32(ctx.esp + 4), kernel::R32(ctx.esp + 8));
    kernel::Return(ctx, 0, 2);
}
// any other way into the shared body would write the missing console voice: skip it
X86_FUNC(sub_004C3DFE) {
    (void)base;
    kernel::Return(ctx, 0, 2);
}

// The library's 3D calculator: 0x4C5D4C (reached directly, and through the 0x4C606D export thunk) works out a
// voice's 3D parameters against the listener, and the listener helpers (0x4C37C1.., reached from 0x4C4613 and
// 0x4C9C86) turn them into mix-bin volumes, filter and pitch. XACT runs them for every 3D sound and hands the result
// to 0x4C3DFE. They are math on the title's own structures, but go through the table of HRTF functions that
// DirectSoundUseFullHRTF (0x4C3976) installs at 0x4E1D1C..0x4E1D44 by calling 0x4C753A, and the runtime's HLE of
// it installs nothing. Install the table before they run.
static void EnsureHrtf(X86Context& ctx) {
    if (!kernel::R32(0x4E1D1C)) kernel::CallGuest(ctx, 0x4C753A, {});
}
#define HRTF_USER(addr)                 \
    X86_EXTERN_FUNC(__imp__sub_##addr); \
    X86_FUNC(sub_##addr) {              \
        EnsureHrtf(ctx);                \
        __imp__sub_##addr(ctx, base);   \
    }
HRTF_USER(004C5D4C)
HRTF_USER(004C37C1)
HRTF_USER(004C37EA)
HRTF_USER(004C3821)
HRTF_USER(004C3841)
HRTF_USER(004C3875)
HRTF_USER(004C3897)
HRTF_USER(004C9C86)

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

// 0x53AD90: the library applies the fixups queued for push buffers (RunPushBuffer with a D3DFixup): each copies
// bytes into a static push buffer, here jumps that pick one of a model's material blocks per pass. On the console
// the chip has run the earlier jumps into that buffer by then, but the runtime reads the command stream later
// (d3d::Walk), so every run of a model drawn twice in a frame read the last pass's patches and the other pass was
// lost. Read what is queued before the patches land.
X86_EXTERN_FUNC(__imp__sub_0053AD90);
X86_FUNC(sub_0053AD90) {
    d3d::FlushPending();
    __imp__sub_0053AD90(ctx, base);
}

// 0x53AE30: RunPushBuffer(push buffer, fixup), stdcall; the D3DPushBuffer's data at +4. XBE_LOD_TRACE logs who first
// ran each buffer and which stop and start from frame to frame, to tie a model's detail switch (the walk's push
// buffer) to the code that picked it. From the model node renderer (0x3F3510, returning to 0x3F3602) esi is the node.
X86_EXTERN_FUNC(__imp__sub_0053AE30);
X86_FUNC(sub_0053AE30) {
    if (d3d::LodTrace())
        if (uint32_t pb = kernel::R32(ctx.esp + 4))
            d3d::LodTraceRun(ctx, kernel::R32(pb + 4), kernel::R32(ctx.esp) == 0x3F3602 ? ctx.esi : 0);
    __imp__sub_0053AE30(ctx, base);
}

// 0x123290 updates a player's idea bulb (type 0x6B, the list at 0x77168C; esi the bulb, edi the player), by its state
// [bulb+0x2F]: 0 arms it when the player stands on a B pad ([[player+0x568]+0x97]), with [bulb+0x2C] twice the
// refresh rate in ticks (two seconds) and state 1; 1 shows it and counts that down by the frame's ticks ([0x7FA20C]),
// going to 2 when it runs out or the player leaves the pad; 2 drops it until the player leaves the pad, then 0.
// bulb::g_lasting: before state 1 counts down (0x12338E), the ticks are given back while time is left, so the bulb
// stays until the player leaves the pad or the game zeroes its time (0x123388, [[player+0x568]+0xA0] set).
bool bulb::g_lasting = false;

// [[hook]] before 0x12338E: the showing bulb's countdown
void ConkerBulbLasting(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!bulb::g_lasting) return;
    int32_t time = int16_t(kernel::R16(ctx.esi + 0x2C));
    int32_t ticks = int32_t(kernel::R32(0x7FA20C));
    if (time > 0) kernel::W16(ctx.esi + 0x2C, uint16_t(std::min(time + ticks, 0x7FFF)));
}

// 0x2D5540 steps a character's timers each frame (edi the character, dl the frame's ticks [0x7FA250]), most by the
// ticks, but the byte [character+0x214] by one a frame (0x2D566C). The animation scripts set it (0x30DA30 sets the
// player's [0x84A5F8] to the command's argument) and branch when it is out (0x2C6A10, the script's jump on +0x214
// zero): the last boss's scripts time the window in which the alien takes Conker's punches with it. The console runs
// at 30 FPS, two ticks a frame, so at 60 the window was half as long, and after the second toss too short to land
// all four punches. It counts console frames here, one each two ticks, as at 30.
// [[hook]] before 0x2D5662, on_true past the decrement (0x2D5674)
bool ConkerScriptTimer(X86Context& ctx, uint8_t* base) {
    uint8_t& left = base[uint32_t(ctx.edi + 0x214)];
    if (left == 0) return true;
    const uint32_t now = display::Ticks(), ticks = ctx.edx & 0xFF;
    const uint32_t frames = now / 2 - (now - ticks) / 2;  // console frames this frame's ticks end
    left = uint8_t(left > frames ? left - frames : 0);
    return true;
}
