// The game's own controller options, Invert Aim and Vibration, taken over by the settings menu: each player's are set
// before the game reads them, and its two Options pages, the pause menu's and the frontend's, open the settings menu
// instead.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include "common/gamepad.h"
#include "common/keyboard.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "common/ui.h"
#include "common/warp.h"

// 0x4D1F0: player edx's control scheme (*eax), invert aim (*edi, which the scheme 0x4D470/0x4D6E0 takes to negate
// the stick's y) and vibration (0x806FE0 + port*0x14, which 0x3D2910 needs set to start a rumble). Out of Xbox Live
// they come from the bytes the pause and frontend menus show, INVERT_AIM at 0x84B140 (zero inverts) and VIBRATION at
// 0x84B141 (non-zero rumbles), one pair for all players, kept in the save (the 0x118 bytes at 0x84B130, new game
// defaults by 0x352410). The function runs for each player in turn (0x4D320, the player whose actions struct
// 0x763260 + edx*0x1B0 it fills), so the player's own settings go into the bytes before each read; the game's menus
// and a save loaded don't change them.
// The scheme it writes is 0 single player's (0x4D470) or 1 to 4 a multiplayer profile's (the byte at profile+0x11B4,
// 0 to 3, through the table at 0x68C308): Emily, Victoria, Samantha and Elspeth, which 0x4D6E0 runs with two flags,
// jump on A rather than a stick's click (Victoria, Elspeth) and the sticks swapped (Samantha, Elspeth). The keys
// follow it (keyboard::SetScheme).
// In a match the function takes neither byte: invert from the player's profile (profile+0x11B5, which 0x4D6E0 takes to
// negate the camera stick's y, aiming or not) and vibration from profile+0x11B6 (through 0x4E250), as the profile's
// own menu in the game sets them; players 3 and 4 get no invert. So the settings menu's invert aim and vibration are
// single player's alone, and the mouse takes the profile's invert (MatchInvert) in place of its own. The strength
// (Rumble) applies in both: the game has no setting for it.
bool gamepad::g_invert_aim[input::kPorts] = {};
bool gamepad::g_vibration[input::kPorts] = {true, true, true, true};
uint32_t gamepad::g_vibration_percent[input::kPorts] = {100, 100, 100, 100};

uint32_t gamepad::g_move_stick_percent[input::kPorts] = {kStickDefault, kStickDefault, kStickDefault, kStickDefault};
uint32_t gamepad::g_look_stick_x_percent[input::kPorts] = {kStickDefault, kStickDefault, kStickDefault, kStickDefault};
uint32_t gamepad::g_look_stick_y_percent[input::kPorts] = {kStickDefault, kStickDefault, kStickDefault, kStickDefault};
uint32_t gamepad::g_move_stick_dead_zone[input::kPorts] = {kDeadZoneDefault, kDeadZoneDefault, kDeadZoneDefault,
                                                           kDeadZoneDefault};
uint32_t gamepad::g_look_stick_dead_zone[input::kPorts] = {kDeadZoneDefault, kDeadZoneDefault, kDeadZoneDefault,
                                                           kDeadZoneDefault};

namespace {
// each player's invert as 0x4D1F0 last left it (*edi, zero inverts, as at 0x84B140), the profile's in a match
std::atomic<bool> g_game_invert[input::kPorts];
}  // namespace

bool gamepad::MatchInvert(int port) {
    return port >= 0 && port < input::kPorts && g_game_invert[port].load(std::memory_order_relaxed);
}

// the motors' speeds the game sets (XInputSetState, the player's port) scaled; a change applies from the next rumble.
// In a match too: the profile turns rumble on or off, but its strength is ours alone
void gamepad::Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port < 0 || port >= input::kPorts) return;
    const uint32_t percent = std::min(g_vibration_percent[port], 100u);
    left = uint16_t(uint32_t(left) * percent / 100);
    right = uint16_t(uint32_t(right) * percent / 100);
}

