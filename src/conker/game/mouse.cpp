// Mouse look: the mouse turns the camera where the game turns it by the right stick.
//
// The window thread adds raw mouse motion (WM_INPUT, unaccelerated counts, or SDL's relative motion) up. The third
// person, first person and weapon cameras all turn by the stick in 0x2F99A0 (hooked at the end of the file): Turn
// adds the motion, in degrees, to the turn it works out each frame, so each camera takes the mouse as it takes the
// stick, inside its own limits.
//
// A camera that does not go through 0x2F99A0 gets the mouse as right stick instead, once Turn has not run for
// kTurnedUs. A stick sets a turn rate and a mouse a distance, so there the motion is kept as a debt that keyboard::Pad
// pays off a game frame at a time, pushing the stick as far as turns this frame's share. The game reads the stick in
// 64 steps past its dead zone (0x4CF80, then 0x4D110), so motion slower than the first step turns it on some frames
// and not others, and motion faster than the stick's full turn is carried into the next frames (up to kMaxYawDebt)
// instead of lost. The pad is polled more than once a frame; the first poll of a frame takes the step and the rest
// repeat it, and the device scan (xapi.cpp) leaves it alone.
//
// kYawRate and kPitchRate are the game's turn at each step, and FastRate its fast turn, which speeds up while it is
// held. With XBE_MOUSE_CALIBRATE set, F9 steps the stick through them while logging how fast the player camera
// turns (see Calibrate and Probe).
//
// While mouse look is on (the default), the game window has the focus and the menu is shut, the cursor is hidden and
// held inside the window. Alt-tab or Esc (the menu) lets it go. The SDL window (not Windows) holds it in relative mode and sends the
// motion as app::kMsgMouseMotion instead.
//
// On the headless harness there is no window and no mouse, so everything but the option globals compiles out.
#include "common/mouse.h"

#include "cpu/x86_ops.h"

#if !defined(XBE_HEADLESS)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>

#include "app/app.h"
#include "common/display.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace mouse {

bool g_look = true;
uint32_t g_sensitivity = kDefaultSensitivity;
uint32_t g_sensitivity_y = kDefaultSensitivity;
bool g_invert_y = false;

