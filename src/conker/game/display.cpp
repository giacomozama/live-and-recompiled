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
// Field of view. The same function then tail calls 0x645C0 with the vertical field of view in xmm0 (0x64468 loads
// it) and the horizontal one in xmm1 ([camera+0x260], widened above), which keeps the first and xmm1 / xmm0, the
// aspect, in the camera's view object ([camera+0x144] +0x104, +0x108) for the projection, as a scope's zoom does
// through [camera+0x264]. Scaling both there by display::g_fov over 50 widens every camera alike and leaves
// [camera+0x264] the game's, so a zoom keeps easing from where it was. [camera+0x210], which 0x64330 works out from
// the unscaled value before, stays as it was: the detail the game picks by distance does not drop for a wider view.
// The game rebuilds a camera's projection as it moves (0x3FA56 and its like, each frame in play) and again when a
// cutscene ends (0x50953, 0x509B2), so the setting and the end of a cutscene take effect at the next rebuild. Not in
// the frontend's levels, nor while a cutscene plays: [0x76F0E0 + slot], its state per slot (1 playing, 2 ending).
//
// HUD. In a multiplayer match the HUD is drawn unstretched (gpu::g_hud_split): each of its elements, as the game draws
// them (the scopes at the end of this file), at the frame's shape against the display's left edge, its middle or its
// right edge, by which third of the frame the element lies in (the clock and radar left, the scores middle, the weapon
// and bars right), or where it is (the crosshair and what follows it, the weapon wheel over its 3D models). Not in the frontend, nor under a menu (a match's
// pause menu, the class and spawn choice: the mouse picks their items where the game drew them), nor in a three or
// four player split screen, whose quarters each have a HUD of their own (the present interval byte 0x6C0268, 3 there).
// The setting hud_split (display::g_hud_split) turns it off, as does XBE_HUD_SPLIT=0.
#include "common/display.h"

#include <algorithm>
#include <utility>
#include <iterator>
#include <vector>
#include <unordered_map>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

#include "common/mouse.h"
#include "common/warp.h"
#include "gpu/shaders.h"

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

// gpu::g_hud_split for the frame about to be drawn
constexpr uint32_t kSplitInterval = 0x6C0268;  // the game's present interval, 3 in a three or four player split screen
bool HudSplit() {
    static const bool off = [] {
        const char* e = std::getenv("XBE_HUD_SPLIT");
        return e && *e == '0';
    }();
    return !off && display::g_hud_split && warp::Multiplayer() && !warp::Frontend() && !mouse::GameMenu() &&
           (kernel::R32(kSplitInterval) & 0xFF) != 3;
}

}  // namespace

void SetFrameRate(uint32_t rate) {
    g_frame_rate = Clamp(rate);
    app::Get().fps_cap = Rate();
    app::Get().frame_generation = Shown() / Rate();
}

uint32_t GameRate() { return Rate(); }

std::atomic<uint32_t> g_ticks{0};
uint32_t Ticks() { return g_ticks.load(std::memory_order_relaxed); }

uint32_t g_fov = kFov;
bool g_hud_split = true;

std::atomic<float> g_camera_fov{50.0f};
float CameraFov() { return g_camera_fov.load(std::memory_order_relaxed); }

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
    gpu::g_hud_split.store(display::HudSplit(), std::memory_order_relaxed);
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
    uint32_t v = kernel::R32(ctx.ecx + ctx.eax + 0x264);
    float vfov;
    std::memcpy(&vfov, &v, 4);
    if (std::isfinite(vfov) && vfov > 0.0f) display::g_camera_fov.store(vfov, std::memory_order_relaxed);
    if (frame != shown) {
        LOGR("display: camera fov %.2f x %.2f degrees, drawn %.2f wide", ctx.xmm[1].f[0], vfov,
             ctx.xmm[1].f[0] * frame * 0.75f);
        shown = frame;
    }
    ctx.xmm[1].f[0] *= frame * 0.75f;
}

// [[hook]] before 0x64471 in the camera projection, its tail jump to 0x645C0: xmm0 holds the vertical field of view
void ConkerCameraFov(X86Context& ctx, uint8_t* base) {
    constexpr uint32_t kCutsceneState = 0x76F0E0, kCutsceneSlots = 4;
    const uint32_t fov = std::clamp(display::g_fov, display::kFovMin, display::kFovMax);
    if (fov == display::kFov || warp::Frontend()) return;
    for (uint32_t i = 0; i < kCutsceneSlots; i++)
        if (base[kCutsceneState + i]) return;
    // 0x645C0 keeps xmm1 / xmm0 as the view's aspect: both scale alike, or a taller view would come out squeezed
    const float h = ctx.xmm[0].f[0], w = ctx.xmm[1].f[0];
    if (!std::isfinite(h) || !std::isfinite(w) || h <= 0.0f || w <= 0.0f) return;
    const float k = std::min(float(fov) / float(display::kFov), 170.0f / h);
    ctx.xmm[0].f[0] = h * k;
    ctx.xmm[1].f[0] = w * k;
}

