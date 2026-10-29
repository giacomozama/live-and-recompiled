// Keyboard controls, so Conker can be played without a gamepad.
//
// The runtime polls the pad on the guest's threads and hands the result to GameInfo::pad (see
// app.h), the hook a title gets to change a polled pad. This fills each port from the keyboards and
// mouse buttons its player has (input.h) and reports it connected, so the frontend and the game
// accept input with no controller plugged in; port 0 always is. A player's controller keeps
// working: the keys only override the axes and buttons they are actually driving.
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
// Every player's actions can be rebound, to up to two keys or mouse buttons each: in the menu (Esc), or in
// settings.toml's [playerN.keys] tables by key name (move_up = 'W', a = ['Space', 'Mouse 1'], 'none' unbinds).
// Multiplayer has a set of its own, keys and mouse settings both, in [playerN.multiplayer]: it starts as a copy of
// single player's, and is used in multiplayer matches only (its menus keep single player's).
//
// With mouse look on (src/conker/game/mouse.cpp) the mouse also turns the camera, over the camera keys while it moves.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include "common/warp.h"
#include "core/input.h"
#include "core/log.h"

namespace keyboard {

int g_keys[kModes][input::kPorts][kActions][kSlots];

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

bool Down(int mode, int port, Action a) {
    for (int vk : g_keys[mode][port][a])
        if (vk && input::KeyDown(port, vk)) return true;
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

// The movement keys as a thumb on the stick's rim. Eight directions jump 45 degrees between two polls, which reads as
// a far faster spin than a thumb makes, and the alien's tail swing (the last boss) takes no momentum from it: Conker
// stands still. So a turn of up to 90 degrees sweeps there at kSweep instead, and a key let go mid-spin, before the
// next one is down, holds the stick for kGap rather than dropping it to the centre. Larger turns and starts from
// the centre snap as before, and a resting stick sits where the keys put it (on the square: diagonals full on both
// axes), so walking is as it was.
constexpr double kSweep = 540.0;           // degrees per second, a brisk spin of the stick
constexpr double kSnap = 90.0 + 1e-3;      // turns wider than this jump
constexpr int64_t kGap = 60'000;           // microseconds a key change can leave the keys up mid-spin
constexpr int64_t kSpinning = 300'000;     // a sweep this recent counts as spinning

struct Thumb {
    bool held = false;
    double angle = 0;          // degrees, counterclockwise from right
    int64_t last = 0;          // the previous poll
    int64_t swept = INT64_MIN / 2;  // the last poll that turned the stick
    int64_t released = 0;      // when the keys went up
};
Thumb g_thumb[input::kPorts];

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// a direction on the square a pair of digital axes reaches, so the eight key directions come out as they are
void OnSquare(double degrees, int16_t& x, int16_t& y) {
    const double r = degrees * (3.14159265358979323846 / 180.0);
    const double c = std::cos(r), s = std::sin(r);
    const double m = std::max(std::fabs(c), std::fabs(s));
    x = int16_t(std::lround(c / m * kFull));
    y = int16_t(std::lround(s / m * kFull));
}

// the left stick from the movement keys; false while neither they nor a held spin drive it
bool MoveStick(int mode, int port, int16_t& x, int16_t& y) {
    Thumb& t = g_thumb[port];
    const int64_t now = NowUs();
    const double dt = std::clamp(double(now - t.last) / 1e6, 0.0, 0.1);
    t.last = now;

    const int16_t kx = Axis(Down(mode, port, kMoveLeft), Down(mode, port, kMoveRight));
    const int16_t ky = Axis(Down(mode, port, kMoveDown), Down(mode, port, kMoveUp));
    if (kx == 0 && ky == 0) {  // no key, or two opposite ones
        if (!t.held) return false;
        if (t.released == 0) t.released = now;
        if (now - t.released < kGap && now - t.swept < kSpinning) {
            OnSquare(t.angle, x, y);
            return true;
        }
        t.held = false;
        return false;
    }
    t.released = 0;

    const double target = std::atan2(double(ky), double(kx)) * (180.0 / 3.14159265358979323846);
    double turn = std::remainder(target - t.angle, 360.0);
    if (!t.held || std::fabs(turn) > kSnap) {
        t.angle = target;
        turn = 0;
    } else if (turn != 0) {
        const double step = kSweep * dt;
        t.angle = std::fabs(turn) <= step ? target : t.angle + std::copysign(step, turn);
        t.swept = now;
    }
    t.held = true;
    if (t.angle == target) {  // at rest: exactly what the keys say
        x = kx;
        y = ky;
    } else {
        OnSquare(t.angle, x, y);
    }
    return true;
}

[[maybe_unused]] const bool g_defaults = [] {  // before Load, at static init
    for (int m = 0; m < kModes; m++)
        for (int p = 0; p < input::kPorts; p++) ResetKeys(m, p);
    return true;
}();

}  // namespace

int ActiveMode() { return warp::Multiplayer() ? kMultiplayer : kSinglePlayer; }

const char* ActionLabel(int action) { return action >= 0 && action < kActions ? kInfo[action].label : ""; }

std::string ActionDefault(int action) {
    if (action < 0 || action >= kActions) return "";
    std::string s = KeyName(kInfo[action].vk);
    if (kInfo[action].vk2) s = s + ", " + KeyName(kInfo[action].vk2);
    return s;
}

const char* KeyName(int vk) { return vk ? Named(vk) : "-"; }

bool Bindable(int vk) { return vk > 0 && vk < 0x100 && vk != kEscape && vk != kInsert && Named(vk); }

void ResetKey(int mode, int player, int action) {
    if (mode < 0 || mode >= kModes || player < 0 || player >= input::kPorts || action < 0 || action >= kActions)
        return;
    g_keys[mode][player][action][0] = kInfo[action].vk;
    g_keys[mode][player][action][1] = kInfo[action].vk2;
}

void ResetKeys(int mode, int player) {
    for (int a = 0; a < kActions; a++) ResetKey(mode, player, a);
}

bool IsDefault(int mode, int player, int action) {
    if (mode < 0 || mode >= kModes || player < 0 || player >= input::kPorts || action < 0 || action >= kActions)
        return false;
    const int(&keys)[kSlots] = g_keys[mode][player][action];
    return keys[0] == kInfo[action].vk && keys[1] == kInfo[action].vk2;
}

void CopyToMultiplayer(int player) {
    if (player < 0 || player >= input::kPorts) return;
    std::memcpy(g_keys[kMultiplayer][player], g_keys[kSinglePlayer][player], sizeof g_keys[kSinglePlayer][player]);
    mouse::g_look[kMultiplayer][player] = mouse::g_look[kSinglePlayer][player];
    mouse::g_sensitivity[kMultiplayer][player] = mouse::g_sensitivity[kSinglePlayer][player];
    mouse::g_sensitivity_y[kMultiplayer][player] = mouse::g_sensitivity_y[kSinglePlayer][player];
    mouse::g_invert_y[kMultiplayer][player] = mouse::g_invert_y[kSinglePlayer][player];
}

#ifdef KEYBOARD_TOML

namespace {

// an action is one name ('W') or a list of up to kSlots (['W', 'Up']); a bad name keeps the action as it was
void LoadKeys(const toml::table* keys, int (&player)[kActions][kSlots]) {
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
                 KeyName(player[a][0]));
            ok = false;
        }
        if (ok) std::copy(vks, vks + kSlots, player[a]);
    }
}

