#pragma once

#include <cstdint>

// Multiplayer characters (src/conker/game/hooks.cpp, the detail pick of 0x7F1E0, the refresh pacing of 0x1310D0 and
// the classes' models).
namespace characters {

// settings.toml character_max_lod: characters keep their full model however far, instead of dropping to a coarser
// model with distance as on the console. Read each frame.
extern bool g_max_lod;

// settings.toml character_smooth_animation: players and bots refresh and animate every frame however far, instead of
// a slower refresh and animation with distance as on the console. Read each frame.
extern bool g_smooth_animation;

// settings.toml character_menu_models: a class (a squirrel or a Tedi, WWII and Future) is drawn in a match with its
// character select model instead of the match's: finer, and for the squirrels furred on the body and head, not only on
// the tail or hair. Read as a match loads and as each player is set up.
extern bool g_menu_models;

// a skeleton node about to hand out its bones (0x3EC7D0: node, data): a character select model's bones that its match
// model doesn't skin to set to their parents' (hooks.cpp FollowBones)
void Skeleton(uint32_t node, uint32_t data);

}  // namespace characters
