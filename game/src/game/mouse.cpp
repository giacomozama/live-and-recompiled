// Mouse look: the mouse turns the camera as the right stick does.
//
// The camera's own yaw and pitch are not mapped, so the mouse drives the stick the game already reads. The window
// thread takes raw mouse motion (WM_INPUT, unaccelerated counts) into a timestamped list; keyboard::Pad, on the
// guest's threads, turns the motion of the last game frame into a stick deflection, faster motion pushing it further.
// Pad only reads the list and never drains it: the pad is polled from more than one place each frame (the device
// scan in xapi.cpp polls every port too), and each poll sees the same motion.
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
#include <mutex>

#include "common/display.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"

namespace mouse {

bool g_look = false;
uint32_t g_sensitivity = 100;
uint32_t g_sensitivity_y = 100;
bool g_invert_y = false;

namespace {

// counts a second that push the stick all the way at 100% sensitivity
constexpr float kFullSpeed = 2000.0f;
// the least deflection any motion gives, so slow motion is not lost in the game's stick dead zone
constexpr float kMinDeflection = 0.25f;

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

int64_t Now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Clear() {
    std::lock_guard lock(g_lock);
    for (Motion& m : g_motion) m = {};
    g_next = 0;
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
    float x = float(dx) * scale, y = float(-dy) * scale * float(g_sensitivity_y) / 100.0f;  // raw y grows downward
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
uint32_t g_sensitivity_y = 100;
bool g_invert_y = false;

void Message(void*, uint32_t, uintptr_t, intptr_t) {}

void Refresh(void*) {}

void Pad(input::Pad&) {}

}  // namespace mouse

#endif
