// Mouse look: the mouse turns the camera.
//
// The window thread takes raw mouse motion (WM_INPUT, unaccelerated counts). keyboard::Pad, on the guest's threads,
// hands it to the game one of two ways:
//
// - straight into the player camera's angles, in degrees, when the camera is found (Turn). XBE_CAMSCAN found them:
//   the camera is [[0x8470DC] + 4] (the player array, 0x770 apart, then its camera), yaw at +0x60 and pitch at
//   +0x970, which the game copies every frame (yaw to +0x5C and +0x124, pitch to +0x97C, +0x980 and +0x9BC). Stick
//   right and stick up both lower them. Yaw keeps what is written; pitch the game eases back to its rest once the
//   stick is let go, so mouse look holds its own (see Turn).
// - else as right stick, for any other camera: the motion of the last game frame becomes a deflection, faster
//   motion pushing it further.
//
// The pad is polled from more than one place each frame (the device scan in xapi.cpp polls every port too). Turn
// applies the motion since the last poll that applied any, so it lands once whichever poll takes it; Stick only reads
// a timestamped list and never drains it, so each poll sees the same deflection.
//
// While mouse look is on, the game window has the focus and the F1 menu is shut, the cursor is hidden and held
// inside the window. Alt-tab or F1 lets it go.
//
// On the headless harness there is no window and no mouse, so everything but the option globals compiles out.
#include "common/mouse.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include "common/display.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace mouse {

bool g_look = false;
uint32_t g_sensitivity = 100;
bool g_invert_y = false;

namespace {

// counts a second that push the stick all the way at 100% sensitivity
constexpr float kFullSpeed = 2000.0f;
// the least deflection any motion gives, so slow motion is not lost in the game's stick dead zone
constexpr float kMinDeflection = 0.25f;
// degrees a count turns the camera at 100% sensitivity: 800 counts (an inch at 800 dpi) is about 32 degrees
constexpr float kDegreesPerCount = 0.04f;
// a stick past this is tilting the camera (XInput's right stick dead zone); a gamepad at rest sits inside it
constexpr int kStickDeadZone = 8689;
// pitch is held inside this until the game shows a tighter limit; the stick reached 55
constexpr float kPitchLimit = 55.0f;

// the player's object, its camera, and the camera's angles (see the top of the file)
constexpr uint32_t kPlayer = 0x8470DC, kPlayerCamera = 0x4;
constexpr uint32_t kYaw = 0x60, kYawCopy = 0x5C, kPitch = 0x970, kPitchCopy = 0x980;

std::atomic<bool> g_held{false};
bool g_registered = false;
bool g_hidden = false;

struct Motion {
    int64_t us;
    int32_t dx, dy;
};
std::mutex g_lock;
Motion g_motion[512];  // a ring; a 1000 Hz mouse fills ~33 of these a 30 fps frame
uint32_t g_next = 0;
int64_t g_total_x = 0, g_total_y = 0;      // every count while held
int64_t g_applied_x = 0, g_applied_y = 0;  // the counts Turn has handed the camera, or skipped

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

void WF(uint32_t va, float f) { std::memcpy(mem::Base() + va, &f, 4); }

// the player camera, or 0 when it is not there or does not look like one: each angle sane and next to its copy
uint32_t Camera() {
    if (!Readable(kPlayer, 4)) return 0;
    const uint32_t player = R32(kPlayer);
    if (!Readable(player + kPlayerCamera, 4)) return 0;
    const uint32_t cam = R32(player + kPlayerCamera);
    if (!Readable(cam, kPitchCopy + 4)) return 0;
    const float yaw = RF(cam + kYaw), yaw_copy = RF(cam + kYawCopy);
    const float pitch = RF(cam + kPitch), pitch_copy = RF(cam + kPitchCopy);
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || std::fabs(yaw) > 1e5f || std::fabs(pitch) > 90.0f) return 0;
    if (std::fabs(yaw - yaw_copy) > 30.0f || std::fabs(pitch - pitch_copy) > 30.0f) return 0;
    return cam;
}

int64_t Now();

// the pitch mouse look holds (see Turn); guest threads, under g_turn_lock
std::mutex g_turn_lock;
struct Pitch {
    uint32_t cam = 0;  // the camera it was taken from
    float hold = 0, written = 0, seen = 0;
    float lo = -kPitchLimit, hi = kPitchLimit;
    int64_t us = 0;  // when it was last written
    int pinned = 0;  // polls in a row the game held it short of what was written
} g_pitch;

