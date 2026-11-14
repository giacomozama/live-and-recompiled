// graphics presets (common/presets.h). Placeholders for now: the settings each sets and the descriptions are to be
// filled in
#include "common/presets.h"

#include "app/app.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/lighting.h"
#include "common/water.h"

namespace presets {

namespace {

struct Info {
    const char* name;
    const char* description;
};
constexpr Info kInfo[kCount] = {
    {"Nostalgic", "Placeholder: the game as it looked on the console."},
    {"Modern", "Placeholder: the console's look, sharper and smoother."},
    {"Enhanced", "Placeholder: every improvement on."},
};

}  // namespace

const char* Name(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].name : ""; }
const char* Description(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].description : ""; }

void Apply(int preset) {
    app::Settings& s = app::Get();
    switch (preset) {
        case kNostalgic:  // placeholder
            display::SetFrameRate(30);
            s.aniso = 0;
            s.msaa = 1;
            s.smaa = false;
            s.ambient_occlusion = 0;
            s.smooth_lightmaps = false;
            fur::g_strands = false;
            fur::g_shading = false;
            lighting::g_per_pixel = false;
            water::g_dynamic = false;
            break;
        case kModern:  // placeholder
            display::SetFrameRate(60);
            s.aniso = 16;
            s.msaa = 4;
            s.smaa = false;
            s.ambient_occlusion = 0;
            s.smooth_lightmaps = true;
            fur::g_strands = false;
            fur::g_shading = true;
            lighting::g_per_pixel = false;
            water::g_dynamic = true;
            break;
        case kEnhanced:  // placeholder
            display::SetFrameRate(60);
            s.aniso = 16;
            s.msaa = 4;
            s.smaa = false;
            s.ambient_occlusion = 50;
            s.smooth_lightmaps = true;
            fur::g_strands = true;
            fur::g_shading = false;  // strands or shaded shells, never both (ui.cpp)
            lighting::g_per_pixel = true;
            water::g_dynamic = true;
            break;
        default:
            break;
    }
}

}  // namespace presets
