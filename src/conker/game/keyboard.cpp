// Keyboard controls, so Conker can be played without a gamepad.
//
// The runtime polls the pad on the guest's threads and hands the result to GameInfo::pad (see
// app.h), the hook a title gets to change a polled pad. This fills each port from the keyboards and
// mouse buttons its player has (input.h) and reports it connected, so the frontend and the game
// accept input with no controller plugged in; port 0 always is. A player's controller keeps
// working: the keys only override the axes and buttons they are actually driving.
//
// Keys are bound to actions, and an action presses the button the player's controls have it on (Drives): single
// player's (and every menu's out of a match), or in a match the controller scheme of the player's profile, which
// 0x4D1F0 picks (gamepad.cpp hooks it, SetScheme). The schemes, as the profile's controller page shows them:
//
//                   single player     Emily       Victoria    Samantha     Elspeth
//   move            left stick        left        left        right        right
//   look            right stick       right       right       left         left
//   jump            A                 left click  A           right click  A
//   reload          X                 A           left click  A            right click
//   aim             B (take out)      right click right click left click   left click   (a match's Zoom)
//   camera zoom     right click       -           -           -            -
//   crouch          RT (duck)         B (posture) B           B            B
//   use             B                 -           -           -            -
//   attack / fire RT, first person / weapon mode LT, ability X, cycle equipped Y, vote black, radio white: in all
//   (out of a match ability, cycle, vote and radio keep Emily's buttons, for the menus)
//
// The default keys, around WASD and the mouse (mouse look is on by default):
//
//   move W A S D   look arrows and the mouse   jump space   crouch left ctrl, C   attack mouse 1   aim mouse 2
//   use E   weapon mode Z   reload R   camera zoom mouse 3   ability Q   cycle equipped tab   vote V   radio T
//   pause enter (and Escape)   quit backspace
//
// Every player's actions can be rebound, to up to two keys or mouse buttons each: in the menu (Esc), or in
// settings.toml's [playerN.keys] tables by key name (move_up = 'W', jump = ['Space', 'Mouse 4'], 'none' unbinds).
//
// With mouse look on (src/conker/game/mouse.cpp) the mouse also turns the camera, over the look keys while it moves.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#define KEYBOARD_TOML 1
#endif

#include "common/camscan.h"
#include "common/gamepad.h"
#include "common/keyboard.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "app/app.h"
#include "common/stickwatch.h"
#include "common/ui.h"
#include "common/warp.h"
#include "core/input.h"
#include "core/log.h"

