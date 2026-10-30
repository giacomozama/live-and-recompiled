#pragma once

#include <cstdint>

#include "common/keyboard.h"
#include "core/input.h"

// Mouse look: each player's mouse turns their camera as the right stick does (src/conker/game/mouse.cpp). The window
// thread reads raw mouse motion and holds the cursor in the window; keyboard::Pad hands the motion to the game as
// right stick.
namespace mouse {

// per set of controls (keyboard::Mode) and player, settings.toml [playerN] (or [playerN.multiplayer]) mouse_look,
// mouse_sensitivity (percent, horizontal), mouse_sensitivity_y (percent, vertical) and mouse_invert_y; read live, so
// the menu applies them, and the running level's set is the one used
extern bool g_look[keyboard::kModes][input::kPorts];
extern uint32_t g_sensitivity[keyboard::kModes][input::kPorts];
extern uint32_t g_sensitivity_y[keyboard::kModes][input::kPorts];
constexpr uint32_t kDefaultSensitivity = 100;  // both, and what the menu resets them to
extern bool g_invert_y[keyboard::kModes][input::kPorts];

// posted to the game window when the setting changes off the window thread (WM_APP + 0x41); Message refreshes
constexpr uint32_t kRefresh = 0x8000 + 0x41;

// from ui::Message, window thread: raw motion and the cursor capture; never consumes a message
void Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);

// takes or lets go of the cursor as the setting, the F1 menu and the window's focus ask; window thread
void Refresh(void* hwnd);

// one of the game's menus ran this frame (single player's pause menu, a menu list), so the cursor goes free while
// they run; game thread
void InMenu();

// from keyboard::Pad: the last frame's motion of the port's mice as right stick, while the cursor is held
void Pad(int port, input::Pad& pad);

}  // namespace mouse
