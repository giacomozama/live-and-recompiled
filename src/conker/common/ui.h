#pragma once

#include <cstdint>

// ImGui settings menu and fps readout, wired through GameInfo::ui and GameInfo::message (see game_info.cpp);
// implemented in src/conker/game/ui.cpp. That file compiles to stubs on the headless harness, where ImGui and the
// real Win32 headers are absent, so nothing here may pull either in.
namespace ui {

// the GameInfo::ui hook: drawn inside the ImGui frame on the render thread, with app::UiLock held
void Draw(double fps);

// the GameInfo::message hook: window thread, each message after ImGui's; true consumes it, result is returned
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// whether the menu is showing; safe from any thread
bool Open();

// shows the menu, on its Controls tab when asked (the game's Options pages open it); any thread
void Show(bool controls);

// settings.toml options (app::Option): the fps overlay and the fps in the window title
extern bool g_fps_overlay;
extern bool g_fps_title;

}  // namespace ui
