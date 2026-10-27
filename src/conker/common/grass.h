#pragma once
#include <cstdint>

// Grass patches (src/conker/game/hooks.cpp, the wrapper of 0x1A9540; src/conker/common/grass.cpp, the wind program's
// wake).
namespace grass {

// settings.toml grass_wind: patches fading out in the distance sway in the wind like the near ones, instead of
// standing still as on the console. Read on each patch, so the F1 menu applies it live.
extern bool g_wind;

// settings.toml grass_max_lod: patches past the near range keep all their blades instead of thinning out as on the
// console. grass_fade: they fade out with distance as on the console; off, they stay solid and are drawn however far.
// grass_max_lod off and grass_fade on is the console. Read on each patch.
extern bool g_max_lod;
extern bool g_fade;

// a blade the player passes close by, fast, bends away by about its height at most, instead of being thrown across
// the screen as on the console; hands the renderer the wake's limit, after the settings load, before boot
void Init();

}  // namespace grass
