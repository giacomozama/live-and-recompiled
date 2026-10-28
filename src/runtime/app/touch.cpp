// The on-screen controller for phones and tablets: two sticks that appear where a thumb lands (the left one on the
// left of the screen, the right one anywhere else free), A B X Y, black and white, the triggers, the d-pad, Back and
// Start, and a button that opens the menu. Its report goes to the first player while no controller drives that
// player; a controller plugged in hides it, the menu hides it too (its own taps are ImGui's, from the mouse SDL makes
// of them).
//
// The layout is in the overlay's units (ImGui's display size, the window's size in points, which SDL's finger
// positions are fractions of), sized by the screen's short side so it is the same under the thumbs on any screen.
#include "app/touch.h"

#ifdef __ANDROID__

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "kernel/kernel.h"

namespace touch {

namespace {

// the controls: the pad's digital buttons by their bit, the analog ones (A B X Y black white LT RT) after them
enum Control : int {
    kUp, kDown, kLeft, kRight, kStart, kBack,
    kA = 8, kB, kX, kY, kBlack, kWhite, kLt, kRt,
    kMenu = 16, kControls,
    kLeftStick = 32, kRightStick,
};

struct Button {
    int control;
    const char* label;
    float x, y, r;  // centre and radius, in the layout's units
};

struct Finger {
    SDL_FingerID id = 0;
    int control = -1;  // a button, a stick, -1 free
    float x0 = 0, y0 = 0, x = 0, y = 0;  // where it landed and where it is, in points
};

std::mutex g_lock;
Finger g_fingers[10];
float g_w = 0, g_h = 0;  // the window, in points, as of the last finger
bool g_shown = false;

constexpr ImU32 kFill = IM_COL32(255, 255, 255, 40), kPressed = IM_COL32(255, 255, 255, 110),
                kLine = IM_COL32(255, 255, 255, 90), kText = IM_COL32(255, 255, 255, 170);

// the unit everything is measured in, a tenth of the short side
float Unit(float w, float h) { return std::min(w, h) / 10; }

// the buttons where they sit on a w by h screen
std::vector<Button> Layout(float w, float h) {
    float u = Unit(w, h);
    float cx = w - 2.3f * u, cy = h - 2.3f * u;  // the face buttons' diamond
    float dx = 1.9f * u, dy = h * 0.40f;          // the d-pad's
    return {
        {kA, "A", cx, cy + 1.0f * u, 0.62f * u},
        {kB, "B", cx + 1.0f * u, cy, 0.62f * u},
        {kX, "X", cx - 1.0f * u, cy, 0.62f * u},
        {kY, "Y", cx, cy - 1.0f * u, 0.62f * u},
        {kWhite, "W", cx - 1.6f * u, cy - 2.0f * u, 0.45f * u},
        {kBlack, "Bk", cx + 0.2f * u, cy - 2.4f * u, 0.45f * u},
        {kLt, "LT", 1.3f * u, 1.3f * u, 0.75f * u},
        {kRt, "RT", w - 1.3f * u, 1.3f * u, 0.75f * u},
        {kUp, "", dx, dy - 0.75f * u, 0.42f * u},
        {kDown, "", dx, dy + 0.75f * u, 0.42f * u},
        {kLeft, "", dx - 0.75f * u, dy, 0.42f * u},
        {kRight, "", dx + 0.75f * u, dy, 0.42f * u},
        {kBack, "Back", w / 2 - 1.5f * u, 0.7f * u, 0.45f * u},
        {kStart, "Start", w / 2 + 1.5f * u, 0.7f * u, 0.45f * u},
        {kMenu, "Menu", w / 2, 0.7f * u, 0.45f * u},
    };
}

// the stick that appears under a thumb landing at x, y: the left one in the lower left, the right one elsewhere
int StickAt(float x, float y, float w, float h) { return x < w * 0.4f && y > h * 0.5f ? kLeftStick : kRightStick; }

// shown while no controller drives the first player and the menu is shut
bool Shown() { return !input::Has(0, input::Kind::Controller) && !kernel::Held(); }

Finger* Find(SDL_FingerID id) {
    for (Finger& f : g_fingers)
        if (f.control != -1 && f.id == id) return &f;
    return nullptr;
}

// a stick's deflection, -1 to 1 each way, y up; the full throw is a unit and a third from where the thumb landed
void Deflection(const Finger& f, float& x, float& y) {
    float range = Unit(g_w, g_h) * 1.33f;
    x = (f.x - f.x0) / range;
    y = (f.y0 - f.y) / range;
    float len = std::sqrt(x * x + y * y);
    if (len > 1) x /= len, y /= len;
}

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767); }

}  // namespace

