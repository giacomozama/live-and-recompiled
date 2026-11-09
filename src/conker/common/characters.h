#pragma once

// Multiplayer characters (src/conker/game/hooks.cpp, the detail pick of 0x7F1E0, the refresh pacing of 0x1310D0 and
// the classes' models).
namespace characters {

// settings.toml character_max_lod: players and bots keep their full model however far, and refresh every frame,
// instead of dropping to a coarser model and a slower refresh with distance as on the console. Read each frame.
extern bool g_max_lod;

// settings.toml character_menu_models: a class (a squirrel or a Tedi, WWII and Future) is drawn in a match with its
// character select model instead of the match's: finer, and for the squirrels furred on the body and head, not only on
// the tail or hair. Read as a match loads and as each player is set up.
extern bool g_menu_models;

}  // namespace characters