namespace keyboard {

int g_keys[input::kPorts][kActions][kSlots];

namespace {

// Windows virtual-key codes, spelled out: they name keys on every platform (app::KeyDown), and <windows.h> is
// Windows only
enum : int {
    kUp = 0x26, kDown = 0x28, kLeft = 0x25, kRight = 0x27,
    kSpace = 0x20, kEnter = 0x0D, kBackspace = 0x08, kTab = 0x09, kEscape = 0x1B, kF1 = 0x70,
    kLShift = 0xA0, kLCtrl = 0xA2, kLAlt = 0xA4,
    kMouse1 = 0x01, kMouse2 = 0x02, kMouse3 = 0x04,
};

// the console controller's button bit (see input::Pad)
enum : uint16_t {
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
    {"move_up", "Move forward", 'W'},       {"move_down", "Move back", 'S'},
    {"move_left", "Move left", 'A'},        {"move_right", "Move right", 'D'},
    {"camera_up", "Look up", kUp},          {"camera_down", "Look down", kDown},
    {"camera_left", "Look left", kLeft},    {"camera_right", "Look right", kRight},
    {"jump", "Jump", kSpace},               {"crouch", "Crouch", kLCtrl, 'C'},
    {"attack", "Attack / fire", kMouse1},
    {"use", "Use", 'E'},                    {"aim", "Aim / take out weapon", kMouse2},
    {"weapon_mode", "First person / weapon mode", 'Z'},
    {"reload", "Reload", 'R'},              {"zoom", "Camera zoom", kMouse3},
    {"ability", "Abilities", 'Q'},          {"cycle", "Cycle equipped", kTab},
    {"vote", "Vote", 'V'},                  {"radio", "Radio mode", 'T'},
    {"pause", "Pause", kEnter},             {"quit", "Quit", kBackspace},
};

// the scheme each port's player has, as the game last picked it (SetScheme); Emily's until a match picks one
std::atomic<int> g_scheme[input::kPorts] = {1, 1, 1, 1};

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

bool Down(int port, Action a) {
    for (int vk : g_keys[port][a])
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
bool MoveStick(int port, int16_t& x, int16_t& y) {
    Thumb& t = g_thumb[port];
    const int64_t now = NowUs();
    const double dt = std::clamp(double(now - t.last) / 1e6, 0.0, 0.1);
    t.last = now;

    const int16_t kx = Axis(Down(port, kMoveLeft), Down(port, kMoveRight));
    const int16_t ky = Axis(Down(port, kMoveDown), Down(port, kMoveUp));
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
    for (int p = 0; p < input::kPorts; p++) ResetKeys(p);
    return true;
}();

}  // namespace

int Scheme(int port) {
    if (port < 0 || port >= input::kPorts || !warp::Multiplayer()) return 0;
    return g_scheme[port].load(std::memory_order_relaxed);
}

const char* SchemeName(int scheme) {
    constexpr const char* kNames[kSchemes] = {"Single player", "Emily", "Victoria", "Samantha", "Elspeth"};
    return scheme >= 0 && scheme < kSchemes ? kNames[scheme] : "";
}

void SetScheme(int port, int scheme) {
    if (port < 0 || port >= input::kPorts || scheme < 0 || scheme >= kSchemes) return;
    if (g_scheme[port].exchange(scheme) != scheme)
        LOGR("input: player %d's keys follow the %s controls", port + 1, SchemeName(scheme));
}

bool Southpaw(int scheme) { return scheme == 3 || scheme == 4; }

int Drives(int scheme, int action) {
    const bool story = scheme <= 0 || scheme >= kSchemes;
    // Victoria and Elspeth have jump on A and reload on a stick's click, Emily and Samantha the other way round; the
    // southpaw schemes swap the sticks, their clicks with them
    const bool jump_on_a = scheme == 2 || scheme == 4;
    const int click = Southpaw(scheme) ? kButtonRS : kButtonLS;  // the click that jumps or reloads
    switch (action) {
        case kJump: return story || jump_on_a ? kButtonA : click;
        case kReload: return story ? kButtonX : jump_on_a ? click : kButtonA;
        case kAim: return story ? kButtonB : Southpaw(scheme) ? kButtonLS : kButtonRS;  // a match's Zoom
        case kZoom: return story ? kButtonRS : -1;
        case kCrouch: return story ? kButtonRT : kButtonB;  // single player ducks on RT, a match's posture is B
        case kUse: return story ? kButtonB : -1;
        case kAttack: return kButtonRT;
        case kWeaponMode: return kButtonLT;
        case kAbility: return kButtonX;
        case kCycle: return kButtonY;
        case kVote: return kButtonBlack;
        case kRadio: return kButtonWhite;
        case kPause: return kButtonStart;
        case kQuit: return kButtonBack;
        default: return -1;  // the sticks
    }
}

const char* ActionLabel(int action) { return action >= 0 && action < kActions ? kInfo[action].label : ""; }

std::string ActionDefault(int action) {
    if (action < 0 || action >= kActions) return "";
    std::string s = KeyName(kInfo[action].vk);
    if (kInfo[action].vk2) s = s + ", " + KeyName(kInfo[action].vk2);
    return s;
}

const char* KeyName(int vk) { return vk ? Named(vk) : "-"; }

bool Bindable(int vk) { return vk > 0 && vk < 0x100 && vk != kEscape && vk != kF1 && Named(vk); }

void ResetKey(int player, int action) {
    if (player < 0 || player >= input::kPorts || action < 0 || action >= kActions) return;
    g_keys[player][action][0] = kInfo[action].vk;
    g_keys[player][action][1] = kInfo[action].vk2;
}

void ResetKeys(int player) {
    for (int a = 0; a < kActions; a++) ResetKey(player, a);
}

bool IsDefault(int player, int action) {
    if (player < 0 || player >= input::kPorts || action < 0 || action >= kActions) return false;
    const int(&keys)[kSlots] = g_keys[player][action];
    return keys[0] == kInfo[action].vk && keys[1] == kInfo[action].vk2;
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
void LoadMouse(const toml::table& t, int p) {
    mouse::g_look[p] = t["mouse_look"].value_or(mouse::g_look[p]);
    mouse::g_sensitivity[p] = t["mouse_sensitivity"].value_or(mouse::g_sensitivity[p]);
    mouse::g_sensitivity_y[p] = t["mouse_sensitivity_y"].value_or(mouse::g_sensitivity_y[p]);
    mouse::g_invert_y[p] = t["mouse_invert_y"].value_or(mouse::g_invert_y[p]);
}

void SaveMouse(toml::table& t, int p) {
    t.insert_or_assign("mouse_look", mouse::g_look[p]);
    t.insert_or_assign("mouse_sensitivity", int64_t(mouse::g_sensitivity[p]));
    t.insert_or_assign("mouse_sensitivity_y", int64_t(mouse::g_sensitivity_y[p]));
    t.insert_or_assign("mouse_invert_y", mouse::g_invert_y[p]);
}


std::string PlayerTable(int p) { return "player" + std::to_string(p + 1); }

}  // namespace

// the keys of actions this file does not know (the controller buttons' of before, a = 'Space') are left out, and those
// actions keep their defaults; the multiplayer set of before ([playerN.multiplayer]) is no longer read, and goes as
// the file is saved
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    for (int p = 0; p < input::kPorts; p++) {
        const toml::table* player = t[PlayerTable(p)].as_table();
        const bool keys = player && (*player)["keys"].as_table();
        const bool look = player && (*player)["mouse_look"];
        if (p == 0 && !keys) LoadKeys(t["keys"].as_table(), g_keys[0]);
        if (p == 0 && !look) LoadMouse(t, 0);
        if (!player) continue;
        LoadKeys((*player)["keys"].as_table(), g_keys[p]);
        LoadMouse(*player, p);
        gamepad::g_invert_aim[p] = (*player)["invert_aim"].value_or(gamepad::g_invert_aim[p]);
        gamepad::g_vibration[p] = (*player)["vibration"].value_or(gamepad::g_vibration[p]);
        gamepad::g_vibration_percent[p] =
            std::min((*player)["vibration_percent"].value_or(gamepad::g_vibration_percent[p]), 100u);
    }
}

// into the [playerN] tables the runtime writes the devices in
void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    for (int p = 0; p < input::kPorts; p++) {
        toml::table* player = t[PlayerTable(p)].as_table();
        if (!player) player = t.insert_or_assign(PlayerTable(p), toml::table{}).first->second.as_table();
        SaveMouse(*player, p);
        player->insert_or_assign("invert_aim", gamepad::g_invert_aim[p]);
        player->insert_or_assign("vibration", gamepad::g_vibration[p]);
        player->insert_or_assign("vibration_percent", int64_t(gamepad::g_vibration_percent[p]));
        player->insert_or_assign("keys", SaveKeys(g_keys[p]));
        player->erase("multiplayer");
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
                       input::Has(port, input::Kind::Mouse) || port == ui::Owner())) {
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
        std::memset(pad.analog, 0, sizeof pad.analog);
        ui::Muzzle(port);
        return;
    }

    // the on-screen keyboard is up: the keys type into it and the mouse works its keys (menus::Typing), so neither
    // drives the pad; what is still held as it goes (the Enter that made the name) stays off the pad until let go
    if (menus::Typing()) {
        ui::Muzzle(port);
        return;
    }

    // each action's keys press the button the player's controls have it on, the sticks' keys the stick that does
    // their part
    const int scheme = Scheme(port);
    const bool southpaw = Southpaw(scheme);
    auto down = [&](Action a) { return Down(port, a); };
    bool held[kButtons] = {};
    for (int a = 0; a < kActions; a++)
        if (const int b = Drives(scheme, a); b >= 0 && down(Action(a))) held[b] = true;
    if (int16_t x, y; MoveStick(port, x, y)) {
        (southpaw ? pad.rx : pad.lx) = x;
        (southpaw ? pad.ry : pad.ly) = y;
    }
    if (down(kLookUp) || down(kLookDown) || down(kLookLeft) || down(kLookRight)) {
        (southpaw ? pad.lx : pad.rx) = Axis(down(kLookLeft), down(kLookRight));
        (southpaw ? pad.ly : pad.ry) = Axis(down(kLookDown), down(kLookUp));
    }

    // single player's pause screen and its quit prompt: their face buttons are fixed keys (menus::PauseKeys), the
    // bindings left out, so the prompts can show them. A key held as the page came up (the Escape that paused, the Q
    // that asked to quit, the Enter that answered No) counts once let go, and so does Pause's on those pages
    static bool armed[0x100] = {};
    static int armed_page = -1;
    const int page = port == 0 && !input::t_presence ? menus::KeyPage() : -1;
    const bool pause_keys = page >= 0;
    if (port == 0 && !input::t_presence && page != armed_page) {
        std::fill(std::begin(armed), std::end(armed), false);
        armed_page = page;
    }
    auto fresh = [&](int vk) {  // held, and let go since the page came up
        if (!input::KeyDown(port, vk)) return armed[vk] = true, false;
        return armed[vk];
    };
    if (pause_keys) {
        for (int face = 0; face < 4; face++)
            for (int vk : menus::PauseKeys(page, face)) {
                if (!vk || !fresh(vk)) continue;
                pad.analog[face] = 0xFF;
                if (vk == kEscape) ui::SwallowEsc();  // not Start (pausing again) until let go
            }
        if (page == 2)
            for (int vk : menus::QuitBackKeys()) {
                if (!fresh(vk)) continue;
                menus::AnswerQuit(false);
                pad.analog[0] = 0xFF;
                if (vk == kEscape) ui::SwallowEsc();
            }
    }
    bool start = false;
    if (!pause_keys) {
        start = held[kButtonStart];
    } else if (!menus::PauseKeysTakeStart(page)) {
        for (int vk : g_keys[port][kPause]) start |= vk && fresh(vk);
    }

    // Escape is the console's Start, for the keyboards (the menu's own Esc is not: ui::EscStart); on the pause
    // screen it is B instead
    if (start || (!pause_keys && (port == 0 || input::Has(port, input::Kind::Keyboard)) && ui::EscStart()))
        pad.buttons |= kPadStart;
    // Back is the console's Xbox Live, which there is none of: the quit key presses it on the pub's pictures alone,
    // where it asks to quit instead (gamepad::Pad)
    if (held[kButtonBack] && gamepad::Pub()) pad.buttons |= kPadBack;
    if (held[kButtonLS]) pad.buttons |= kPadLS;
    if (held[kButtonRS]) pad.buttons |= kPadRS;

    // analog buttons: A B X Y black white LT RT (Button's order); on those screens the fixed keys' faces instead
    for (int b = kButtonA; b <= kButtonRT; b++)
        if (held[b] && !(b <= kButtonY && menus::FaceFixed(page, b))) pad.analog[b] = 0xFF;

    ui::Unmuzzle(port, pad);  // what shut the menu stays unpressed until it is let go
    menus::Pad(port, pad);  // the mouse points and clicks on the game's menu lists and the pub's pictures
    gamepad::Pad(port, pad);  // A on the frontend's Options opens the settings menu, the mouse's too
    mouse::Pad(port, pad);  // the mouse, while it looks, takes the right stick
    if (port != 0) return;
    camscan::Tick(pad);  // XBE_CAMSCAN: the camera angle search watches the stick the game gets
    stickwatch::Tick();  // XBE_STICK_WATCH: F11 finds the code that reads the right stick

    // the first key of each: the rest are in the settings log
    auto key = [&](Action a) { return KeyName(g_keys[0][a][0]); };
    LOG_FIRST(1, "input: keyboard controls active (move %s%s%s%s, look %s%s%s%s, jump %s, use %s, attack %s, aim %s)",
              key(kMoveUp), key(kMoveLeft), key(kMoveDown), key(kMoveRight), key(kLookUp), key(kLookLeft),
              key(kLookDown), key(kLookRight), key(kJump), key(kUse), key(kAttack), key(kAim));
}

}  // namespace keyboard
