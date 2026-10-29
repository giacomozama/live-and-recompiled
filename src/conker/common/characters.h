#pragma once

// Multiplayer characters (src/conker/game/hooks.cpp, the detail pick of 0x7F1E0 and the refresh pacing of 0x1310D0).
namespace characters {

// settings.toml character_max_lod: players and bots keep their full model however far, and refresh every frame,
// instead of dropping to a coarser model and a slower refresh with distance as on the console. Read each frame.
extern bool g_max_lod;

}  // namespace characters