namespace {

// degrees a count turns the camera at 100% sensitivity: 800 counts (an inch at 800 dpi) is 32 degrees
constexpr float kDegreesPerCount = 0.04f;
// the game's stick dead zone (0x4CF80): a stick inside 0.3 of full reads as still, and past it the rest is spread
// evenly, so a stick of 0.3 + 0.7 t reads as t; 0x4D110 then truncates t * 32767 / 512 to a step of 0..63
constexpr float kDeadZone = 0.3f;
constexpr int kSteps = 64;

// The player camera's turn at each step, in degrees a second, measured with XBE_MOUSE_CALIBRATE at 60 fps in third
// person; the steps between are read off the line through their neighbours. Yaw goes up in three straight runs
// (0.45 a step to 20, 0.89 to 30, 2.68 from 32 to 40), and from kFastStep it is the fast turn below. Pitch (the view's,
// 1.22 times the angle the stick sums at +0x970, a little behind it) goes up the same way, later and slower.
struct Point {
    int step;
    float rate;
};
constexpr Point kYawPoints[] = {{0, 0},         {1, 0.45f},    {2, 0.89f},     {3, 1.34f},     {4, 1.79f},
                                {5, 2.23f},     {6, 2.68f},    {8, 3.57f},     {10, 4.46f},    {12, 5.36f},
                                {16, 7.14f},    {20, 8.93f},   {22, 10.64f},   {24, 12.43f},   {26, 14.21f},
                                {28, 16.00f},   {30, 17.78f},  {32, 22.71f},   {36, 33.43f},   {40, 44.15f},
                                {63, 44.15f}};
constexpr float kViewPerSum = 1.22f;
constexpr Point kPitchPoints[] = {{0, 0},       {1, 0.12f},    {4, 0.50f},     {8, 1.00f},     {12, 1.50f},
                                  {16, 2.00f},  {20, 2.49f},   {22, 2.74f},    {24, 2.99f},    {26, 3.34f},
                                  {28, 3.84f},  {30, 4.34f},   {32, 4.84f},    {36, 5.84f},    {40, 7.93f},
                                  {44, 10.92f}, {48, 13.92f},  {52, 20.99f},   {56, 34.16f},   {60, 50.29f},
                                  {63, 62.4f}};
template <size_t N>
constexpr std::array<float, kSteps> Table(const Point (&p)[N], float scale) {
    std::array<float, kSteps> r{};
    for (size_t i = 0; i + 1 < N; i++)
        for (int k = p[i].step; k <= p[i + 1].step; k++)
            r[k] = scale * (p[i].rate + (p[i + 1].rate - p[i].rate) * float(k - p[i].step) /
                                            float(p[i + 1].step - p[i].step));
    return r;
}
constexpr auto kYawRate = Table(kYawPoints, 1.0f);
constexpr auto kPitchRate = Table(kPitchPoints, kViewPerSum);

// The fast turn (yaw only): from step 41 the camera starts at kFastStart a second, the same from step 46 on, and
// speeds up by 1/61 of that every frame (a second at 60 fps) to about twice it, 209 a second; a single frame below
// step 41 starts it over.
constexpr int kFastStep = 41;
constexpr float kFastStart[] = {54.0f, 66.0f, 78.0f, 90.0f, 102.0f, 106.1f};
constexpr float kFastRampS = 61.3f / 60.0f;  // seconds to add the start again
constexpr float kFastMax = 1.967f;

float FastRate(int k, float fast_s) {
    const float start = kFastStart[std::min(k - kFastStep, int(std::size(kFastStart)) - 1)];
    return start * std::min(1.0f + fast_s / kFastRampS, kFastMax);
}

// the most turn carried into later frames, in degrees: a fast flick keeps turning after the mouse stops until it
// is paid, and the pitch, which stops at the view's limits, keeps less so it does not hold on at one
constexpr float kMaxYawDebt = 60.0f, kMaxPitchDebt = 8.0f;
// motion too slow for even the first step is let go once the mouse has rested this long
constexpr int64_t kRestUs = 100000;

std::atomic<bool> g_held{false};

int64_t Now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// the motion the window thread has not handed over yet, under g_lock
std::mutex g_lock;
int64_t g_dx = 0, g_dy = 0;
int64_t g_moved_us = 0;  // the last motion

void Clear() {
    std::lock_guard lock(g_lock);
    g_dx = g_dy = 0;
}

void Record(int32_t dx, int32_t dy) {
    if (!dx && !dy) return;
    std::lock_guard lock(g_lock);
    g_dx += dx, g_dy += dy;
    g_moved_us = Now();
}

#ifdef _WIN32

bool g_registered = false;
bool g_hidden = false;

void Hold(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);
    POINT tl{r.left, r.top}, br{r.right, r.bottom};
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    RECT clip{tl.x, tl.y, br.x, br.y};
    ClipCursor(&clip);
    if (!g_hidden) ShowCursor(FALSE), g_hidden = true;
}

void Release() {
    ClipCursor(nullptr);
    if (g_hidden) ShowCursor(TRUE), g_hidden = false;
}

#else

void Release() { app::HoldMouse(false); }

#endif

}  // namespace

void Refresh(void* window) {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(window);
    const bool want = g_look && !ui::Open() && hwnd && GetForegroundWindow() == hwnd && !IsIconic(hwnd);
    if (want && !g_registered) {
        // only foreground motion (no RIDEV_INPUTSINK); the usual mouse messages still come, for ImGui
        RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd};  // generic desktop, mouse
        g_registered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != 0;
        if (!g_registered) LOGE("mouse: raw input registration failed (%lu)", GetLastError());
    }
    if (want) Hold(hwnd);  // again while held: the window may have moved or changed size
    else if (g_held.load()) Release();
#else
    (void)window;
    const bool want = g_look && !ui::Open() && app::Focused();
    if (want || g_held.load()) app::HoldMouse(want);
#endif
    if (want != g_held.exchange(want)) {
        Clear();
        LOGI("mouse: look %s", want ? "holds the cursor" : "lets the cursor go");
    }
}

void Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
    switch (msg) {
#ifdef _WIN32
        case WM_INPUT: {
            if (!g_held.load()) break;
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
                    UINT(-1) ||
                raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                break;  // absolute motion is a tablet or a remote desktop, not a mouse to look with
            Record(int32_t(raw.data.mouse.lLastX), int32_t(raw.data.mouse.lLastY));
            break;
        }
#else
        case app::kMsgMouseMotion:
            if (g_held.load()) Record(int32_t(wp), int32_t(lp));
            break;
#endif
        case app::kMsgActivate:
            if ((wp & 0xFFFF) == 0) {  // WA_INACTIVE
                if (g_held.exchange(false)) Release(), Clear();
                break;
            }
            Refresh(hwnd);
            break;
        case app::kMsgSize:
        case app::kMsgMove:
        case kRefresh:
            Refresh(hwnd);
            break;
        default:
            break;
    }
}

namespace {

// the guest's side, under g_pad_lock: the debt in seconds of the stick's full turn, and this frame's step
std::mutex g_pad_lock;
struct Axis {
    const std::array<float, kSteps>& rate;
    bool fast;  // has the fast turn past kFastStep
    float max_debt;
    float debt = 0;  // degrees
    int step = 0;  // signed
    float fast_s = 0;  // how long the fast turn has run
    void Reset() { debt = 0, step = 0, fast_s = 0; }
};
Axis g_x{kYawRate, true, kMaxYawDebt}, g_y{kPitchRate, false, kMaxPitchDebt};
int64_t g_frame_us = 0;  // when this frame's first poll came
float g_frame = 1.0f / 30.0f;  // seconds a frame lasts, smoothed

// a step on each axis as a right stick, aimed at the middle of the step so the game's truncations land on it
void Stick(input::Pad& pad, int x, int y) {
    const float px = x ? std::copysign((float(std::abs(x)) + 0.5f) * 512.0f / 32767.0f, float(x)) : 0.0f;
    const float py = y ? std::copysign((float(std::abs(y)) + 0.5f) * 512.0f / 32767.0f, float(y)) : 0.0f;
    const float len = std::hypot(px, py);
    if (len == 0.0f) return;
    const float out = (kDeadZone + (1.0f - kDeadZone) * len) / len;
    pad.rx = int16_t(std::clamp(px * out, -1.0f, 1.0f) * 32767.0f);
    pad.ry = int16_t(std::clamp(py * out, -1.0f, 1.0f) * 32767.0f);
}

// the step whose turn this frame comes nearest the debt, and the debt it leaves
void Pay(Axis& a, bool resting) {
    const float want = std::fabs(a.debt) / g_frame;
    auto rate = [&](int k) { return a.fast && k >= kFastStep ? FastRate(k, a.fast_s) : a.rate[k]; };
    int k = 0;
    for (int i = 1; i < kSteps; i++)
        if (std::fabs(want - rate(i)) < std::fabs(want - rate(k))) k = i;
    const float turn = rate(k);
    a.fast_s = a.fast && k >= kFastStep ? a.fast_s + g_frame : 0.0f;
    if (k == 0) {
        a.step = 0;
        if (resting) a.debt = 0;
        return;
    }
    a.step = a.debt < 0 ? -k : k;
    a.debt -= std::copysign(turn * g_frame, a.debt);
}

// XBE_MOUSE_CALIBRATE and F9: holds the stick at a run of steps, x and then y, kHoldUs each with kGapUs between,
// and logs how far the player camera turns: its yaw (+0x60) for x; for y the pitch the stick sums (+0x970) and the
// view's pitch (+0x34). Each step logs the rate over the hold's first tenth of a second (how the turn ramps up) and
// over its second half (its steady speed), in degrees a second, and the coast, how far it turned after the stick
// let go. y alternates up and down so the pitch stays inside its limits. Stand still in third person and leave the
// mouse and the stick alone; F9 again stops. Under g_pad_lock.
constexpr uint32_t kPlayer = 0x8470DC, kPlayerCamera = 0x4;
constexpr uint32_t kYaw = 0x60, kYawCopy = 0x5C, kPitch = 0x970, kPitchCopy = 0x980, kView = 0x34;
constexpr int kLevels[] = {1, 4, 8, 12, 16, 20, 22, 24, 26, 28, 30, 32, 36, 40, 44, 48, 52, 56, 60, 63};
constexpr int kLevelCount = int(std::size(kLevels));
constexpr int64_t kHoldUs = 1000000, kGapUs = 700000;

bool Readable(uint32_t va, uint32_t size) { return va && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1); }

