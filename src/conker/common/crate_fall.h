#pragma once

#include <cstdint>

// The Creaky Crate's slam (src/conker/game/crate_fall.cpp): its hop ends in the air and the next clip starts on the
// ground, so it fell in one tick; the jump is carried and eased out like a fall. Called from the player draw and the
// skeleton node hooks (squirrel_tail.cpp).
namespace crate_fall {

// a player's draw (0x7FB00), the player's address: notes a crate's skeleton node
void NoteDraw(uint32_t player);

// a skeleton node (0x3EC7D0: node, data) before it hands its bone matrices on
void Skeleton(uint32_t node, uint32_t data);

}  // namespace crate_fall
