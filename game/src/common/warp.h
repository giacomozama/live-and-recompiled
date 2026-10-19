#pragma once

#include <string>
#include <vector>

struct X86Context;

// Debug warps (src/game/warp.cpp): start the game at any checkpoint, load any level at a spawn point, play any
// in-engine cutscene. The F1 menu's Debug tab and the XBE_WARP / XBE_CUTSCENE variables post requests; the main
// thread carries them out at the next frame the game can take a level change.
namespace warp {

// the game's checkpoints (its "contexts", 0x59E9A0): where each starts, and its name if it has one
struct Checkpoint {
    int level;
    int spawn;
    std::string name;
};
const std::vector<Checkpoint>& Checkpoints();

// the level table (0x69CF90): its name, or nullptr for an unused index or a level the disc lacks
constexpr int kLevels = 75;
const char* LevelName(int level);

// the running level, -1 before the first
int CurrentLevel();

// start a new game at checkpoint `index`, with the progress of the checkpoints before it
void GoCheckpoint(int index);
// a new game in `level` at spawn point `spawn`, no progress
void GoLevel(int level, int spawn);
// cutscene `index` of `level`, loading the level first if it isn't the running one; a cutscene the level lacks is
// refused (the game would otherwise stop on its missing-data screen)
void PlayCutscene(int level, int index);

// the running level's cutscenes, found when it loaded; `level` says which level they belong to
std::vector<int> Cutscenes(int* level);

// what the last request did, for the menu
std::string Status();

// the frame timer hook (display.cpp) calls this each frame on the main thread: requests run here
void Poll(X86Context& ctx);

}  // namespace warp