uint32_t R32(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, mem::Base() + va, 4);
    return v;
}

float RF(uint32_t va) {
    float f;
    std::memcpy(&f, mem::Base() + va, 4);
    return f;
}

// the player camera, or 0 when it is not there or does not look like one: each angle sane and next to its copy
uint32_t Camera() {
    if (!Readable(kPlayer, 4)) return 0;
    const uint32_t player = R32(kPlayer);
    if (!Readable(player + kPlayerCamera, 4)) return 0;
    const uint32_t cam = R32(player + kPlayerCamera);
    if (!Readable(cam, kPitchCopy + 4)) return 0;
    const float yaw = RF(cam + kYaw), yaw_copy = RF(cam + kYawCopy);
    const float pitch = RF(cam + kPitch), pitch_copy = RF(cam + kPitchCopy), view = RF(cam + kView);
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(view) || std::fabs(yaw) > 1e5f ||
        std::fabs(pitch) > 90.0f || std::fabs(view) > 90.0f)
        return 0;
    const float yaw_apart = std::remainder(yaw - yaw_copy, 360.0f);  // the copy may be on the other side of a wrap
    if (std::fabs(yaw_apart) > 30.0f || std::fabs(pitch - pitch_copy) > 30.0f) return 0;
    return cam;
}

struct Sweep {
    int index = -1;  // into x's levels then y's; -1 not sweeping
    int64_t start = 0;  // the hold's start
    float last[3] = {};  // yaw, summed pitch, view as last read
    double turned[3] = {};  // each since the hold's start, unwrapped
    // the turn at a tenth of a second, half the hold, its end and the gap's end, and when each was taken
    double mark[4][3] = {};
    int64_t mark_us[4] = {};
    int frames = 0;  // polls a frame apart during the hold
    int64_t frame_us = 0;
    int64_t seen_us = 0;  // the camera last read
};
Sweep g_sweep;

void Read(uint32_t cam, float (&a)[3]) { a[0] = RF(cam + kYaw), a[1] = RF(cam + kPitch), a[2] = RF(cam + kView); }

void Begin(uint32_t cam, int index, int64_t now) {
    g_sweep = {};
    g_sweep.index = index;
    g_sweep.start = g_sweep.seen_us = now;
    Read(cam, g_sweep.last);
}

// XBE_MOUSE_CALIBRATE=2 and F9: the stick's fast turn past step ~42, frame by frame. Holds each kProbe segment and
// logs every frame's yaw change; the last runs drop out of the fast turn for a frame or a few to see what resets it.
struct Segment {
    const char* tag;
    int step;
    int64_t us;
};
constexpr Segment kProbe[] = {
    {"41", 41, 800000},        {"rest", 0, 500000}, {"42", 42, 800000},        {"rest", 0, 500000},
    {"43", 43, 800000},        {"rest", 0, 500000}, {"44", 44, 800000},        {"rest", 0, 500000},
    {"46", 46, 800000},        {"rest", 0, 500000}, {"63", 63, 1200000},       {"rest", 0, 500000},
    {"63", 63, 800000},        {"dip 40 x1", 40, 10000}, {"63 after", 63, 400000}, {"rest", 0, 500000},
    {"63", 63, 800000},        {"dip 0 x1", 0, 10000},   {"63 after", 63, 400000}, {"rest", 0, 500000},
    {"63", 63, 800000},        {"dip 40 x4", 40, 60000}, {"63 after", 63, 400000}, {"rest", 0, 500000},
    {"40", 40, 500000},        {"63 from 40", 63, 600000}, {"rest", 0, 500000},
};

struct ProbeState {
    int index = -1;
    int64_t start = 0, frame_us = 0;
    bool hold = false;  // the segment's one-frame dip has had its frame
    float last = 0;
};
ProbeState g_probe;

