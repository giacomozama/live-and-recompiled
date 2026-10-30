#pragma once

#include <cstdint>

#include "core/input.h"

// ImGui settings menu and fps readout, wired through GameInfo::ui and GameInfo::message (see game_info.cpp);
// implemented in src/conker/game/ui.cpp. That file compiles to stubs on the headless harness, where ImGui and the
// real Win32 headers are absent, so nothing here may pull either in.
namespace ui {

// the GameInfo::ui hook: drawn inside the ImGui frame on the render thread, with app::UiLock held
void Draw(double fps);

// the GameInfo::message hook: window thread, each message after ImGui's; true consumes it, result is returned
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// whether the menu or the quit prompt is showing; safe from any thread
bool Open();

// the tab Show opens on: the one last shown, Controls (the game's Options pages) or Multiplayer (the Online Play
// menu's Options)
enum class Tab { Last, Controls, Multiplayer };

// shows the menu on that tab; any thread. The port is the controller that opened it, -1 for a keyboard: in a
// multiplayer match, which runs on, it alone drives the menu
void Show(Tab tab, int port = -1);

// the port of the controller that opened the menu, -1 none (Open() and no controller, or not open)
int Owner();

// shows the quit prompt on its own (the frontend's Start), the game held under it; any thread
void ShowQuit();

// Escape as the console's Start: down, and not the press that shut the menu or the prompt. Any thread
bool EscStart();

// the game's pad on that port, polled: while the menu holds it neutral it Muzzle()s the port, and after the menu
// shuts Unmuzzle() keeps each button held then (the B or A that shut it) off the pad until it is let go. Game thread
void Muzzle(int port);
void Unmuzzle(int port, input::Pad& pad);

// settings.toml options (app::Option): the fps overlay and the fps in the window title
extern bool g_fps_overlay;
extern bool g_fps_title;

}  // namespace ui
