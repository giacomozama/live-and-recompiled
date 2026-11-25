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
    .title = L"Live & Recompiled",        // window and splash
    .short_title = L"Live & Recompiled",  // message boxes
    .name = L"Conker",                    // window classes, crash dumps, handover event
    .log = "game",                        // log file prefix
    .settings = "settings.toml",          // settings file in app::ConfigDir
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "Game"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_overlay", &ui::g_fps_overlay, nullptr},  // ImGui settings menu options (src/conker/game/ui.cpp)
                {"menu_resizable", &ui::g_menu_resizable, nullptr},  // Debug: the settings menu moved and sized by the mouse (ui.cpp)
                {"fast_forward", &display::g_fast_forward_enabled, nullptr},  // Debug: F10 and the Debug tab fast forward the game, off by default (display.cpp)
                {"menu_decorations", &ui::g_decorations, nullptr},  // outlines round the settings menu's sections and widgets (ui.cpp)
                {"ui_accent", nullptr, &ui::g_accent},  // the settings menu's accent color, 0 blue to 6 purple, 4 orange by default (ui_font.cpp)
                {"frame_rate", nullptr, &display::g_frame_rate},  // 30, 60, 120, 180 or 240; 0, the old Highest, moves to a rate (src/conker/game/display.cpp)
                {"fov", nullptr, &display::g_fov},  // the view's height in degrees in play, 50 the console's (display.cpp)
                {"look_up", nullptr, &display::g_look_up},  // degrees the third person camera tilts up past the console's (display.cpp)
                {"hud_split", &display::g_hud_split, nullptr},  // a match's HUD unstretched, at the display's edges (display.cpp)
                {"player_tags", nullptr, &display::g_player_tags},  // over other players: 0 names, 1 markers, 2 off (player_tags.cpp)
                {"player_ranks", &display::g_player_ranks, nullptr},  // the rank emblem in the tag over other players (player_tags.cpp)
                {"health_bars", nullptr, &display::g_health_bars},  // other players' health bars: 0 the game's beside them, 1 instead of the name or marker, 2 none (player_tags.cpp)
                {"player_outlines", &display::g_player_outlines, nullptr},  // other players outlined in a match (player_tags.cpp)
                {"medgun_beam", &medbeam::g_enabled, nullptr},  // the medgun's heal beam in a match (medbeam.cpp)
                {"sharp_drone_view", &display::g_sharp_drone, nullptr},  // the Sneeker drone's view sharp, no haze (display.cpp)
                {"team_colors", nullptr, &display::g_team_colors},  // a match's team colours: 0 green/red, 1 blue/orange (player_tags.cpp)
                {"no_friendly_fire", &syslink::g_no_friendly_fire, nullptr},  // a match's teammates don't hurt each other; online the host's (friendly_fire.cpp)
                {"achievements", &achievements::g_enabled, nullptr},  // achievements are earned, locally (achievements.cpp)
                {"skippable_cutscenes", &cutscenes::g_skippable, nullptr},  // LT skips any cutscene (hooks.cpp)
                {"english_bubbles", &cutscenes::g_english_bubbles, nullptr},  // the throne monologues' bubbles in English (hooks.cpp)
                {"speech_subtitles", &subtitles::g_on, nullptr},  // speech bubbles shown as subtitles instead (subtitles.cpp)
                {"redrawn_bubbles", &subtitles::g_redraw, nullptr},  // speech bubbles drawn again by the runtime (bubbles.cpp)
                {"fade_loads", &loading::g_fade, nullptr},  // a fade to black instead of loading screens (hooks.cpp)
                {"rope_physics", &rope::g_physics, nullptr},  // ropes swing as a weighted chain (rope.cpp)
                {"grass_wind", &grass::g_wind, nullptr},  // distant grass sways too (hooks.cpp)
                {"grass_max_lod", &grass::g_max_lod, nullptr},  // distant grass keeps all its blades (hooks.cpp)
                {"grass_fade", &grass::g_fade, nullptr},  // distant grass fades out, else solid however far (hooks.cpp)
                {"fur_shading", &fur::g_shading, nullptr},  // lit, soft edged fur shells (src/conker/common/fur.cpp)
                {"fur_strands", &fur::g_strands, nullptr},  // super fur: procedural strands; each model's look in [fur_models] (fur.cpp)
                {"fur_distant", &fur::g_distant, nullptr},  // distant fur keeps all its shells (hooks.cpp)
                {"character_max_lod", &characters::g_max_lod, nullptr},  // characters keep their full model (hooks.cpp)
                {"character_smooth_animation", &characters::g_smooth_animation, nullptr},  // far players animate every frame (hooks.cpp)
                {"character_menu_models", &characters::g_menu_models, nullptr},  // the classes' character select models (hooks.cpp)
                {"dynamic_water", &water::g_dynamic, nullptr},  // flowing, rippling water (src/conker/common/water.cpp)
                {"per_pixel_lighting", &lighting::g_per_pixel, nullptr},  // lit per pixel (src/conker/common/lighting.cpp)
                {"floor_depth", &reflection::g_floor_depth, nullptr},  // floors over reflections blur by their own depth (reflection.cpp)
                {"vignette", &vignette::g_on, nullptr},  // the game's darkened screen edges (src/conker/game/vignette.cpp)
                {"bright_menu_hints", &menus::g_bright_hints, nullptr},  // solid page hints (src/conker/game/menus.cpp)
                {"skip_intro_fmvs", &menus::g_skip_intro, nullptr},  // the launch movies skipped (menus.cpp, hooks.cpp)
                {"attract_video", &menus::g_attract, nullptr},  // the idle menu's attract video (menus.cpp)
                {"keyboard_prompts", &prompts::g_on, nullptr},  // button icons drawn as keys on kb+mouse (prompts.cpp)
                {"redrawn_hud", &hud_art::g_on, nullptr}},  // HUD art drawn from vector pictures (src/conker/common/hud_art.cpp)
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