bool Probe(input::Pad& pad, int64_t now, uint32_t cam, bool pressed) {
    ProbeState& p = g_probe;
    if (pressed) {
        if (p.index >= 0) {
            p.index = -1;
            LOGR("mouse: probe stopped");
            return false;
        }
        if (!cam) {
            LOGR("mouse: probe needs the player camera (stand in a level, third person)");
            return false;
        }
        LOGR("mouse: probing the fast turn at %u fps; leave the mouse and the stick alone (F9 stops)",
             display::GameRate());
        p = {0, now, 0, false, RF(cam + kYaw)};
    }
    if (p.index < 0) return false;
    if (!cam) return true;
    // a poll at least 4 ms after the last is a new frame (the game polls more than once a frame)
    if (now - p.frame_us >= 4000) {
        if (now - p.start >= kProbe[p.index].us && (kProbe[p.index].us > 20000 || p.hold)) {
            if (++p.index == int(std::size(kProbe))) {
                p.index = -1;
                LOGR("mouse: probe done");
                return false;
            }
            p.start = now, p.hold = false;
        }
        const float yaw = RF(cam + kYaw);
        const float d = std::remainder(yaw - p.last, 360.0f);
        p.last = yaw;
        if (p.frame_us)
            LOGR("mouse: probe %-10s t %5.3f step %2d  yaw/frame %8.3f", kProbe[p.index].tag,
                 double(now - p.start) * 1e-6, kProbe[p.index].step, d);
        p.frame_us = now;
        if (kProbe[p.index].us <= 20000) p.hold = true;  // a dip lasts exactly one frame
    }
    const int step = kProbe[p.index].step;
    pad.rx = pad.ry = 0;
    Stick(pad, step, 0);
    return true;
}

bool Calibrate(input::Pad& pad, int64_t now) {
    static const char* env = std::getenv("XBE_MOUSE_CALIBRATE");
    if (!env) return false;
    static bool f9 = false;
    const bool down = app::KeyDown(0x78);  // F9
    const bool pressed = down && !f9;
    f9 = down;
    Sweep& s = g_sweep;
    const uint32_t cam = Camera();
    if (env[0] == '2') return Probe(pad, now, cam, pressed);
    if (pressed && s.index >= 0) {
        s.index = -1;
        LOGR("mouse: calibration stopped");
        return false;
    }
    if (pressed) {
        if (!cam) {
            LOGR("mouse: calibration needs the player camera (stand in a level, third person)");
            return false;
        }
        LOGR("mouse: calibrating at %u fps; leave the mouse and the stick alone (F9 stops)", display::GameRate());
        Begin(cam, 0, now);
    }
    if (s.index < 0) return false;
    if (!cam) {  // a frame or two without it is let pass
        if (now - s.seen_us < 500000) return true;
        s.index = -1;
        LOGR("mouse: calibration lost the player camera");
        return false;
    }
    s.seen_us = now;

    float cur[3];
    Read(cam, cur);
    for (int i = 0; i < 3; i++) {
        float d = cur[i] - s.last[i];
        d -= 360.0f * std::round(d / 360.0f);  // the yaw may wrap
        s.turned[i] += d;
        s.last[i] = cur[i];
    }
    const int64_t t = now - s.start;
    constexpr int64_t kMarks[4] = {100000, kHoldUs / 2, kHoldUs, kHoldUs + kGapUs};
    for (int m = 0; m < 4; m++)
        if (t >= kMarks[m] && !s.mark_us[m]) {
            s.mark_us[m] = t;
            for (int i = 0; i < 3; i++) s.mark[m][i] = s.turned[i];
        }
    if (t < kHoldUs && now - s.frame_us >= 4000) s.frames++, s.frame_us = now;

    const bool y = s.index >= kLevelCount;
    const int level = kLevels[s.index % kLevelCount];
    if (s.mark_us[3]) {
        const double ramp = 1e6 / double(s.mark_us[0]), steady = 1e6 / double(s.mark_us[2] - s.mark_us[1]);
        auto rate0 = [&](int i) { return s.mark[0][i] * ramp; };
        auto rate1 = [&](int i) { return (s.mark[2][i] - s.mark[1][i]) * steady; };
        auto coast = [&](int i) { return s.mark[3][i] - s.mark[2][i]; };
        if (!y)
            LOGR("mouse: calibrate x step %2d  yaw %8.2f deg/s (first 0.1 s %8.2f)  coast %7.2f  frames %d", level,
                 rate1(0), rate0(0), coast(0), s.frames);
        else
            LOGR("mouse: calibrate y step %+3d  sum %8.2f deg/s (first 0.1 s %8.2f) coast %7.2f  view %8.2f deg/s "
                 "(first 0.1 s %8.2f) coast %7.2f  view now %7.2f  frames %d",
                 s.index % 2 ? -level : level, rate1(1), rate0(1), coast(1), rate1(2), rate0(2), coast(2), cur[2],
                 s.frames);
        if (s.index + 1 == 2 * kLevelCount) {
            s.index = -1;
            LOGR("mouse: calibration done");
            return false;
        }
        Begin(cam, s.index + 1, now);
        return true;
    }
    if (t < kHoldUs) {
        const int step = y && s.index % 2 ? -level : level;
        Stick(pad, y ? 0 : step, y ? step : 0);
    } else {
        pad.rx = pad.ry = 0;
    }
    return true;
}


