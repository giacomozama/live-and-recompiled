// Keyboard controls, so Conker can be played without a gamepad.
//
// The runtime polls the pad on the guest's threads and hands the result to GameInfo::pad (see
// app.h), the hook a title gets to change a polled pad. This fills port 0 from the host keyboard
// and reports it connected, so the frontend and the game accept input with no controller plugged
// in. A real gamepad on port 0 keeps working: the keyboard only overrides the axes and buttons it
// is actually driving.
//
// The default layout, around WASD and the mouse (mouse look is on by default):
//
//   left stick   W A S D          camera (right stick)   arrow keys, and the mouse
//   d-pad        1 / 2 / 3 / 4 (up / right / down / left)
//   A / B        space / E or right mouse                X / Y   R / tab
//   LB / RB      T (white) / K (black)                   LT / RT Q or middle mouse / left ctrl or left mouse
//   start        enter            back                   backspace
//   stick clicks left shift (left) / left alt (right)
//
// Every action can be rebound, to up to two keys or mouse buttons: in the menu (Esc), or in settings.toml's [keys]
// table by key name (move_up = 'W', a = ['Space', 'Mouse 1'], 'none' unbinds).
//
// With mouse look on (src/game/mouse.cpp) the mouse also turns the camera, over the camera keys while it moves.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define KEYBOARD_TOML 1
#endif

#include "common/camscan.h"
#include "common/keyboard.h"
#include "common/mouse.h"
#include "app/app.h"
#include "common/stickwatch.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"