// The HUD's elements, for gpu::g_hud_split, each a scope (d3d::HudScope) whose draws are placed by its gpu::HudMode.
// The game draws its 2D in three ways (traced in a match: captures 153041..153049):
// - Scene graph nodes, 0x37E230 (ecx the node: its own position and scale at +0x20, its children's draw methods at
//   +0x3C, +0x1D of them). The HUD's tree has containers at 0 0 and its widgets under them at their own positions (the
//   radar at -243 147): an element is the outermost node with a position of its own, kept to its edge.
// - The effect layers' objects: 0x0D9950 calls each layer's draw routine for each of its objects at 0x0D9AAF (ebx the
//   layer, esi the object, ebp the routine). The HUD's layers, each one element: 122 the crosshair (five objects,
//   unstretched in place: it is carried onto a target; the sniper's scope over the frame is one of its draws, with
//   black at the sides), 127 the HUD (0x1C7C60), 130 the clock and the kill messages, 131 a banner, 134 the bars at the
//   right (made again each frame); the rest (112 the drops on the lens, particles, smoke) is 3D or lies over the 3D,
//   and stays as drawn.
//   0x1C7C60 draws several widgets, each through a call of its own (HudCall): the scores 0x1CB330, the weapon panel
//   0x1CC030, the weapon wheel 0x1CA190 (its pieces unstretched each in place, over the weapons' models, which the
//   frame draws in 3D), the weapon icons 0x1CC770 and 0x1CAAA0, the button icons 0x1CB1A0 and 0x330AB0 (through a
//   pointer); what it draws outside them (the bar at the top right) is the object's.
// - The text list ([0x849794], entries of 0x102C bytes made by 0x1F27B0 through 0x1F25F0 or 0x1F26E0, drawn by
//   0x1F2BC0: +0 an object it is drawn over, +8 and +0xA its x and y, the string at +0x28, the next at +0x24). An
//   entry goes with the element whose scope made it (the clock's digits and kill messages, the scores'), or, made in one
//   placed in place, is placed in place itself (the wheel's names); one centred on its x (flag 1 at +0xC) is a line of
//   its own, kept to its edge (the clock's spree message, centred on the frame); one over an object (projected from the
//   world) stays as drawn; the rest are their maker's (the call site), kept to its edge, but for the ammo under the
//   crosshair (0x1C8813), which follows it in place.
// Draws in no scope (the names over players, drawn with the characters) stay as drawn. XBE_HUD_TRACE=1 notes each scope
// in the frame capture.
namespace {

bool HudTrace() {
    static const bool on = std::getenv("XBE_HUD_TRACE") != nullptr;
    return on;
}

bool HudScopes() { return gpu::g_hud_split.load(std::memory_order_relaxed); }

float HudF(uint32_t va) {
    const uint32_t v = kernel::R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr uint32_t kHudLayerCrosshair = 122, kHudLayerHud = 127;
constexpr uint32_t kHudLayers[] = {kHudLayerCrosshair, kHudLayerHud, 130, 131, 134};
// a HUD layer's objects as one element (the crosshair's five), but 127's: some layers make theirs again each frame
constexpr uint32_t kHudLayerId = 0xFF000000u;
constexpr uint32_t kHudAmmoText = 0x1C8813;  // its text under the crosshair (0x1F25F0's return address)
constexpr uint8_t kTextCentred = 0x01;  // a text entry's flags (+0xC): centred on its x

// the scopes this file opened, so each end closes its own: game thread
bool g_layer_scoped = false;     // a layer object's scope is open
uint32_t g_hud_object = 0;       // the layer object 0x1C7C60 is drawing, 0 none
uint32_t g_hud_call = 0;         // the call of 0x1C7C60's whose scope is open, 0 none
uint32_t g_node_element = 0;     // the node element open, 0 none
uint32_t g_text_site = 0;        // the call that is adding a text entry (0x1F25F0's or 0x1F26E0's return address)
bool g_text_scope = false;       // the text list's drawing has a scope open for its entry

// how a text entry is placed, as its maker left it: by entry address (an address made again is a new entry)
struct TextMade {
    uint32_t id = 0;
    uint8_t mode = 0;
    uint32_t site = 0;  // the call that made it
};
std::unordered_map<uint32_t, TextMade> g_text_made;

// An entry placed in place about its own box is an element of its own, placed about its box of the frame before. The
// list is made again each frame, so its addresses go to other strings from frame to frame (or to none the frame
// before), and an element by address took another string's box, or in its first frame a box growing draw by draw,
// each glyph and its shadow placed about a different middle: the wheel's names shook. So such an entry is the n-th
// one its maker made, in the list's order (TextPlace), counted again each time the list is drawn (0x1F2BC0).
std::unordered_map<uint32_t, uint32_t> g_text_drawn;  // maker -> its entries in place drawn so far
uint32_t TextPlace(uint32_t site) {
    const uint32_t n = g_text_drawn[site]++;
    return 0xF0000000u | ((site & 0x00FFFFFFu) << 4) | (n & 0xFu);
}

void HudNoteF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void HudNoteF(const char* fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    d3d::HudNote(text);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0037E230);
X86_FUNC(sub_0037E230) {
    const uint32_t node = ctx.ecx;
    bool scoped = false;
    if (HudScopes()) {
        if (HudTrace())
            HudNoteF("hud node %08X class %08X flags %02X %02X kids %u local %.1f %.1f %.1f | %.3f %.3f %.3f %.3f from %06X",
                     node, kernel::R32(node), base[node + 0x14], base[node + 0x1C], base[node + 0x1D], HudF(node + 0x20),
                     HudF(node + 0x24), HudF(node + 0x28), HudF(node + 0x2C), HudF(node + 0x30), HudF(node + 0x34),
                     HudF(node + 0x38), kernel::R32(ctx.esp));
        // the outermost node with a position of its own: a widget
        if (!g_node_element && (HudF(node + 0x20) != 0.0f || HudF(node + 0x24) != 0.0f)) {
            d3d::HudScope(node, gpu::kHudEdge);
            g_node_element = node;
            scoped = true;
        }
    }
    __imp__sub_0037E230(ctx, base);
    if (scoped) {
        d3d::HudScopeEnd();
        g_node_element = 0;
    }
}

// [[hook]] before 0x0D9AAF, an effect layer's object drawn: ebx the layer, esi the object, ebp its draw routine
void ConkerHudLayerObject(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!HudScopes()) return;
    g_layer_scoped = true;
    if (HudTrace()) HudNoteF("hud layer %u object %08X draw %06X", ctx.ebx, ctx.esi, ctx.ebp);
    const bool hud = std::find(std::begin(kHudLayers), std::end(kHudLayers), ctx.ebx) != std::end(kHudLayers);
    if (!hud) {
        d3d::HudScope(ctx.esi, gpu::kHudStretched);
    } else if (ctx.ebx == kHudLayerCrosshair) {
        d3d::HudScope(kHudLayerId | ctx.ebx, gpu::kHudInPlaceBars);  // the sniper's scope covers the frame
    } else if (ctx.ebx == kHudLayerHud) {
        d3d::HudScope(ctx.esi, gpu::kHudEdge);
        g_hud_object = ctx.esi;
    } else {
        d3d::HudScope(kHudLayerId | ctx.ebx, gpu::kHudEdge);  // its objects are made again each frame (the bars)
    }
}

// [[hook]] before 0x0D9AB1, after it
void ConkerHudLayerObjectEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!std::exchange(g_layer_scoped, false)) return;
    d3d::HudScopeEnd();
    g_hud_object = 0;
}