void WF(uint32_t va, float f) { std::memcpy(mem::Base() + va, &f, 4); }

// when Turn last turned a camera; while it does, Pad leaves the stick alone
std::atomic<int64_t> g_turned_us{0};
constexpr int64_t kTurnedUs = 250000;

}  // namespace

// 0x2F99A0, the camera's turn by the stick (see the hook at the end of the file), after it has run: this frame's
// motion, for player 1's actions, added to the turn it worked out. The callers take that turn two ways. Most apply
// the frame's turns, the yaw's at +0x08 (taken off their yaw) and the pitch's at +0x44, to angles of their own (the
// third person camera at [player+4]+0x5C, and others with their own limits); the mouse is added there, in degrees.
// The rest keep the angles 0x2F99A0 turns, the yaw at +0x04 and the pitch at +0x40 (the weapon camera's pitch): the
// mouse moves those too, the pitch only as far as the camera's limits allow, and the eased values the view follows
// (+0x10, +0x4C) as far, so the mouse turns at once while the stick's turn eases as the game has it. A caller takes
// one or the other, never both. The motion is taken once a frame, and every camera turned in that frame gets it.
void Turn(uint32_t actions, uint32_t turn) {
    constexpr uint32_t kPlayer1Actions = 0x763260;
    constexpr uint32_t kYaw = 0x04, kYawTurn = 0x08, kYawEased = 0x10;
    constexpr uint32_t kPitch = 0x40, kPitchTurn = 0x44, kPitchEased = 0x4C;
    constexpr uint32_t kPitchMin = 0x18, kPitchMax = 0x1C;  // in the camera's settings, [turn]
    if (actions != kPlayer1Actions || !g_look || !g_held.load() || !Readable(turn, kPitchEased + 4)) return;
    const uint32_t settings = R32(turn);
    if (!Readable(settings, kPitchMax + 4)) return;

    // a new frame takes the motion since the last; the cameras turned within it share it
    static std::mutex frame_lock;
    static int64_t frame_us = 0;
    static int64_t frame_dx = 0, frame_dy = 0;
    static uint32_t served[8] = {};
    static int served_count = 0;
    int64_t dx, dy;
    {
        std::lock_guard frame(frame_lock);
        const int64_t now = Now();
        g_turned_us.store(now, std::memory_order_relaxed);
        if (now - frame_us >= 4000) {
            std::lock_guard motion(g_lock);
            frame_dx = g_dx, frame_dy = g_dy;
            g_dx = g_dy = 0;
            frame_us = now;
            served_count = 0;
        }
        for (int i = 0; i < served_count; i++)
            if (served[i] == turn) return;
        if (served_count < int(std::size(served))) served[served_count++] = turn;
        dx = frame_dx, dy = frame_dy;
    }
    if (!dx && !dy) return;
    LOG_FIRST(1, "mouse: look turns the camera");

    // counts into degrees: right lowers the yaw, up raises the pitch (as the stick does); raw y grows downward
    const float yaw = float(dx) * kDegreesPerCount * float(g_sensitivity) / 100.0f;
    float pitch = float(-dy) * kDegreesPerCount * float(g_sensitivity_y) / 100.0f;
    if (g_invert_y) pitch = -pitch;
    // the frame's turns, which come off the yaw and the pitch
    WF(turn + kYawTurn, RF(turn + kYawTurn) + yaw);
    WF(turn + kPitchTurn, RF(turn + kPitchTurn) - pitch);
    // the angles kept here, and their eased values
    float y = RF(turn + kYaw) - yaw;
    while (y < 0.0f) y += 360.0f;
    while (y >= 360.0f) y -= 360.0f;
    WF(turn + kYaw, y);
    WF(turn + kYawEased, RF(turn + kYawEased) - yaw);
    const float lo = RF(settings + kPitchMin), hi = RF(settings + kPitchMax), was = RF(turn + kPitch);
    const float now = lo <= hi ? std::clamp(was + pitch, std::min(lo, was), std::max(hi, was)) : was + pitch;
    WF(turn + kPitch, now);
    WF(turn + kPitchEased, RF(turn + kPitchEased) + (now - was));
}

