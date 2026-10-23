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
//
// Split screen. 0x2A9260 fills a table per layout ([0x84A144], the players less one) from the frame's width and
// height: [0x7F9FD8 + l*4] the view width, [0x7F9FC4 + l*4] its height, [0x7F9F70 + (l*5 + p)*4] each player's view
// center (two words). Two players are full width, half height, centered a quarter and three quarters down. 0x2A88E0
// sets a camera from them when a match lays out its views: the size less 2 (the line between), and for two players the
// width cut to [0x7F9FC0] (some 80%) with the horizontal field of view [camera+0x258] at that times 103 degrees (one
// player's is 60.6, the vertical 50). Side by side, the hook gives them half the width and the full height, and the
// field of view one player has for that shape.
#include "common/display.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

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
bool g_side_by_side = false;

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

namespace {

float F32(uint32_t a) {
    uint32_t v = kernel::R32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void W32F(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    kernel::W32(a, v);
}

// the two player views were last laid out side by side: the HUD sizes for them (the setting may change mid match)
bool g_side_laid = false;

bool SideBySide() { return g_side_laid; }

}  // namespace

// [[hook]] before 0x2A8A43 in the camera layout 0x2A88E0, its size and fields of view set, before the half size and
// center: esi holds the layout, edi the camera's offset in the camera array
void ConkerSplitLayout(X86Context& ctx, uint8_t* base) {
    (void)base;
    // one player (camera 0 at layout 0) or three or four: not two side by side. Two players also lay out a third
    // camera at layout 0, which leaves it.
    if (ctx.esi >= 2 || (ctx.esi == 0 && ctx.ebx == 0)) g_side_laid = false;
    if (ctx.esi != 1) return;
    // the frame, layout 0's view
    int32_t w = int32_t(kernel::R32(0x7F9FD8)), h = int32_t(kernel::R32(0x7F9FC4));
    bool side = display::g_side_by_side;
    g_side_laid = side;
    // the two players' centers, as 0x2A9260 writes them for top and bottom
    auto center = [](uint32_t p, int32_t x, int32_t y) {
        kernel::W16(0x7F9F84 + p * 4, uint16_t(x));
        kernel::W16(0x7F9F86 + p * 4, uint16_t(y));
    };
    center(0, side ? w / 4 : w / 2, side ? h / 2 : h / 4);
    center(1, side ? w / 4 * 3 : w / 2, side ? h / 2 : h / 4 * 3);
    kernel::W32(0x7F9FDC, uint32_t(side ? w / 2 : w));
    kernel::W32(0x7F9FC8, uint32_t(side ? h : h / 2));
    if (!side) return;
    uint32_t cam = kernel::R32(0x84A13C) + ctx.edi;
    float vw = float(w / 2) - 2.0f, vh = float(h) - 2.0f;
    // one player's horizontal field of view (0x58BFEC) for their 4:3 frame, scaled to this shape
    float hfov = F32(0x58BFEC) * (vw / vh) / (float(w) / float(h));
    W32F(cam + 0x1F4, vw);
    W32F(cam + 0x1F8, vh);
    W32F(cam + 0x258, hfov);
    W32F(cam + 0x260, hfov);
    static bool logged = false;
    if (!logged) LOGR("display: two players side by side, %.0fx%.0f views, fov %.1f x %.1f", vw, vh, hfov, F32(cam + 0x264));
    logged = true;
}

// The HUD places a split view's elements at its corner plus their 640x480 position times sx = width / 640 and
// sy = height / 480, and sizes them by the larger of the two (0x1C7D7C in the HUD draw 0x1C7xxx, 0x43541 in 0x43460, which prints text),
// text by sx and sy each. Top and bottom that is 0.79; side by side the larger, about 1, draws the HUD at full size
// into half the width and the text twice as tall as wide. These hooks size both by the smaller, 0.5, as for four
// players, the positions still spread over the half.

// [[hook]] before 0x43551 in 0x43460: xmm0 holds sx, xmm1 sy, xmm2 the larger, the scale
void ConkerSplitHudScaleA(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (SideBySide()) ctx.xmm[2].f[0] = std::min(ctx.xmm[0].f[0], ctx.xmm[1].f[0]);
}

// [[hook]] before 0x1C7DB0 in the HUD draw: [esp+0x5C] holds sx, [esp+0x64] sy, [esp+0x44] the scale
void ConkerSplitHudScaleB(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!SideBySide() || !*static_cast<uint8_t*>(kernel::HostPtr(ctx.esp + 0x2C))) return;
    static bool logged = false;
    if (!logged) LOGR("display: side by side HUD, sx %.3f sy %.3f", F32(ctx.esp + 0x5C), F32(ctx.esp + 0x64));
    logged = true;
    W32F(ctx.esp + 0x44, std::min(F32(ctx.esp + 0x5C), F32(ctx.esp + 0x64)));
}

namespace {

// a text size about to be multiplied by sx (in xmm[x]) and sy (in xmm[y]): it ends up times the smaller instead
void HudText(X86Context& ctx, int x, int y) {
    if (!SideBySide()) return;
    float sx = F32(ctx.esp + 0x5C), sy = F32(ctx.esp + 0x64), s = std::min(sx, sy);
    if (sx <= 0 || sy <= 0) return;
    ctx.xmm[x].f[0] *= s / sx;
    ctx.xmm[y].f[0] *= s / sy;
}

}  // namespace

// [[hook]] before 0x1C851D, 0x1C85BD and 0x1C88BC in the HUD draw: a text's width in xmm0, height in xmm1
void ConkerSplitHudText(X86Context& ctx, uint8_t* base) {
    (void)base;
    HudText(ctx, 0, 1);
}