namespace keyboard {

int g_keys[kActions][kSlots];

namespace {

// Windows virtual-key codes, spelled out: they name keys on every platform (app::KeyDown), and <windows.h> is
// Windows only
enum : int {
    kUp = 0x26, kDown = 0x28, kLeft = 0x25, kRight = 0x27,
    kSpace = 0x20, kEnter = 0x0D, kBackspace = 0x08, kTab = 0x09, kEscape = 0x1B, kInsert = 0x2D,
    kLShift = 0xA0, kLCtrl = 0xA2, kLAlt = 0xA4,
    kMouse1 = 0x01, kMouse2 = 0x02, kMouse3 = 0x04,
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
    const char* label;  // the menu
    int vk, vk2 = 0;    // defaults
};
constexpr ActionInfo kInfo[kActions] = {
    {"move_up", "Move up", 'W'},             {"move_down", "Move down", 'S'},
    {"move_left", "Move left", 'A'},         {"move_right", "Move right", 'D'},
    {"camera_up", "Camera up", kUp},         {"camera_down", "Camera down", kDown},
    {"camera_left", "Camera left", kLeft},   {"camera_right", "Camera right", kRight},
    {"dpad_up", "D-pad up", '1'},            {"dpad_down", "D-pad down", '3'},
    {"dpad_left", "D-pad left", '4'},        {"dpad_right", "D-pad right", '2'},
    {"a", "A", kSpace},                      {"b", "B", 'E', kMouse2},
    {"x", "X", 'R'},                         {"y", "Y", kTab},
    {"lb", "LB (white)", 'T'},               {"rb", "RB (black)", 'K'},
    {"lt", "LT", 'Q', kMouse3},              {"rt", "RT", kLCtrl, kMouse1},
    {"start", "Start", kEnter},              {"back", "Back", kBackspace},
    {"ls", "Left stick click", kLShift},     {"rs", "Right stick click", kLAlt},
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

bool Down(int vk) { return vk && app::KeyDown(vk); }
bool Down(Action a) {
    for (int vk : g_keys[a])
        if (Down(vk)) return true;
    return false;
}

#ifdef KEYBOARD_TOML
// one name from settings.toml; false for one that names no key
bool Parse(const std::string& name, int& vk) {
    if (name == "none" || name == "-" || name.empty()) return vk = 0, true;
    for (int k = 1; k < 0x100; k++)
        if (Bindable(k) && SameName(Named(k), name.c_str())) return vk = k, true;
    return false;
}
#endif

// one axis from its two keys: keyboard strokes are digital, so a held key is full deflection
int16_t Axis(bool negative, bool positive) {
    return int16_t((positive ? kFull : 0) - (negative ? kFull : 0));
}

[[maybe_unused]] const bool g_defaults = (ResetKeys(), true);  // before Load, at static init

}  // namespace

const char* ActionLabel(int action) { return action >= 0 && action < kActions ? kInfo[action].label : ""; }

std::string ActionDefault(int action) {
    if (action < 0 || action >= kActions) return "";
    std::string s = KeyName(kInfo[action].vk);
    if (kInfo[action].vk2) s = s + ", " + KeyName(kInfo[action].vk2);
    return s;
}

const char* KeyName(int vk) { return vk ? Named(vk) : "-"; }

bool Bindable(int vk) { return vk > 0 && vk < 0x100 && vk != kEscape && vk != kInsert && Named(vk); }

void ResetKey(int action) {
    if (action < 0 || action >= kActions) return;
    g_keys[action][0] = kInfo[action].vk;
    g_keys[action][1] = kInfo[action].vk2;
}

void ResetKeys() {
    for (int a = 0; a < kActions; a++) ResetKey(a);
}

bool IsDefault(int action) {
    return action >= 0 && action < kActions && g_keys[action][0] == kInfo[action].vk &&
           g_keys[action][1] == kInfo[action].vk2;
}

#ifdef KEYBOARD_TOML

// an action is one name ('W') or a list of up to kSlots (['W', 'Up']); a bad name keeps the action as it was
void Load(const void* table) {
    const toml::table* keys = (*static_cast<const toml::table*>(table))["keys"].as_table();
    if (!keys) return;
    for (int a = 0; a < kActions; a++) {
        const toml::node* node = keys->get(kInfo[a].key);
        if (!node) continue;
        std::vector<std::string> names;
        if (const toml::array* list = node->as_array()) {
            for (const toml::node& n : *list)
                if (std::optional<std::string> name = n.value<std::string>()) names.push_back(*name);
        } else if (std::optional<std::string> name = node->value<std::string>()) {
            names.push_back(*name);
        }
        if (names.size() > size_t(kSlots))
            LOGW("input: settings [keys] %s has %zu keys, keeping the first %d", kInfo[a].key, names.size(), kSlots);
        int vks[kSlots] = {};
        bool ok = true;
        for (size_t s = 0; s < names.size() && s < size_t(kSlots); s++) {
            if (Parse(names[s], vks[s])) continue;
            LOGW("input: settings [keys] %s: '%s' is not a key name, keeping %s", kInfo[a].key, names[s].c_str(),
                 KeyName(g_keys[a][0]));
            ok = false;
        }
        if (ok) std::copy(vks, vks + kSlots, g_keys[a]);
    }
}

// one name for an action with a single key, else the list: 'W', ['W', 'Up'], 'none'
void Save(void* table) {
    toml::table keys;
    for (int a = 0; a < kActions; a++) {
        const char* names[kSlots];
        int n = 0;
        for (int vk : g_keys[a])
            if (vk) names[n++] = Named(vk);
        if (n == 0) {
            keys.insert(kInfo[a].key, "none");
        } else if (n == 1) {
            keys.insert(kInfo[a].key, names[0]);
        } else {
            toml::array list;
            for (int i = 0; i < n; i++) list.push_back(names[i]);
            keys.insert(kInfo[a].key, std::move(list));
        }
    }
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
    stickwatch::Tick();  // XBE_STICK_WATCH: F11 finds the code that reads the right stick

    // the first key of each: the rest are in the settings log
    auto key = [](Action a) { return KeyName(g_keys[a][0]); };
    LOG_FIRST(1, "input: keyboard controls active (move %s%s%s%s, camera %s%s%s%s, A %s, B %s, X %s, Y %s)",
              key(kMoveUp), key(kMoveLeft), key(kMoveDown), key(kMoveRight), key(kCamUp), key(kCamLeft),
              key(kCamDown), key(kCamRight), key(kA), key(kB), key(kX), key(kY));
}

}  // namespace keyboard