namespace {

}  // namespace

void Pad(input::Pad& pad) {
    if (input::t_presence) return;
    const int64_t now = Now();
    std::lock_guard lock(g_pad_lock);
    const bool calibrating = Calibrate(pad, now);
    if (calibrating || !g_look || !g_held.load()) {
        g_x.Reset(), g_y.Reset();
        if (!calibrating) Clear();
        return;
    }
    // a camera Turn reaches takes the motion itself; the stick is for the rest
    if (now - g_turned_us.load(std::memory_order_relaxed) < kTurnedUs) {
        g_x.Reset(), g_y.Reset();
        return;
    }

    // the first poll of a frame: hand this frame's motion over and take its steps
    const float expect = 1.0f / float(display::GameRate());
    if (now - g_frame_us >= int64_t(expect * 0.5e6f)) {
        if (g_frame_us) g_frame += (std::clamp(float(now - g_frame_us) * 1e-6f, expect * 0.5f, 0.1f) - g_frame) * 0.1f;
        g_frame_us = now;
        int64_t dx, dy;
        bool resting;
        {
            std::lock_guard motion(g_lock);
            dx = g_dx, dy = g_dy;
            g_dx = g_dy = 0;
            resting = now - g_moved_us > kRestUs;
        }
        // counts into degrees; raw y grows downward
        float y = float(-dy) * kDegreesPerCount * float(g_sensitivity_y) / 100.0f;
        if (g_invert_y) y = -y;
        g_x.debt = std::clamp(g_x.debt + float(dx) * kDegreesPerCount * float(g_sensitivity) / 100.0f, -g_x.max_debt, g_x.max_debt);
        g_y.debt = std::clamp(g_y.debt + y, -g_y.max_debt, g_y.max_debt);
        Pay(g_x, resting);
        Pay(g_y, resting);
    }
    if (!g_x.step && !g_y.step) return;  // still: the keyboard's or a gamepad's right stick stands
    Stick(pad, g_x.step, g_y.step);
    LOG_FIRST(1, "mouse: look drives the right stick");
}

}  // namespace mouse

#else  // no window or mouse here (the headless harness)

namespace mouse {

bool g_look = false;
uint32_t g_sensitivity = kDefaultSensitivity;
uint32_t g_sensitivity_y = kDefaultSensitivity;
bool g_invert_y = false;

void Message(void*, uint32_t, uintptr_t, intptr_t) {}

void Refresh(void*) {}

void Pad(input::Pad&) {}

void Turn(uint32_t, uint32_t) {}

}  // namespace mouse

#endif

// 0x2F99A0 (actions in eax, the camera's turn in ecx, the frame's time on the stack): turns a camera by the right
// stick, for the third person, first person and weapon cameras. The stick, through the curve at 0x747798, sets a rate
// (faster past 0.8 of full, up to config+0x10 times as the stick is held), and the frame's turn comes off the yaw at
// +0x04 (wrapped to 0..360) and the pitch at +0x40 (held inside config+0x18..+0x1C); springs at +0x0C and +0x48 ease
// the view after them (+0x10, +0x4C). The frame's turns stay at +0x08 and +0x44 for the callers. The mouse is added
// to what it worked out (mouse::Turn).
X86_EXTERN_FUNC(__imp__sub_002F99A0);
X86_FUNC(sub_002F99A0) {
    const uint32_t actions = ctx.eax, turn = ctx.ecx;
    __imp__sub_002F99A0(ctx, base);
    mouse::Turn(actions, turn);
}
