#pragma once

// Grass patches (src/game/hooks.cpp, the wrapper of 0x1A9540).
namespace grass {

// settings.toml grass_wind: patches fading out in the distance sway in the wind like the near ones, instead of
// standing still as on the console. Read on each patch, so the F1 menu applies it live.
extern bool g_wind;

}  // namespace grass
