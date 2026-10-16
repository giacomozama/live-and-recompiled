// Keyboard controls, so Conker can be played without a gamepad.
//
// The runtime polls the pad on the guest's threads and hands the result to GameInfo::pad (see
// app.h), the hook a title gets to change a polled pad. This fills port 0 from the host keyboard
// and reports it connected, so the frontend and the game accept input with no controller plugged
// in. A real gamepad on port 0 keeps working: the keyboard only overrides the axes and buttons it
// is actually driving.
//
// The default layout (movement on the left, camera and face buttons on the right):
//
//   left stick   W A S D          camera (right stick)   I J K L
//   d-pad        arrow keys       A / B / X / Y          space / left shift / E / Q
//   LB / RB      1 / 2            LT / RT                3 / 4
//   start        enter            back                   backspace
//   stick clicks C (left)         V (right)
//
// Every action can be rebound, to a key or a mouse button: in the F1 menu, or in settings.toml's [keys]
// table by key name (move_up = 'W', a = 'Mouse 1', 'none' unbinds).
//
// With mouse look on (src/game/mouse.cpp) the mouse also turns the camera, over the camera keys while it moves.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <windows.h>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define KEYBOARD_TOML 1
#endif

#include "common/camscan.h"
#include "common/keyboard.h"
#include "common/mouse.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"