namespace {
// Both sticks go through 0x4D110 (the filters at [0x806ABC] and [0x806AB4], set at 0x4C969): 0x4CF80 clamps each
// axis to full, reads a stick inside 0.3 of full as still, and past it reads (m - 0.3) / 0.7 along the stick's
// direction n (m its length), each axis clamped to full (so a square gate's corner reads full on both); 0x4D110 then
// cuts each axis to 64 steps. Stick works out what the game should read, g, from the player's own dead zone and
// percents, then hands the game the stick that reads as g: g (0.3 / |g| + 0.7), along g, its axes inside full while
// g's are. With the game's dead zone and 100 that is the stick as it was
struct Shape {
    float dead_zone;  // of full
    float power_x, power_y;  // 100 / percent
    bool radial;  // one power for the length, the direction kept (the stick that moves); else each axis its own
};

void Bend(int16_t& x, int16_t& y, const Shape& s) {
    constexpr float kGameDeadZone = 0.3f;
    const float fx = std::clamp(float(x) / 32767.0f, -1.0f, 1.0f), fy = std::clamp(float(y) / 32767.0f, -1.0f, 1.0f);
    const float m = std::hypot(fx, fy);
    if (m <= s.dead_zone) {
        x = y = 0;
        return;
    }
    const float t = (m - s.dead_zone) / (1.0f - s.dead_zone);
    float gx = std::clamp(fx / m * t, -1.0f, 1.0f), gy = std::clamp(fy / m * t, -1.0f, 1.0f);
    if (s.radial) {
        const float r = std::hypot(gx, gy);
        if (r < 1.0f) {
            const float scale = std::pow(r, s.power_x - 1.0f);
            gx *= scale, gy *= scale;
        }
    } else {
        gx = std::copysign(std::pow(std::fabs(gx), s.power_x), gx);
        gy = std::copysign(std::pow(std::fabs(gy), s.power_y), gy);
    }
    const float g = std::hypot(gx, gy);
    if (g <= 0.0f) {
        x = y = 0;
        return;
    }
    const float scale = kGameDeadZone / g + (1.0f - kGameDeadZone);
    x = int16_t(std::clamp(std::lround(gx * scale * 32767.0f), -32767L, 32767L));
    y = int16_t(std::clamp(std::lround(gy * scale * 32767.0f), -32767L, 32767L));
}

float Power(uint32_t percent) {
    return 100.0f / float(std::clamp(percent, gamepad::kStickMin, gamepad::kStickMax));
}
float DeadZone(uint32_t percent) { return float(std::min(percent, gamepad::kDeadZoneMax)) / 100.0f; }
}  // namespace

void gamepad::Sticks(int port, input::Pad& pad, bool southpaw) {
    if (port < 0 || port >= input::kPorts) return;
    const float move_power = Power(g_move_stick_percent[port]);
    const Shape move = {DeadZone(g_move_stick_dead_zone[port]), move_power, move_power, true};
    // the camera's stick keeps the game's curve (its speed is Turn's), its own dead zone
    const Shape look = {DeadZone(g_look_stick_dead_zone[port]), 1.0f, 1.0f, false};
    Bend(pad.lx, pad.ly, southpaw ? look : move);
    Bend(pad.rx, pad.ry, southpaw ? move : look);
}

// The stick's turn (see the 0x2F99A0 hook in mouse.cpp): the frame's yaw turn at +0x08 (the callers take it off their
// own yaw), the yaw at +0x04 (wrapped 0..360) and its eased value at +0x10; the pitch at +0x40, held inside the
// camera's settings [turn]+0x18..+0x1C, its eased value at +0x4C and the frame's pitch turn at +0x44. A caller takes
// the turns or the angles, so both grow by the same share. The game's whole turn scales, its fast turn and that ramp
// too, so a full tilt turns faster or slower as well. The tank's aim on foot (0x3F4E0, zoomed aim) is not this turn
void gamepad::Turn(uint32_t actions, uint32_t turn, float pitch_was) {
    constexpr uint32_t kPlayer1Actions = 0x763260, kActionsSize = 0x1B0;
    constexpr uint32_t kYaw = 0x04, kYawTurn = 0x08, kYawEased = 0x10;
    constexpr uint32_t kPitch = 0x40, kPitchTurn = 0x44, kPitchEased = 0x4C;
    constexpr uint32_t kPitchMin = 0x18, kPitchMax = 0x1C;
    if (actions < kPlayer1Actions || (actions - kPlayer1Actions) % kActionsSize) return;
    const uint32_t port = (actions - kPlayer1Actions) / kActionsSize;
    if (port >= uint32_t(input::kPorts)) return;
    const float kx = float(std::clamp(g_look_stick_x_percent[port], kStickMin, kLookXMax)) / 100.0f;
    const float ky = float(std::clamp(g_look_stick_y_percent[port], kStickMin, kLookYMax)) / 100.0f;
    if (kx == 1.0f && ky == 1.0f) return;
    auto readable = [](uint32_t va, uint32_t size) {
        return va && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
    };
    if (!readable(turn, kPitchEased + 4)) return;
    uint8_t* base = mem::Base();
    auto rf = [&](uint32_t va) { float f; std::memcpy(&f, base + va, 4); return f; };
    auto wf = [&](uint32_t va, float f) { std::memcpy(base + va, &f, 4); };
    if (kx != 1.0f) {
        const float extra = rf(turn + kYawTurn) * (kx - 1.0f);
        if (std::isfinite(extra) && extra != 0.0f) {
            wf(turn + kYawTurn, rf(turn + kYawTurn) + extra);
            float y = rf(turn + kYaw) - extra;
            while (y < 0.0f) y += 360.0f;
            while (y >= 360.0f) y -= 360.0f;
            wf(turn + kYaw, y);
            wf(turn + kYawEased, rf(turn + kYawEased) - extra);
        }
    }
    if (ky != 1.0f) {
        const float turned = rf(turn + kPitchTurn) * (ky - 1.0f);
        if (std::isfinite(turned)) wf(turn + kPitchTurn, rf(turn + kPitchTurn) + turned);
        const uint32_t settings = kernel::R32(turn);
        const float was = rf(turn + kPitch), extra = (was - pitch_was) * (ky - 1.0f);
        if (std::isfinite(extra) && extra != 0.0f && readable(settings, kPitchMax + 4)) {
            const float lo = rf(settings + kPitchMin), hi = rf(settings + kPitchMax);
            const float now = lo <= hi ? std::clamp(was + extra, std::min(lo, was), std::max(hi, was)) : was + extra;
            wf(turn + kPitch, now);
            wf(turn + kPitchEased, rf(turn + kPitchEased) + (now - was));
        }
    }
}

