// graphics quality presets (common/presets.h): the settings each makes are in the registry
// (common/settings_registry.cpp). The values are first guesses, to be tuned
#include "common/presets.h"

#include "app/app.h"

namespace presets {

namespace {

constexpr const char* kNames[kCount] = {"low", "medium", "high", "ultra"};  // IDs of the overlay text (ui_text.inc)

}  // namespace

const char* Name(int preset) { return preset >= 0 && preset < kCount ? kNames[preset] : ""; }

void Apply(int preset) { app::ApplyPreset(preset); }

}  // namespace presets