// one name for an action with a single key, else the list: 'W', ['W', 'Up'], 'none'
toml::table SaveKeys(const int (&player)[kActions][kSlots]) {
    toml::table keys;
    for (int a = 0; a < kActions; a++) {
        const char* names[kSlots];
        int n = 0;
        for (int vk : player[a])
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
    return keys;
}

// a player's mouse settings, from their table (or the file's top level, where player 1's were before)
void LoadMouse(const toml::table& t, int m, int p) {
    mouse::g_look[m][p] = t["mouse_look"].value_or(mouse::g_look[m][p]);
    mouse::g_sensitivity[m][p] = t["mouse_sensitivity"].value_or(mouse::g_sensitivity[m][p]);
    mouse::g_sensitivity_y[m][p] = t["mouse_sensitivity_y"].value_or(mouse::g_sensitivity_y[m][p]);
    mouse::g_invert_y[m][p] = t["mouse_invert_y"].value_or(mouse::g_invert_y[m][p]);
}

void SaveMouse(toml::table& t, int m, int p) {
    t.insert_or_assign("mouse_look", mouse::g_look[m][p]);
    t.insert_or_assign("mouse_sensitivity", int64_t(mouse::g_sensitivity[m][p]));
    t.insert_or_assign("mouse_sensitivity_y", int64_t(mouse::g_sensitivity_y[m][p]));
    t.insert_or_assign("mouse_invert_y", mouse::g_invert_y[m][p]);
}


std::string PlayerTable(int p) { return "player" + std::to_string(p + 1); }

}  // namespace

