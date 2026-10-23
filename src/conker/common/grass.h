#pragma once
#include <cstdint>

// Grass patches (src/conker/game/hooks.cpp, the wrapper of 0x1A9540).
namespace grass {

// settings.toml grass_wind: patches fading out in the distance sway in the wind like the near ones, instead of
// standing still as on the console. Read on each patch, so the F1 menu applies it live.
extern bool g_wind;

// settings.toml grass_distant: patches past the near range thin out and fade as on the console (kConsole), keep all
// their blades as they fade (kFullFading), or are drawn as near ones however far, never fading or culled (kNear).
// Read on each patch.
enum : uint32_t { kConsole, kFullFading, kNear };
extern uint32_t g_distant;

}  // namespace grass
