#include "app/app.h"
#include "common/achievements.h"
#include "common/characters.h"
#include "common/cutscenes.h"
#include "common/subtitles.h"
#include "common/game_fonts.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/gamepad.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/lighting.h"
#include "common/link.h"
#include "common/loading.h"
#include "common/medbeam.h"
#include "common/menus.h"
#include "common/prompts.h"
#include "common/reflection.h"
#include "common/rope.h"
#include "common/settings_registry.h"
#include "common/hud_art.h"
#include "common/ui.h"
#include "common/vignette.h"
#include "common/water.h"

namespace {

// the shader records shipped with the game (app::GameInfo::base_shaders): a shaders.rec recorded through the game's
// soak test (LT + RT + Black on the main menu, or the debug menu's Start soak test: every task of the story back to
// back) and multiplayer matches, its header's layout set to gpu::kRecordLayout. Record again when kRecordRevision moves
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
const uint8_t kBaseShaders[] = {
#embed "../base_shaders.rec"
};
#pragma clang diagnostic pop

// after the settings load: the fps cap follows frame_rate, the fur, water and grass programs get their shading,
// then the lit programs theirs, reflective floors their depth
void Init() {
    display::Init();
    fur::Init();
    water::Init();
    grass::Init();
    grass::Update();  // the fogged grass entry as the settings have it from the start (gpu::shader_gen::Stamp reads it)
    lighting::Init();
    reflection::Init();
    game_fonts::Init();
    hud_art::Init();
}

// the settings file's tables: each player's ([playerN]) and the models' fur looks ([fur_models])
void LoadTables(const void* table) {
    keyboard::Load(table);
    fur::Load(table);
    game_fonts::Load(table);
}

void SaveTables(void* table) {
    keyboard::Save(table);
    fur::Save(table);
    game_fonts::Save(table);
}

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Live & Recompiled",        // window
    .short_title = L"Live & Recompiled",  // message boxes
    .name = L"Conker",                    // window classes, crash dumps, handover event
    .log = "game",                        // log file prefix
    .settings_file = "settings.toml",     // in app::ConfigDir
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Game"}},  // each recompiled xbe and its exe, for title launches
    .settings = settings::Registry,  // src/conker/common/settings_registry.cpp
    // the [playerN] tables: each player's keyboard bindings, mouse look, invert aim and vibration
    // (src/conker/game/keyboard.cpp, mouse.cpp, hooks.cpp); [fur_models]: each model's super fur look (fur.cpp)
    .load = LoadTables,
    .save = SaveTables,                   // writes them
    .init = Init,                         // after the settings load: the fps cap, fur and waves
    .loaded = fur::ImageLoaded,           // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = ui::Message,               // window messages first; true takes one
    .ui = ui::Draw,                       // ImGui drawing each frame
    .pad = keyboard::Pad,                 // each player's keyboard and mouse as their pad (src/conker/game/keyboard.cpp)
    .rumble = gamepad::Rumble,            // each player's rumble at their vibration % (src/conker/game/hooks.cpp)
    .aspect = display::Aspect,            // the camera patch draws the display's shape (src/conker/game/display.cpp)
    .welcome = ui::Welcome,               // the first launch's language and graphics preset, before boot (ui.cpp)
    .base_shaders = kBaseShaders,         // src/conker/base_shaders.rec: the soak test and multiplayer, recorded
    .base_shaders_size = sizeof kBaseShaders,
};
