// graphics presets (common/presets.h): the settings each makes are in the registry (common/settings_registry.cpp).
// Placeholders for now: the values and the descriptions are to be filled in
#include "common/presets.h"

#include "app/app.h"

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

void Apply(int preset) { app::ApplyPreset(preset); }

}  // namespace presets
