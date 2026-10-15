#include "app/app.h"
#include "common/keyboard.h"
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
                {"fps_in_title", &ui::g_fps_title, nullptr}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = ui::Message,               // window messages first; true takes one
    .ui = ui::Draw,                       // ImGui drawing each frame
    .pad = keyboard::Pad,                 // keyboard as a pad on port 0 (src/game/keyboard.cpp)
    .rumble = nullptr,                    // changes a rumble before it is set
};
