// The game's own controller options, Invert Aim and Vibration, taken over by the settings menu: each player's are set
// before the game reads them, and its two Options pages, the pause menu's and the frontend's, open the settings menu
// instead.
#include <algorithm>
#include <atomic>
#include <cstdint>

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

namespace {
// each player's invert as 0x4D1F0 last left it (*edi), the profile's in a match
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
        g_game_invert[player].store(base[invert] != 0, std::memory_order_relaxed);
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