void Init() { SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1"); }

bool Handle(const void* sdl_event) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(sdl_event);
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
        e.type != SDL_EVENT_FINGER_CANCELED)
        return false;
    int w = 0, h = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(app::Window()), &w, &h);
    std::lock_guard lock(g_lock);
    g_w = float(w);
    g_h = float(h);
    g_shown = Shown();
    float x = e.tfinger.x * g_w, y = e.tfinger.y * g_h;
    if (!g_shown) {
        for (Finger& f : g_fingers) f.control = -1;
        return false;
    }
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        Finger* f = std::find_if(std::begin(g_fingers), std::end(g_fingers), [](const Finger& f) { return f.control == -1; });
        if (f == std::end(g_fingers)) return false;
        *f = {e.tfinger.fingerID, StickAt(x, y, g_w, g_h), x, y, x, y};
        for (const Button& b : Layout(g_w, g_h)) {
            // a little past the drawn circle: thumbs land wide
            float r = b.r * 1.25f;
            if ((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y) <= r * r) f->control = b.control;
        }
        return f->control == kMenu;
    }
    Finger* f = Find(e.tfinger.fingerID);
    if (!f) return false;
    f->x = x;
    f->y = y;
    if (e.type != SDL_EVENT_FINGER_MOTION) f->control = -1;
    return false;
}

void Merge(int port, input::Pad& pad) {
    if (port != 0) return;
    std::lock_guard lock(g_lock);
    if (!g_shown && !Shown()) return;
    pad.connected = true;
    for (const Finger& f : g_fingers) {
        if (f.control == -1) continue;
        if (f.control < 8) {
            pad.buttons |= uint16_t(1 << f.control);
        } else if (f.control < kMenu) {
            pad.analog[f.control - kA] = 0xFF;
        } else if (f.control == kLeftStick || f.control == kRightStick) {
            float x, y;
            Deflection(f, x, y);
            // not plain negation, as input::Poll has it: the console's range is -32768 to 32767
            (f.control == kLeftStick ? pad.lx : pad.rx) = Axis(x);
            (f.control == kLeftStick ? pad.ly : pad.ry) = Axis(y);
        }
    }
}

void Draw() {
    std::lock_guard lock(g_lock);
    g_shown = Shown();
    if (!g_shown) return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0 || size.y <= 0) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float u = Unit(size.x, size.y);
    auto held = [&](int control) {
        return std::any_of(std::begin(g_fingers), std::end(g_fingers), [&](const Finger& f) { return f.control == control; });
    };
    for (const Button& b : Layout(size.x, size.y)) {
        ImVec2 c(b.x, b.y);
        dl->AddCircleFilled(c, b.r, held(b.control) ? kPressed : kFill);
        dl->AddCircle(c, b.r, kLine, 0, 2);
        if (b.control == kMenu) {
            for (int i = -1; i <= 1; i++)
                dl->AddLine(ImVec2(c.x - b.r * 0.45f, c.y + i * b.r * 0.32f), ImVec2(c.x + b.r * 0.45f, c.y + i * b.r * 0.32f),
                            kText, 2);
            continue;
        }
        if (!*b.label) continue;
        float fs = b.r * (std::strlen(b.label) > 2 ? 0.55f : 0.8f);
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, b.label);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), kText, b.label);
    }
    // the sticks under their thumbs, and where the left one is when no thumb holds it
    bool left = false;
    for (const Finger& f : g_fingers) {
        if (f.control != kLeftStick && f.control != kRightStick) continue;
        left |= f.control == kLeftStick;
        float x, y;
        Deflection(f, x, y);
        float range = u * 1.33f;
        dl->AddCircleFilled(ImVec2(f.x0, f.y0), range, kFill);
        dl->AddCircle(ImVec2(f.x0, f.y0), range, kLine, 0, 2);
        dl->AddCircleFilled(ImVec2(f.x0 + x * range, f.y0 - y * range), u * 0.55f, kPressed);
    }
    if (!left) {
        ImVec2 c(2.6f * u, size.y - 2.6f * u);
        dl->AddCircle(c, u * 1.33f, kLine, 0, 2);
        dl->AddCircleFilled(c, u * 0.55f, kFill);
    }
}

}  // namespace touch

#else  // only Android shows one

namespace touch {

void Init() {}
bool Handle(const void*) { return false; }
void Draw() {}
void Merge(int, input::Pad&) {}

}  // namespace touch

#endif