// 0x1C7C60's widgets: a call of its, made while it draws, is a scope by its return address
namespace {
// an element's id, for one drawn by several calls (0 for each call its own, by its return address)
constexpr uint32_t kHudScores = 0xFE000001u, kHudWeapon = 0xFE000002u;

template <void (*Imp)(X86Context&, uint8_t*), uint8_t Mode, uint32_t Element>
void HudCall(X86Context& ctx, uint8_t* base) {
    const uint32_t site = kernel::R32(ctx.esp);
    const bool open = g_hud_object && !g_hud_call && HudScopes();
    if (open) {
        if (HudTrace()) HudNoteF("hud call %06X", site);
        d3d::HudScope(Element ? Element : site, Mode);
        g_hud_call = site;
    }
    Imp(ctx, base);
    if (open) {
        d3d::HudScopeEnd();
        g_hud_call = 0;
    }
}
}  // namespace

// the weapon panel's pieces are one element: its button icon (406..414) alone lies in the middle third
#define CONKER_HUD_CALL(addr, mode, element)                                    \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                         \
    X86_FUNC(sub_##addr) { HudCall<__imp__sub_##addr, gpu::mode, element>(ctx, base); }
CONKER_HUD_CALL(001CB330, kHudEdge, kHudScores)  // the scores
CONKER_HUD_CALL(001CC030, kHudEdge, kHudWeapon)  // the weapon panel
CONKER_HUD_CALL(001CA190, kHudInPlaceEach, 0)    // the weapon wheel
CONKER_HUD_CALL(001CC770, kHudEdge, kHudWeapon)  // a weapon icon, the bar at the top right
CONKER_HUD_CALL(001CAAA0, kHudEdge, kHudWeapon)  // a weapon picture
CONKER_HUD_CALL(001CB1A0, kHudEdge, kHudWeapon)  // the panel's button icon
CONKER_HUD_CALL(00330AB0, kHudEdge, kHudWeapon)  // another, through a pointer
#undef CONKER_HUD_CALL

// 0x1F2BC0, the text list drawn: its entries in place are counted from the start (TextPlace)
X86_EXTERN_FUNC(__imp__sub_001F2BC0);
X86_FUNC(sub_001F2BC0) {
    g_text_drawn.clear();
    __imp__sub_001F2BC0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001F25F0);
X86_FUNC(sub_001F25F0) {
    const uint32_t was = std::exchange(g_text_site, kernel::R32(ctx.esp));
    __imp__sub_001F25F0(ctx, base);
    g_text_site = was;
}

X86_EXTERN_FUNC(__imp__sub_001F26E0);
X86_FUNC(sub_001F26E0) {
    const uint32_t was = std::exchange(g_text_site, kernel::R32(ctx.esp));
    __imp__sub_001F26E0(ctx, base);
    g_text_site = was;
}

// [[hook]] before 0x1F2835 in 0x1F27B0, a text entry made and linked: edx the entry
void ConkerHudTextMade(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!HudScopes()) return;
    const uint32_t e = ctx.edx;
    const auto [scope, mode] = d3d::HudScopeNow();
    TextMade made;
    if (g_text_site == kHudAmmoText || mode == gpu::kHudInPlace || mode == gpu::kHudInPlaceEach ||
        mode == gpu::kHudInPlaceBars) {
        made = {e, gpu::kHudInPlace, g_text_site};  // about its own box (TextPlace)
    } else if (mode == gpu::kHudEdge && !(g_hud_object && scope == g_hud_object)) {
        made = {scope, gpu::kHudEdge};  // with the widget that made it
    } else if (mode == gpu::kHudStretched) {
        made = {scope, gpu::kHudStretched};
    } else {
        made = {g_text_site ? g_text_site : e, gpu::kHudEdge};  // its maker's
    }
    g_text_made[e] = made;
    if (g_text_made.size() > 4096) g_text_made.clear();  // entries come and go; a stale one is only placed by its maker
    if (HudTrace()) HudNoteF("hud text made %08X by %06X in %08X: %08X mode %u", e, g_text_site, scope, made.id, made.mode);
}

