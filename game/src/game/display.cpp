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
// stay right at any frame rate: two a frame at 30, one at 60, alternately one and none at 120. Nothing divides by
// them. Fixed step mode measures time in ticks alone, so it never gets a frame without one and runs at most 60.
//
// Aspect. 0x64468 builds a camera's projection from its aspect ([camera+0x260], 4:3) and vertical field of view
// ([camera+0x264]). The widescreen patch multiplied the aspect by 4/3 there for a 16:9 TV; scaling it by the display
// over 4:3 widens the view to any shape with the same vertical view (Hor+). The frame stays the console's 640x480 and
// the window shows it at the display's shape, so 2D (the HUD, menus) stretches across it as with the XBE patch.
#include "common/display.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "xdk/hle.h"

namespace display {

#if defined(_WIN32)
uint32_t g_frame_rate = 60;
#else
uint32_t g_frame_rate = 30;
#endif

namespace {

constexpr uint32_t kFixedStep = 0x7819D5;          // byte: dt is ticks / 60 (multiplayer)
constexpr uint32_t kPresentInterval = 0x549BF4;    // D3DRS_PRESENTATIONINTERVAL in the render state shadow
constexpr uint32_t kIntervalImmediate = 0x80000000u;

// what the setting asks for: 30 (the console's), 60 or 120
uint32_t Rate() {
    static const uint32_t env = [] {
        const char* e = std::getenv("XBE_FRAME_RATE");
        return e ? uint32_t(std::atoi(e)) : 0u;
    }();
    uint32_t r = env ? env : g_frame_rate;
    return r <= 30 ? 30 : r <= 60 ? 60 : 120;
}

}  // namespace

float Aspect(float d) {
    static const float env = [] {
        float w = 0, h = 0;
        const char* e = std::getenv("XBE_ASPECT");
        return e && std::sscanf(e, "%f:%f", &w, &h) == 2 && w > 0 && h > 0 ? w / h : 0.0f;
    }();
    if (env) return env;
#if defined(_WIN32)
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
    using clock = std::chrono::steady_clock;
    static bool owned = false;
    static uint32_t shown = 0;
    static clock::time_point next = clock::now();
    uint32_t rate = Rate();
    if (rate > 30 && base[display::kFixedStep] && rate > 60) rate = 60;  // fixed step: every frame a tick
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
    static double carry = 0;
    if (Rate() <= 30) {
        carry = 0;
        return;
    }
    uint64_t us = uint64_t(kernel::R32(ctx.esp + 0x10)) | uint64_t(kernel::R32(ctx.esp + 0x14)) << 32;
    double t = carry + double(int64_t(us)) * 60e-6;
    int32_t ticks = int32_t(std::min(std::max(t + 0.5, 0.0), 60.0));
    // fixed step, or 60 FPS and below: a frame is a tick at least. The console never sees a frame of none, and a
    // short frame after a long one rounded to none (carry -0.5 + 0.9) made the step 0x2D37F4 stores zero and divided by it
    if ((base[display::kFixedStep] || Rate() <= 60) && ticks < 1) ticks = 1;
    carry = std::clamp(t - ticks, -0.5, 0.5);
    static const bool trace = std::getenv("XBE_TRACE_TICKS") != nullptr;
    if (trace) LOGR("display: frame %llu us, %d ticks (console's %u), carry %.2f", (unsigned long long)us, ticks, ctx.ecx, carry);
    ctx.ecx = uint32_t(ticks);
}

// [[hook]] before 0x2D37F4: cx holds the frame's step, int(speed [edi+0x11C] * [0x7F9F54] * dt [0x7FA24C] * 100), which
// 0x2DA316 and others divide by. Above 60 FPS a frame may be no tick at all, dt 0 and the step with it, which the console
// never has: such a frame takes one tick's step (dt 0.5), at least 1.
void ConkerFrameStep(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (uint16_t(ctx.ecx)) return;
    auto f32 = [](uint32_t va) {
        uint32_t v = kernel::R32(va);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    };
    if (f32(0x7FA24C) != 0.0f) return;  // a step the game made zero itself stays
    int32_t step = int32_t(f32(ctx.edi + 0x11C) * f32(0x7F9F54) * 0.5f * 100.0f);
    ctx.ecx = (ctx.ecx & 0xFFFF0000u) | uint16_t(std::clamp(step, 1, 0x7FFF));
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
