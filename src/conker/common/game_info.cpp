#include "app/app.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/glass.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/metal.h"
#include "common/ui.h"
#include "common/water.h"

namespace {

// after the settings load: the fps cap follows frame_rate, the fur, water, metal and glass programs get their shading
void Init() {
    display::Init();
    fur::Init();
    water::Init();
    glass::Init();  // before metal: the skinned bottles' program has metal's match too
    metal::Init();
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Live & Recompiled",        // window and splash
    .short_title = L"Live & Recompiled",  // message boxes
    .name = L"Conker",                    // window classes, crash dumps, handover event
    .log = "game",                        // log file prefix
    .settings = "settings.toml",          // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Game"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_overlay", &ui::g_fps_overlay, nullptr},  // ImGui settings menu options (src/conker/game/ui.cpp)
                {"fps_in_title", &ui::g_fps_title, nullptr},
                {"frame_rate", nullptr, &display::g_frame_rate},  // 30, 60, 120, 180 or 240 (src/conker/game/display.cpp)
                {"side_by_side", &display::g_side_by_side, nullptr},  // two players split left and right (display.cpp)
                {"skippable_cutscenes", &cutscenes::g_skippable, nullptr},  // LT skips any cutscene (hooks.cpp)
                {"grass_wind", &grass::g_wind, nullptr},  // distant grass sways too (hooks.cpp)
                {"fur_shading", &fur::g_shading, nullptr},  // lit, soft edged fur shells (src/conker/common/fur.cpp)
                {"dynamic_water", &water::g_dynamic, nullptr},  // flowing, rippling water (src/conker/common/water.cpp)
                {"metal_shading", &metal::g_shading, nullptr},  // shiny, dented armour (src/conker/common/metal.cpp)
                {"glass_shading", &glass::g_shading, nullptr}},  // glowing, glinting bottles (src/conker/common/glass.cpp)
    // the [playerN] tables: each player's keyboard bindings and mouse look (src/conker/game/keyboard.cpp, mouse.cpp)
    .load = keyboard::Load,
    .save = keyboard::Save,               // writes them
    .init = Init,                         // after the settings load: the fps cap, fur, waves, metal and glass
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = ui::Message,               // window messages first; true takes one
    .ui = ui::Draw,                       // ImGui drawing each frame
    .pad = keyboard::Pad,                 // each player's keyboard and mouse as their pad (src/conker/game/keyboard.cpp)
    .rumble = nullptr,                    // changes a rumble before it is set
    .aspect = display::Aspect,            // the camera patch draws the display's shape (src/conker/game/display.cpp)
};