// [[hook]] before 0x1F2F7B in 0x1F2BC0, a text entry about to be drawn: esi the entry
void ConkerHudTextEntry(X86Context& ctx, uint8_t* base) {
    if (!HudScopes()) return;
    const uint32_t e = ctx.esi;
    TextMade made{e, gpu::kHudEdge};
    if (auto it = g_text_made.find(e); it != g_text_made.end()) made = it->second;
    if (kernel::R32(e)) {
        made = {e, gpu::kHudStretched};  // over an object in the world
    } else if ((base[e + 0xC] & kTextCentred) && made.mode == gpu::kHudEdge) {
        made = {e, gpu::kHudEdge};  // centred on its x, a line of its own (the clock's spree message, at 320)
    } else if (made.mode == gpu::kHudInPlace && made.id == e) {
        made.id = TextPlace(made.site);  // the same element from frame to frame
    }
    if (HudTrace()) {
        char str[40] = {};
        for (uint32_t i = 0; i + 1 < sizeof(str); i++) {
            const uint16_t c = kernel::R16(e + 0x28 + i * 2);
            if (!c) break;
            str[i] = c < 0x80 && c >= 0x20 ? char(c) : '?';
        }
        HudNoteF("hud text entry %08X over %08X at %d %d flags %02X \"%s\": %08X mode %u", e, kernel::R32(e),
                 int16_t(kernel::R16(e + 8)), int16_t(kernel::R16(e + 0xA)), base[e + 0xC], str, made.id, made.mode);
    }
    d3d::HudScope(made.id, made.mode);
    g_text_scope = true;
}

// [[hook]] before 0x1F2FC3 in 0x1F2BC0, an entry done (drawn or skipped)
void ConkerHudTextEntryEnd(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (std::exchange(g_text_scope, false)) d3d::HudScopeEnd();
}