X86_EXTERN_FUNC(__imp__sub_0004D1F0);
X86_FUNC(sub_0004D1F0) {
    const uint32_t player = ctx.edx;
    if (player < uint32_t(input::kPorts)) {
        base[0x84B140] = gamepad::g_invert_aim[player] ? 0 : 1;
        base[0x84B141] = gamepad::g_vibration[player] ? 1 : 0;
    }
    const uint32_t scheme = ctx.eax, invert = ctx.edi;
    __imp__sub_0004D1F0(ctx, base);
    if (player < uint32_t(input::kPorts)) {
        keyboard::SetScheme(int(player), int(kernel::R32(scheme)));
        g_game_invert[player].store(base[invert] == 0, std::memory_order_relaxed);
    }
}

// 0x203B50: the pause menu's input, once a frame while paused (0x204060). Its page is [0x849AF8]: 0 the pause screen,
// 1 Options (the two options' Yes / No, layers 6 and 2 of the pause UI at [0x85E554]), 2 the quit confirmation, which
// goes back to the page in [0x849AFC]. Y (0x800 in the pausing player's buttons) turns between 0 and 1. The turn to
// Options is taken back before a frame draws it, and the settings menu opens on its Controls tab instead. While it
// runs the cursor is free (mouse::InMenu), and a click on a prompt presses its button (menus::Paused).
X86_EXTERN_FUNC(__imp__sub_00203B50);
X86_FUNC(sub_00203B50) {
    constexpr uint32_t kPage = 0x849AF8;
    const uint32_t was = kernel::R32(kPage);
    mouse::InMenu();
    menus::Paused();
    __imp__sub_00203B50(ctx, base);
    if (was == 0 && kernel::R32(kPage) == 1) {
        kernel::W32(kPage, 0);
        ui::Show(ui::Tab::Controls);
    }
}

// The frontend (level 29, the pub) shows the picture in focus, byte [0x77625E]: 0 Xbox Live, 1 Options (to Live's
// right), 2 Chapters, 3 to 5 the three games (0x205361 shows its UI page, the frontend's own at [0x85E550]). Options
// is a page of Yes / No settings, so A on its picture opens the settings menu instead and the game never sees it.
// Start there opens the settings menu instead and Back asks to quit (as Esc does) instead of the game's Xbox Live
// prompt, and Start with Back opens the settings menu anywhere (ui.cpp watches the raw pads for it), so the game never
// sees the two together. Back is the console's Xbox Live everywhere else, which there is none of: the game never sees it
// there, from a controller or the keys.
bool gamepad::Pub() {
    constexpr int kFrontend = 29;
    constexpr uint32_t kFrontendUi = 0x85E550;
    return warp::CurrentLevel() == kFrontend && kernel::R32(kFrontendUi);
}

void gamepad::Pad(int port, input::Pad& pad) {
    constexpr uint32_t kPicture = 0x77625E;
    constexpr int kOptions = 1;
    constexpr uint16_t kStart = 1 << 4, kBack = 1 << 5;
    static bool was_down[input::kPorts] = {}, start_was[input::kPorts] = {}, back_was[input::kPorts] = {};
    if (port < 0 || port >= input::kPorts) return;
    if ((pad.buttons & (kStart | kBack)) == (kStart | kBack)) pad.buttons &= uint16_t(~(kStart | kBack));
    const bool frontend = Pub();
    const bool start = pad.buttons & kStart;
    const bool start_pressed = start && !start_was[port];
    start_was[port] = start;
    if (frontend && start) {
        pad.buttons &= uint16_t(~kStart);
        if (start_pressed) ui::Show(ui::Tab::Last, port);
    }
    const bool back = pad.buttons & kBack;
    const bool back_pressed = back && !back_was[port];
    back_was[port] = back;
    pad.buttons &= uint16_t(~kBack);
    if (frontend && back_pressed) ui::ShowQuit();
    const bool down = pad.analog[0] > 0x40;  // A, past where the game takes it as pressed
    const bool pressed = down && !was_down[port];
    was_down[port] = down;
    if (!down || !frontend || int8_t(mem::Base()[kPicture]) != kOptions) return;
    pad.analog[0] = 0;
    if (pressed) ui::Show(ui::Tab::Controls, port);
}
