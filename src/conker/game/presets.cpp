// graphics presets (common/presets.h). Placeholders for now: the settings each sets and the descriptions are to be
// filled in
#include "common/presets.h"

#include "app/app.h"
#include "common/characters.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/grass.h"
#include "common/lighting.h"
#include "common/menus.h"
#include "common/reflection.h"
#include "common/rope.h"
#include "common/subtitles.h"
#include "common/vignette.h"
#include "common/water.h"

namespace presets {

namespace {

struct Info {
    const char* name;         // IDs of the overlay text (ui_text.inc)
    const char* description;
};
constexpr Info kInfo[kCount] = {
    {"nostalgic", "hint_as_close_as_possible_to"},
    {"modern", "hint_faithful_to_the_original_with"},
    {"enhanced", "hint_takes_advantage_of_modern_hardware"},
};

}  // namespace

const char* Name(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].name : ""; }
const char* Description(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].description : ""; }

void Apply(int preset) {
    app::Settings& s = app::Get();
    switch (preset) {
        case kNostalgic:  // placeholder
            display::SetFrameRate(30);
            s.aspect = "4:3";
            s.aniso = 0;
            s.msaa = 1;
            s.smaa = false;
            s.ambient_occlusion = 0;
            s.smooth_lightmaps = false;
            fur::g_strands = false;
            fur::g_shading = false;
            lighting::g_per_pixel = false;
            water::g_dynamic = false;
            s.render_percent = 100;
            s.vsync = true;
            s.fsr = false;
            s.fsr_sharpness = 80;
            s.fur_sharpness = 0;
            s.film_grain = 0;
            s.chromatic_aberration = 0;
            s.motion_blur = 0;
            s.ambient_occlusion_radius = 50;
            s.fog = app::Fog::Enabled;
            s.lightmap_smoothing = 5;
            s.lightmap_size = 512;
            vignette::g_on = true;
            fur::g_distant = false;
            characters::g_max_lod = false;
            characters::g_smooth_animation = false;
            characters::g_menu_models = false;
            grass::g_wind = false;
            grass::g_max_lod = false;
            grass::g_fade = true;
            reflection::g_floor_depth = false;
            display::g_hud_split = false;
            menus::g_bright_hints = false;
            cutscenes::g_english_bubbles = false;
            subtitles::g_redraw = false;
            rope::g_physics = false;
            break;
        case kModern:  // placeholder
            display::SetFrameRate(60);
            s.aniso = 16;
            s.msaa = 4;
            s.smaa = false;
            s.ambient_occlusion = 0;
            s.smooth_lightmaps = false;
            fur::g_strands = false;
            fur::g_shading = false;
            lighting::g_per_pixel = false;
            water::g_dynamic = true;
            s.render_percent = 100;
            s.vsync = true;
            s.fsr = false;
            s.fsr_sharpness = 80;
            s.fur_sharpness = 0;
            s.film_grain = 0;
            s.chromatic_aberration = 0;
            s.motion_blur = 0;
            s.ambient_occlusion_radius = 50;
            s.fog = app::Fog::Enabled;
            s.lightmap_smoothing = 5;
            s.lightmap_size = 512;
            vignette::g_on = true;
            fur::g_distant = true;
            characters::g_max_lod = true;
            characters::g_smooth_animation = true;
            characters::g_menu_models = false;
            grass::g_wind = true;
            grass::g_max_lod = true;
            grass::g_fade = false;
            reflection::g_floor_depth = true;
            display::g_hud_split = true;
            menus::g_bright_hints = true;
            cutscenes::g_english_bubbles = true;
            subtitles::g_redraw = !subtitles::g_on;  // one or the other
            rope::g_physics = true;
            break;
        case kEnhanced:  // placeholder
            display::SetFrameRate(display::HighestFrameRate());  // picked once, for this monitor
            s.aniso = 16;
            s.msaa = 4;
            s.smaa = false;
            s.ambient_occlusion = 100;
            s.smooth_lightmaps = true;
            fur::g_strands = true;
            fur::g_shading = false;  // strands or shaded shells, never both (ui.cpp)
            lighting::g_per_pixel = true;
            water::g_dynamic = true;
            s.render_percent = 100;
            s.vsync = true;
            s.fsr = true;
            s.fsr_sharpness = 80;
            s.fur_sharpness = 0;
            s.film_grain = 0;
            s.chromatic_aberration = 0;
            s.motion_blur = 0;
            s.ambient_occlusion_radius = 50;
            s.fog = app::Fog::Enabled;
            s.lightmap_smoothing = 5;
            s.lightmap_size = 1024;
            vignette::g_on = true;
            fur::g_distant = true;
            characters::g_max_lod = true;
            characters::g_smooth_animation = true;
            characters::g_menu_models = false;
            grass::g_wind = true;
            grass::g_max_lod = true;
            grass::g_fade = false;
            reflection::g_floor_depth = true;
            display::g_hud_split = true;
            menus::g_bright_hints = true;
            cutscenes::g_english_bubbles = true;
            subtitles::g_redraw = !subtitles::g_on;  // one or the other
            rope::g_physics = true;
            break;
        default:
            break;
    }
}

}  // namespace presets
