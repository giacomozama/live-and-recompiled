#pragma once

#include <cstdint>

namespace input {
struct Pad;
}

// Mouse look: the mouse turns the camera as the right stick does (src/game/mouse.cpp). The window thread reads raw
// mouse motion and holds the cursor in the window; keyboard::Pad hands the motion to the game as right stick.
namespace mouse {

// settings.toml mouse_look, mouse_sensitivity (percent, horizontal), mouse_sensitivity_y (percent, vertical) and
// mouse_invert_y; read live, so the F1 menu applies them
extern bool g_look;
extern uint32_t g_sensitivity;
extern uint32_t g_sensitivity_y;
constexpr uint32_t kDefaultSensitivity = 100;  // both, and what the F1 menu resets them to
extern bool g_invert_y;

// posted to the game window when the setting changes off the window thread (WM_APP + 0x41); Message refreshes
constexpr uint32_t kRefresh = 0x8000 + 0x41;

// from ui::Message, window thread: raw motion and the cursor capture; never consumes a message
void Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);

// takes or lets go of the cursor as the setting, the F1 menu and the window's focus ask; window thread
void Refresh(void* hwnd);

// from keyboard::Pad, port 0: the last frame's mouse motion as right stick, while the cursor is held
void Pad(input::Pad& pad);

}  // namespace mouse
