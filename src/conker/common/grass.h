#pragma once
#include <cstdint>

// Grass patches (src/conker/game/hooks.cpp, the wrapper of 0x1A9540; src/conker/common/grass.cpp, the wind program's
// wake).
namespace grass {

// settings.toml grass_wind: patches fading out in the distance sway in the wind like the near ones, instead of
// standing still as on the console. Read on each patch, so the F1 menu applies it live.
extern bool g_wind;

// settings.toml grass_distant: patches past the near range thin out and fade as on the console (kConsole), keep all
// their blades as they fade (kFullFading), or are drawn as near ones however far, never fading or culled (kNear).
// Read on each patch.
enum : uint32_t { kConsole, kFullFading, kNear };
extern uint32_t g_distant;

// settings.toml grass_wake: a blade the player passes close by, fast, bends away by about its height at most, instead
// of being thrown across the screen as on the console. Read on each draw, so the F1 menu applies it live.
extern bool g_wake;

// hands the renderer the wake's limit; after the settings load, before boot
void Init();

}  // namespace grass
