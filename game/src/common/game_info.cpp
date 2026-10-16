#include "app/app.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/mouse.h"
#include "common/ui.h"

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Conker: Live & Reloaded",  // window and splash
    .short_title = L"Conker",             // message boxes
    .name = L"Conker",                    // window classes, crash dumps, handover event
    .log = "game",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Game"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_overlay", &ui::g_fps_overlay, nullptr},  // ImGui settings menu options (src/game/ui.cpp)
                {"fps_in_title", &ui::g_fps_title, nullptr},
                {"frame_rate", nullptr, &display::g_frame_rate},  // 30, 60 or 120 (src/game/display.cpp)
                {"skippable_cutscenes", &cutscenes::g_skippable, nullptr},  // LT skips any cutscene (hooks.cpp)
                {"grass_wind", &grass::g_wind, nullptr},  // distant grass sways too (hooks.cpp)
                {"mouse_look", &mouse::g_look, nullptr},  // the mouse turns the camera (src/game/mouse.cpp)
                {"mouse_sensitivity", nullptr, &mouse::g_sensitivity},  // percent
                {"mouse_sensitivity_y", nullptr, &mouse::g_sensitivity_y},  // percent of it, vertical only
                {"mouse_invert_y", &mouse::g_invert_y, nullptr}},
    .load = keyboard::Load,               // the [keys] table: keyboard bindings (src/game/keyboard.cpp)
    .save = keyboard::Save,               // writes it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = ui::Message,               // window messages first; true takes one
    .ui = ui::Draw,                       // ImGui drawing each frame
    .pad = keyboard::Pad,                 // keyboard as a pad on port 0 (src/game/keyboard.cpp)
    .rumble = nullptr,                    // changes a rumble before it is set
    .aspect = display::Aspect,            // the camera patch draws the display's shape (src/game/display.cpp)
};
