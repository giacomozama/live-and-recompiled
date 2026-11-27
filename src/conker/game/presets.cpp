// graphics quality presets (common/presets.h): the settings each makes are in the registry
// (common/settings_registry.cpp). The values are first guesses, to be tuned
#include "common/presets.h"

#include "app/app.h"

namespace presets {

namespace {

struct Info {
    const char* name;         // IDs of the overlay text (ui_text.inc)
    const char* description;
};
constexpr Info kInfo[kCount] = {
    {"low", "hint_preset_low"},
    {"medium", "hint_preset_medium"},
    {"high", "hint_preset_high"},
    {"ultra", "hint_preset_ultra"},
};

}  // namespace

const char* Name(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].name : ""; }
const char* Description(int preset) { return preset >= 0 && preset < kCount ? kInfo[preset].description : ""; }

void Apply(int preset) { app::ApplyPreset(preset); }

}  // namespace presets
