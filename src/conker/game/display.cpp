// Frame rate and aspect. Both follow the community XBE patches for the console game (widescreen, and theboy181's
// work in progress 60 FPS), which the Xbox One backward compatibility patcher in 60fps/ carries over; here they are
// hooks into the recompiled code, so they apply live from the settings menu.
//
// Frame rate. The frame loop (0x2A4800..) presents with D3DPRESENT_INTERVAL_TWO: 0x2A78D0 builds the present
// parameters from edx (2, or 3 in a three or four player split screen), and keeps it in the byte 0x6C0268. The frame
// timer 0x2A7B60 then hands the game two clocks each frame:
//   - [0x7FA21C] dt, seconds: measured on the time stamp counter, or ticks / 60 in fixed step mode ([0x7819D5],
//     set by the multiplayer setup), clamped to 1 s; [0x7FA214] is 1 / dt
//   - [0x7FA20C] ticks, the 60 Hz blanks the frame took: the interval byte plus the blanks the swap callback
//     (0x2A8020, D3DSWAPDATA.MissedVBlanks) reports missed, at most 60
// About 400 places step by ticks (counters, fades, integer motion), so the XBE patch's interval ONE (60 FPS) is one
// tick a frame. Here ticks come from the time the frame took instead, rounded with the remainder carried, so they
// stay right when a frame runs long, at least one a frame. The game runs no faster than 60: the ticks would alternate
// one and none, and what steps by them (Conker's jump and fall, weapon mode's walk, the pad's presses) would move or
// read only on every second frame, so jitter and drop half the presses. 120, 180 and 240 keep the game at 60 and have
// the runtime draw each frame again for the pictures between it and the one before (frame generation).
//
// Aspect. 0x64468 builds a camera's projection from its aspect ([camera+0x260], 4:3) and vertical field of view
// ([camera+0x264]). The widescreen patch multiplied the aspect by 4/3 there for a 16:9 TV; scaling it by the display
// over 4:3 widens the view to any shape with the same vertical view (Hor+). The frame stays the console's 640x480 and
// the window shows it at the display's shape, so 2D (the HUD, menus) stretches across it as with the XBE patch.
#include "common/display.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "xdk/hle.h"

#include "common/warp.h"

namespace display {

#if !defined(XBE_HEADLESS)
uint32_t g_frame_rate = 60;
#else
uint32_t g_frame_rate = 30;
#endif

namespace {

constexpr uint32_t kPresentInterval = 0x549BF4;    // D3DRS_PRESENTATIONINTERVAL in the render state shadow
constexpr uint32_t kIntervalImmediate = 0x80000000u;

constexpr uint32_t kGameMost = 60;     // the ticks are 60ths of a second: the game runs no faster
constexpr uint32_t kShownMost = 240;   // four pictures a frame, the most the runtime draws

// 30, or a multiple of 60 up to kShownMost
uint32_t Clamp(uint32_t r) { return r <= 30 ? 30 : std::min((r + kGameMost / 2) / kGameMost * kGameMost, kShownMost); }

// what the setting asks for: the pictures shown a second
uint32_t Shown() {
    static const uint32_t env = [] {
        const char* e = std::getenv("XBE_FRAME_RATE");
        return e ? uint32_t(std::atoi(e)) : 0u;
    }();
    return Clamp(env ? env : g_frame_rate);
}

// the game's: 30 (the console's) or 60, past which the runtime draws the frames between
uint32_t Rate() { return std::min(Shown(), kGameMost); }

}  // namespace

void SetFrameRate(uint32_t rate) {
    g_frame_rate = Clamp(rate);
    app::Get().fps_cap = Rate();
    app::Get().frame_generation = Shown() / Rate();
}

uint32_t GameRate() { return Rate(); }

std::atomic<uint32_t> g_ticks{0};
uint32_t Ticks() { return g_ticks.load(std::memory_order_relaxed); }

void Init() { SetFrameRate(g_frame_rate); }

float Aspect(float d) {
    static const float env = [] {
        float w = 0, h = 0;
        const char* e = std::getenv("XBE_ASPECT");
        return e && std::sscanf(e, "%f:%f", &w, &h) == 2 && w > 0 && h > 0 ? w / h : 0.0f;
    }();
    if (env) return env;
#if !defined(XBE_HEADLESS)
    return d;
#else
    (void)d;
    return 4.0f / 3.0f;  // the harness keeps the console's frame, which its captures are compared against
#endif
}

}  // namespace display

