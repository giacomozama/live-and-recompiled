#pragma once

// Ropes Conker climbs (src/conker/game/rope.cpp, the wrapper of 0x158C10 and the hook at 0x158E38).
namespace rope {

// settings.toml rope_physics: ropes swing as a weighted chain, solved in small steps of the frame's time, instead of
// the console's one pass a frame, whose stiffness and impulses make them writhe. Read on each step, so the F1 menu
// applies it live.
extern bool g_physics;

}  // namespace rope
