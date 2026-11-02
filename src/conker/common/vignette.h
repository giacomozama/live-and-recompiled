#pragma once

// The game's vignette, its screen's edges darkened (src/conker/game/vignette.cpp, the hook on 0x15CAF0).
namespace vignette {

// settings.toml vignette: on (the default) draws it as the game does; off leaves it out. Only the plain dark one: a
// tinted one (a red flash when Conker is hurt) is drawn either way. XBE_VIGNETTE=0 turns it off as well. Read on each
// frame, so the F1 menu applies it live.
extern bool g_on;

}  // namespace vignette