using display::Rate;

// 0x2A7B60: the frame timer, called once a frame by the frame loop before it renders and presents. Above 30 the game
// paces itself: the runtime still paces Swap to the fps cap, and to 30 for the console's interval, so the render
// state the runtime reads each Swap asks for no blank wait. The time this waits is the time the timer then measures,
// as the blank wait was on the console.
X86_EXTERN_FUNC(__imp__sub_002A7B60);
X86_FUNC(sub_002A7B60) {
    warp::Poll(ctx);  // a frame of the level loop: the Debug tab's warps can run
    using clock = std::chrono::steady_clock;
    static bool owned = false;
    static uint32_t shown = 0;
    static clock::time_point next = clock::now();
    uint32_t rate = Rate();
    if (rate != shown) {
        LOGR("display: %u FPS%s", rate, rate > 30 ? ", ticks from the frame time" : " (the console's)");
        shown = rate;
    }
    if (rate > 30) {
        kernel::W32(display::kPresentInterval, display::kIntervalImmediate);
        owned = true;
        auto period = std::chrono::nanoseconds(1000000000ull / rate);
        auto now = clock::now();
        next += period;
        if (next < now - period) next = now;
        while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (clock::now() < next) std::this_thread::yield();
    } else if (owned) {
        kernel::W32(display::kPresentInterval, 0);  // the device's own interval again
        owned = false;
        next = clock::now();
    }
    __imp__sub_002A7B60(ctx, base);
}

// [[hook]] before 0x2A7CBC in the frame timer: ecx holds the frame's ticks, [esp+0x10] the microseconds it took
void ConkerFrameTicks(X86Context& ctx, uint8_t* base) {
    (void)base;
    static double carry = 0;
    if (Rate() <= 30) {
        carry = 0;
        display::g_ticks += ctx.ecx;
        return;
    }
    uint64_t us = uint64_t(kernel::R32(ctx.esp + 0x10)) | uint64_t(kernel::R32(ctx.esp + 0x14)) << 32;
    double t = carry + double(int64_t(us)) * 60e-6;
    // a frame is a tick at least: the console never sees a frame of none, and a short frame after a long one rounded
    // to none (carry -0.5 + 0.9) made the step 0x2D37F4 stores zero, which 0x2DA316 divides by
    int32_t ticks = int32_t(std::min(std::max(t + 0.5, 1.0), 60.0));
    carry = std::clamp(t - ticks, -0.5, 0.5);
    static const bool trace = std::getenv("XBE_TRACE_TICKS") != nullptr;
    if (trace) LOGR("display: frame %llu us, %d ticks (console's %u), carry %.2f", (unsigned long long)us, ticks, ctx.ecx, carry);
    ctx.ecx = uint32_t(ticks);
    display::g_ticks += ctx.ecx;
}

// [[hook]] before 0x64468 in the camera projection: xmm1 holds the camera's aspect
void ConkerCameraAspect(X86Context& ctx, uint8_t* base) {
    (void)base;
    static float shown = 0;
    float frame = app::FrameAspect();
    if (frame != shown) {
        uint32_t v = kernel::R32(ctx.ecx + ctx.eax + 0x264);
        float vfov;
        std::memcpy(&vfov, &v, 4);
        LOGR("display: camera fov %.2f x %.2f degrees, drawn %.2f wide", ctx.xmm[1].f[0], vfov,
             ctx.xmm[1].f[0] * frame * 0.75f);
        shown = frame;
    }
    ctx.xmm[1].f[0] *= frame * 0.75f;
}