void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    for (int p = 0; p < input::kPorts; p++) {
        const toml::table* player = t[PlayerTable(p)].as_table();
        const bool keys = player && (*player)["keys"].as_table();
        const bool look = player && (*player)["mouse_look"];
        if (p == 0 && !keys) LoadKeys(t["keys"].as_table(), g_keys[kSinglePlayer][0]);
        if (p == 0 && !look) LoadMouse(t, kSinglePlayer, 0);
        if (player) {
            LoadKeys((*player)["keys"].as_table(), g_keys[kSinglePlayer][p]);
            LoadMouse(*player, kSinglePlayer, p);
        }
        // a player's multiplayer set starts as their single player one: what its table leaves out stays so
        CopyToMultiplayer(p);
        const toml::table* multi = player ? (*player)["multiplayer"].as_table() : nullptr;
        if (!multi) continue;
        LoadKeys((*multi)["keys"].as_table(), g_keys[kMultiplayer][p]);
        LoadMouse(*multi, kMultiplayer, p);
    }
}

// into the [playerN] tables the runtime writes the devices in
void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    for (int p = 0; p < input::kPorts; p++) {
        toml::table* player = t[PlayerTable(p)].as_table();
        if (!player) player = t.insert_or_assign(PlayerTable(p), toml::table{}).first->second.as_table();
        SaveMouse(*player, kSinglePlayer, p);
        player->insert_or_assign("keys", SaveKeys(g_keys[kSinglePlayer][p]));
        toml::table multi;
        SaveMouse(multi, kMultiplayer, p);
        multi.insert_or_assign("keys", SaveKeys(g_keys[kMultiplayer][p]));
        player->insert_or_assign("multiplayer", std::move(multi));
    }
}

#else  // no toml++ (the headless harness): the defaults only

void Load(const void*) {}
void Save(void*) {}

#endif

void Pad(int port, input::Pad& pad) {
    if (port < 0 || port >= input::kPorts) return;
    // player 1's keyboard is always present: without this the game sees no controller and ignores input
    if (port == 0 || input::Has(port, input::Kind::Keyboard) || input::Has(port, input::Kind::Mouse))
        pad.connected = true;

    // the settings menu is up: hand the game neutral pads so every player stands still while one is in the UI. In
    // a multiplayer match the game runs on under the menu (ui.cpp), and only the players with a keyboard or mouse, which
    // work the menu, stand still
    if (ui::Open() && (!warp::Multiplayer() || input::Has(port, input::Kind::Keyboard) ||
                       input::Has(port, input::Kind::Mouse))) {
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
        std::memset(pad.analog, 0, sizeof pad.analog);
        return;
    }

    const int mode = ActiveMode();
    auto down = [&](Action a) { return Down(mode, port, a); };
    if (int16_t x, y; MoveStick(mode, port, x, y)) {
        pad.lx = x;
        pad.ly = y;
    }
    if (down(kCamUp) || down(kCamDown) || down(kCamLeft) || down(kCamRight)) {
        pad.rx = Axis(down(kCamLeft), down(kCamRight));
        pad.ry = Axis(down(kCamDown), down(kCamUp));
    }

    if (down(kDpadUp)) pad.buttons |= kPadUp;
    if (down(kDpadDown)) pad.buttons |= kPadDown;
    if (down(kDpadLeft)) pad.buttons |= kPadLeft;
    if (down(kDpadRight)) pad.buttons |= kPadRight;
    if (down(kStart)) pad.buttons |= kPadStart;
    if (down(kBack)) pad.buttons |= kPadBack;
    if (down(kLS)) pad.buttons |= kPadLS;
    if (down(kRS)) pad.buttons |= kPadRS;

    // analog face buttons: A B X Y black(RB) white(LB) LT RT
    if (down(kA)) pad.analog[0] = 0xFF;
    if (down(kB)) pad.analog[1] = 0xFF;
    if (down(kX)) pad.analog[2] = 0xFF;
    if (down(kY)) pad.analog[3] = 0xFF;
    if (down(kRB)) pad.analog[4] = 0xFF;  // black / right bumper
    if (down(kLB)) pad.analog[5] = 0xFF;  // white / left bumper
    if (down(kLT)) pad.analog[6] = 0xFF;  // left trigger
    if (down(kRT)) pad.analog[7] = 0xFF;  // right trigger

    mouse::Pad(port, pad);  // the mouse, while it looks, takes the right stick
    if (port != 0) return;
    camscan::Tick(pad);  // XBE_CAMSCAN: the camera angle search watches the stick the game gets
    stickwatch::Tick();  // XBE_STICK_WATCH: F11 finds the code that reads the right stick

    // the first key of each: the rest are in the settings log
    auto key = [&](Action a) { return KeyName(g_keys[mode][0][a][0]); };
    LOG_FIRST(1, "input: keyboard controls active (move %s%s%s%s, camera %s%s%s%s, A %s, B %s, X %s, Y %s)",
              key(kMoveUp), key(kMoveLeft), key(kMoveDown), key(kMoveRight), key(kCamUp), key(kCamLeft),
              key(kCamDown), key(kCamRight), key(kA), key(kB), key(kX), key(kY));
}

}  // namespace keyboard