// [[hook]] before 0x1C87C4 in the HUD draw: a text's width in xmm1, height in xmm0
void ConkerSplitHudTextSwapped(X86Context& ctx, uint8_t* base) {
    (void)base;
    HudText(ctx, 1, 0);
}

// [[hook]] before 0x1C8A50 in the HUD draw: the weapon's charge bar ([weapon+0x284]), sized by sx and sy each,
// [esp+0x48] its width and [esp+0x1C] its height
void ConkerSplitHudBar(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!SideBySide() || !*static_cast<uint8_t*>(kernel::HostPtr(ctx.esp + 0x2C))) return;
    float sx = F32(ctx.esp + 0x5C), sy = F32(ctx.esp + 0x64), s = std::min(sx, sy);
    if (sx <= 0 || sy <= 0) return;
    W32F(ctx.esp + 0x48, F32(ctx.esp + 0x48) * s / sx);
    W32F(ctx.esp + 0x1C, F32(ctx.esp + 0x1C) * s / sy);
}

// [[hook]] at 0x1CC9F0: the two player vignette, gradient strips 16 wide at the narrowed views' sides (x 66 and 558,
// y 1 to 239 and 241 to 479); side by side they cross the views, so they are left out
bool ConkerSplitVignette(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    static bool logged = false;
    if (!logged) LOGR("display: two player vignette, side by side %d", SideBySide());
    logged = true;
    return SideBySide();
}

// The sniper scope. 0x1CF1C0 (0x1CF469) picks the lens's half size, 320 by 240 for one player and 158.67 by 119 for
// two, and draws its texture (a tall ellipse, round in a 4:3 box) around the view center. The frame is stretched to
// the display, which widens 2D by FrameAspect / (4/3): top and bottom the lens comes out wide on a 16:9 display, side
// by side more so. Side by side the lens takes the half's width with the height that is round on the display.
//
// The masks: 0x1CF0F0 (and 0x1CF130, the same with soft inner edges) draw boxes beside the lens with
// 0x1CF010(x0, y0, x1, y1), black in ebx and edi, at x 66 to 161 and 479 to 574 and the player's y range (1 to 239 or
// 241 to 479). Side by side they go above and below the lens across the half instead: these hooks rewrite each box's
// corners on the stack before the call.
namespace {

// the lens's half width and height side by side, in frame pixels
std::pair<float, float> LensHalf() {
    float stretch = app::FrameAspect() * 0.75f;  // display pixels across per frame pixel, relative to height
    float hw = 159.0f, hh = hw * 0.75f * stretch;
    if (hh > 239.0f) hw *= 239.0f / hh, hh = 239.0f;
    return {hw, hh};
}

enum class ScopeBox { kAbove, kBelow, kNone };

void ScopeMask(X86Context& ctx, ScopeBox box, bool solid) {
    if (!SideBySide()) return;
    float y0 = F32(ctx.esp + 4), y1 = 0;
    bool right = y0 > 240.0f;  // the caller's range for the second player
    float x0 = right ? 321.0f : 1.0f, x1 = right ? 639.0f : 319.0f;
    float hh = LensHalf().second;
    switch (box) {
        case ScopeBox::kAbove: y0 = 1.0f, y1 = 240.0f - hh; break;
        case ScopeBox::kBelow: y0 = 240.0f + hh, y1 = 479.0f; break;
        case ScopeBox::kNone: x1 = x0, y0 = y1 = 1.0f; break;
    }
    W32F(ctx.esp, x0);
    W32F(ctx.esp + 4, y0);
    W32F(ctx.esp + 8, x1);
    W32F(ctx.esp + 12, y1);
    if (solid) ctx.ebx = ctx.edi = ctx.ebp;  // 0x1CF130's black, without its edge gradient
}

}  // namespace

// [[hook]] before 0x1CF4A5 in the scope draw: [esp+0x98] holds the lens's half width, xmm0 its half height
void ConkerScopeLens(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!SideBySide()) return;
    auto [hw, hh] = LensHalf();
    W32F(ctx.esp + 0x98, hw);
    ctx.xmm[0].f[0] = hh;
}

// [[hook]] before 0x1CF112 in 0x1CF0F0: its left box
void ConkerScopeMaskAbove(X86Context& ctx, uint8_t* base) {
    (void)base;
    ScopeMask(ctx, ScopeBox::kAbove, false);
}

// [[hook]] before 0x1CF123 in 0x1CF0F0: its right box
void ConkerScopeMaskBelow(X86Context& ctx, uint8_t* base) {
    (void)base;
    ScopeMask(ctx, ScopeBox::kBelow, false);
}

// [[hook]] before 0x1CF15C in 0x1CF130: its outer left box
void ConkerScopeEdgeAbove(X86Context& ctx, uint8_t* base) {
    (void)base;
    ScopeMask(ctx, ScopeBox::kAbove, true);
}

// [[hook]] before 0x1CF190 in 0x1CF130: its inner right box
void ConkerScopeEdgeBelow(X86Context& ctx, uint8_t* base) {
    (void)base;
    ScopeMask(ctx, ScopeBox::kBelow, true);
}

// [[hook]] before 0x1CF177 and 0x1CF1AB in 0x1CF130: its gradient boxes, left out
void ConkerScopeEdgeNone(X86Context& ctx, uint8_t* base) {
    (void)base;
    ScopeMask(ctx, ScopeBox::kNone, true);
}