namespace keyboard {

int g_keys[kActions];

namespace {

// Windows virtual-key codes, spelled out rather than taken from <windows.h>: the headless harness
// compiles this file against a small Win32 shim that declares only a few of them.
enum : int {
    kUp = 0x26, kDown = 0x28, kLeft = 0x25, kRight = 0x27,
    kSpace = 0x20, kEnter = 0x0D, kBackspace = 0x08, kLShift = 0xA0, kEscape = 0x1B,
    kF1 = 0x70, kInsert = 0x2D,
};

// the console controller's button bit (see input::Pad)
enum : uint16_t {
    kPadUp = 1 << 0, kPadDown = 1 << 1, kPadLeft = 1 << 2, kPadRight = 1 << 3,
    kPadStart = 1 << 4, kPadBack = 1 << 5, kPadLS = 1 << 6, kPadRS = 1 << 7,
};

// XINPUT left/right thumb fully deflected
constexpr int16_t kFull = 32767;

struct ActionInfo {
    const char* key;    // settings.toml [keys]
    const char* label;  // F1 menu
    int vk;             // default
};
constexpr ActionInfo kInfo[kActions] = {
    {"move_up", "Move up", 'W'},          {"move_down", "Move down", 'S'},
    {"move_left", "Move left", 'A'},      {"move_right", "Move right", 'D'},
    {"camera_up", "Camera up", 'I'},      {"camera_down", "Camera down", 'K'},
    {"camera_left", "Camera left", 'J'},  {"camera_right", "Camera right", 'L'},
    {"dpad_up", "D-pad up", kUp},         {"dpad_down", "D-pad down", kDown},
    {"dpad_left", "D-pad left", kLeft},   {"dpad_right", "D-pad right", kRight},
    {"a", "A", kSpace},                   {"b", "B", kLShift},
    {"x", "X", 'E'},                      {"y", "Y", 'Q'},
    {"lb", "LB (white)", '1'},            {"rb", "RB (black)", '2'},
    {"lt", "LT", '3'},                    {"rt", "RT", '4'},
    {"start", "Start", kEnter},           {"back", "Back", kBackspace},
    {"ls", "Left stick click", 'C'},      {"rs", "Right stick click", 'V'},
};

// the keys that have a name, so they can be bound; null for the rest
const char* Named(int vk) {
    static const auto names = [] {
        struct Names {
            const char* of[0x100] = {};
            char text[0x100][10] = {};  // the generated ones: letters, digits, F keys, numpad digits
        } n;
        auto set = [&](int k, const char* fmt, int i) {
            std::snprintf(n.text[k], sizeof n.text[k], fmt, i);
            n.of[k] = n.text[k];
        };
        for (int k = 'A'; k <= 'Z'; k++) set(k, "%c", k);  // letters and digits are their own codes
        for (int k = '0'; k <= '9'; k++) set(k, "%c", k);
        for (int i = 0; i < 12; i++) set(0x70 + i, "F%d", i + 1);
        for (int i = 0; i < 10; i++) set(0x60 + i, "Numpad %d", i);
        const std::pair<int, const char*> fixed[] = {
            {0x01, "Mouse 1"}, {0x02, "Mouse 2"}, {0x04, "Mouse 3"}, {0x05, "Mouse 4"}, {0x06, "Mouse 5"},
            {0x08, "Backspace"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x14, "Caps Lock"}, {0x1B, "Escape"},
            {0x20, "Space"}, {0x21, "Page Up"}, {0x22, "Page Down"}, {0x23, "End"}, {0x24, "Home"},
            {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"}, {0x2D, "Insert"}, {0x2E, "Delete"},
            {0x6A, "Numpad *"}, {0x6B, "Numpad +"}, {0x6D, "Numpad -"}, {0x6E, "Numpad ."}, {0x6F, "Numpad /"},
            {0xA0, "Left Shift"}, {0xA1, "Right Shift"}, {0xA2, "Left Ctrl"}, {0xA3, "Right Ctrl"},
            {0xA4, "Left Alt"}, {0xA5, "Right Alt"},
            {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"}, {0xC0, "`"},
            {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"},
        };
        for (const auto& [k, name] : fixed) n.of[k] = name;
        return n;
    }();
    return vk > 0 && vk < 0x100 ? names.of[vk] : nullptr;
}

// key names in settings.toml match whatever their case
bool SameName(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (std::tolower(uint8_t(*a)) != std::tolower(uint8_t(*b))) return false;
    return *a == *b;
}

bool Down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool Down(Action a) { return Down(g_keys[a]); }

// one axis from its two keys: keyboard strokes are digital, so a held key is full deflection
int16_t Axis(bool negative, bool positive) {
    return int16_t((positive ? kFull : 0) - (negative ? kFull : 0));
}

[[maybe_unused]] const bool g_defaults = (ResetKeys(), true);  // before Load, at static init

}  // namespace

const char* ActionLabel(int action) { return action >= 0 && action < kActions ? kInfo[action].label : ""; }

const char* KeyName(int vk) { return vk ? Named(vk) : "-"; }

bool Bindable(int vk) { return vk > 0 && vk < 0x100 && vk != kF1 && vk != kInsert && vk != kEscape && Named(vk); }

void ResetKeys() {
    for (int a = 0; a < kActions; a++) g_keys[a] = kInfo[a].vk;
}

#ifdef KEYBOARD_TOML

void Load(const void* table) {
    const toml::table* keys = (*static_cast<const toml::table*>(table))["keys"].as_table();
    if (!keys) return;
    for (int a = 0; a < kActions; a++) {
        std::optional<std::string> name = (*keys)[kInfo[a].key].value<std::string>();
        if (!name) continue;
        if (*name == "none" || *name == "-" || name->empty()) {
            g_keys[a] = 0;
            continue;
        }
        int vk = 0;
        for (int k = 1; k < 0x100 && !vk; k++)
            if (Bindable(k) && SameName(Named(k), name->c_str())) vk = k;
        if (vk) g_keys[a] = vk;
        else LOGW("input: settings [keys] %s = '%s' is not a key name, keeping %s", kInfo[a].key, name->c_str(),
                 KeyName(g_keys[a]));
    }
}

void Save(void* table) {
    toml::table keys;
    for (int a = 0; a < kActions; a++) keys.insert(kInfo[a].key, g_keys[a] ? Named(g_keys[a]) : "none");
    static_cast<toml::table*>(table)->insert_or_assign("keys", std::move(keys));
}

#else  // no toml++ (the headless harness): the defaults only

void Load(const void*) {}
void Save(void*) {}

#endif

void Pad(int port, input::Pad& pad) {
    if (port != 0) return;
    // the keyboard is always present: without this the game sees no controller and ignores input
    pad.connected = true;

    // the settings menu is up: hand the game a neutral pad so it stands still while the player is in the UI
    if (ui::Open()) {
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
        std::memset(pad.analog, 0, sizeof pad.analog);
        return;
    }

    if (Down(kMoveUp) || Down(kMoveDown) || Down(kMoveLeft) || Down(kMoveRight)) {
        pad.lx = Axis(Down(kMoveLeft), Down(kMoveRight));
        pad.ly = Axis(Down(kMoveDown), Down(kMoveUp));
    }
    if (Down(kCamUp) || Down(kCamDown) || Down(kCamLeft) || Down(kCamRight)) {
        pad.rx = Axis(Down(kCamLeft), Down(kCamRight));
        pad.ry = Axis(Down(kCamDown), Down(kCamUp));
    }

    if (Down(kDpadUp)) pad.buttons |= kPadUp;
    if (Down(kDpadDown)) pad.buttons |= kPadDown;
    if (Down(kDpadLeft)) pad.buttons |= kPadLeft;
    if (Down(kDpadRight)) pad.buttons |= kPadRight;
    if (Down(kStart)) pad.buttons |= kPadStart;
    if (Down(kBack)) pad.buttons |= kPadBack;
    if (Down(kLS)) pad.buttons |= kPadLS;
    if (Down(kRS)) pad.buttons |= kPadRS;

    // analog face buttons: A B X Y black(RB) white(LB) LT RT
    if (Down(kA)) pad.analog[0] = 0xFF;
    if (Down(kB)) pad.analog[1] = 0xFF;
    if (Down(kX)) pad.analog[2] = 0xFF;
    if (Down(kY)) pad.analog[3] = 0xFF;
    if (Down(kRB)) pad.analog[4] = 0xFF;  // black / right bumper
    if (Down(kLB)) pad.analog[5] = 0xFF;  // white / left bumper
    if (Down(kLT)) pad.analog[6] = 0xFF;  // left trigger
    if (Down(kRT)) pad.analog[7] = 0xFF;  // right trigger

    mouse::Pad(pad);  // the mouse, while it looks, takes the right stick
    camscan::Tick(pad);  // XBE_CAMSCAN: the camera angle search watches the stick the game gets

    LOG_FIRST(1, "input: keyboard controls active (move %s%s%s%s, camera %s%s%s%s, A %s, B %s, X %s, Y %s)",
              KeyName(g_keys[kMoveUp]), KeyName(g_keys[kMoveLeft]), KeyName(g_keys[kMoveDown]),
              KeyName(g_keys[kMoveRight]), KeyName(g_keys[kCamUp]), KeyName(g_keys[kCamLeft]),
              KeyName(g_keys[kCamDown]), KeyName(g_keys[kCamRight]), KeyName(g_keys[kA]), KeyName(g_keys[kB]),
              KeyName(g_keys[kX]), KeyName(g_keys[kY]));
}

}  // namespace keyboard