// the motion since the last call straight into the camera's angles; false when there is no camera to turn.
//
// Yaw takes the motion as it comes. Pitch the game eases back to its rest when the stick is let go, so a pitch the
// mouse sets would drift away: mouse look keeps its own and writes it every poll. It takes the game's back when the
// keyboard or a gamepad tilts the camera (stick_y), when the game moves it far (another camera mode), after a pause
// in the polls, and when the camera changes; and it learns the game's limits from the game holding it short.
bool Turn(bool stick_y) {
    int64_t dx, dy;
    {
        std::lock_guard lock(g_lock);
        dx = g_total_x - g_applied_x;
        dy = g_total_y - g_applied_y;
        g_applied_x = g_total_x;
        g_applied_y = g_total_y;
    }
    static std::atomic<uint32_t> last{~0u};
    const uint32_t cam = Camera();
    if (last.exchange(cam) != cam) {
        if (cam) LOGI("mouse: look turns the camera at %08X (yaw %.1f, pitch %.1f)", cam, RF(cam + kYaw), RF(cam + kPitch));
        else LOGI("mouse: no player camera, look drives the right stick");
    }
    if (!cam) return false;
    const float k = kDegreesPerCount * float(g_sensitivity) / 100.0f;
    // mouse right turns as the stick right does, mouse up tilts as the stick up does: both lower the angle
    if (dx) WF(cam + kYaw, RF(cam + kYaw) - float(dx) * k);

    std::lock_guard lock(g_turn_lock);
    Pitch& p = g_pitch;
    const float game = RF(cam + kPitch);
    const int64_t now = Now();
    if (p.cam != cam) p = {}, p.cam = cam;
    const bool fresh = !p.us || now - p.us > 500000 || stick_y || std::fabs(game - p.written) > 20.0f;
    if (!fresh && std::fabs(game - p.written) > 2.0f && game == p.seen) {
        // held short of what was written, the same each poll: the game's limit
        if (++p.pinned >= 3) {
            if (p.written > game) p.hi = game;
            else p.lo = game;
        }
    } else {
        p.pinned = 0;
    }
    p.seen = game;
    if (fresh) p.hold = game;
    if (stick_y) {
        p.us = now;
        p.written = game;
        return true;
    }
    p.hold = std::clamp(p.hold + float(g_invert_y ? -dy : dy) * k, p.lo, p.hi);
    WF(cam + kPitch, p.hold);
    p.written = p.hold;
    p.us = now;
    return true;
}

int64_t Now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Clear() {
    std::lock_guard lock(g_lock);
    for (Motion& m : g_motion) m = {};
    g_next = 0;
    g_applied_x = g_total_x;
    g_applied_y = g_total_y;
}

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

}  // namespace

void Refresh(void* window) {
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
    if (want != g_held.exchange(want)) {
        Clear();
        LOGI("mouse: look %s", want ? "holds the cursor" : "lets the cursor go");
    }
}

void Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
    switch (msg) {
        case WM_INPUT: {
            if (!g_held.load()) break;
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
                    UINT(-1) ||
                raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                break;  // absolute motion is a tablet or a remote desktop, not a mouse to look with
            if (!raw.data.mouse.lLastX && !raw.data.mouse.lLastY) break;
            std::lock_guard lock(g_lock);
            g_motion[g_next] = {Now(), int32_t(raw.data.mouse.lLastX), int32_t(raw.data.mouse.lLastY)};
            g_next = (g_next + 1) % std::size(g_motion);
            g_total_x += raw.data.mouse.lLastX;
            g_total_y += raw.data.mouse.lLastY;
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (g_held.exchange(false)) Release(), Clear();
                break;
            }
            Refresh(hwnd);
            break;
        case WM_SIZE:
        case WM_MOVE:
        case kRefresh:
            Refresh(hwnd);
            break;
        default:
            break;
    }
}

void Pad(input::Pad& pad) {
    if (!g_look || !g_held.load()) return;
    // the player camera turns with the mouse (the keyboard's or a gamepad's stick is in pad already)
    if (Turn(std::abs(pad.ry) > kStickDeadZone)) return;
    // the motion of the last game frame
    const float frame = 1.0f / float(std::max<uint32_t>(display::g_frame_rate, 30));
    const int64_t since = Now() - int64_t(frame * 1e6f);
    int64_t dx = 0, dy = 0;
    {
        std::lock_guard lock(g_lock);
        for (const Motion& m : g_motion)
            if (m.us >= since) dx += m.dx, dy += m.dy;
    }
    if (!dx && !dy) return;  // still: the keyboard's or a gamepad's right stick stands

    // counts a second into deflection, then lifted past the dead zone along its own direction
    const float scale = float(g_sensitivity) / 100.0f / kFullSpeed / frame;
    float x = float(dx) * scale, y = float(-dy) * scale;  // raw y grows downward, the stick's up
    if (g_invert_y) y = -y;
    const float len = std::hypot(x, y);
    const float out = kMinDeflection + (1.0f - kMinDeflection) * std::min(len, 1.0f);
    x *= out / len;
    y *= out / len;
    pad.rx = int16_t(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
    pad.ry = int16_t(std::clamp(y, -1.0f, 1.0f) * 32767.0f);
    LOG_FIRST(1, "mouse: look drives the right stick");
}

}  // namespace mouse

#else  // no window or mouse here (the headless harness)

namespace mouse {

bool g_look = false;
uint32_t g_sensitivity = 100;
bool g_invert_y = false;

void Message(void*, uint32_t, uintptr_t, intptr_t) {}

void Refresh(void*) {}

void Pad(input::Pad&) {}

}  // namespace mouse

#endif
