#pragma once

// graphics presets: a few settings set together, picked on the first launch's welcome screen (ui.cpp Welcome);
// implemented in src/conker/game/presets.cpp
namespace presets {

enum Preset { kNostalgic, kModern, kEnhanced, kCount };

const char* Name(int preset);
const char* Description(int preset);  // a line or two for under the choice
void Apply(int preset);               // the preset's settings, live (as the settings menu sets them)

}  // namespace presets
