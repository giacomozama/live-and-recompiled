#pragma once

// graphics presets: a few settings set together, picked on the first launch's welcome screen (ui.cpp Welcome);
// implemented in src/conker/game/presets.cpp
namespace presets {

enum Preset { kLow, kMedium, kHigh, kUltra, kCount };

const char* Name(int preset);
void Apply(int preset);               // the preset's settings, live (as the settings menu sets them)

}  // namespace presets
