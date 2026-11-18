#pragma once

#include <cstdint>

struct X86Context;

// The Xbox Store release's 41 achievements, earned and kept locally (src/conker/game/achievements.cpp): nothing goes to
// any online service. The F1 menu lists them and a toast shows each as it is earned (ui.cpp). What earns each one is
// read from the game's own state (checkpoints completed, its stat table, task records, the Dumbots match's result),
// in the spirit of the Store release's hook layer (achievements/conker_achievements.md) but not taken from it.
namespace achievements {

struct Info {
    const char* name;            // the Store release's title, in English as it has them
    const char* description[6];  // what earns it: English (the Store's), German, French, Spanish, Italian, Japanese,
                                 // with the game's own names for what the game names (its chapters, characters, modes)
    int goal;                 // a count the progress runs to (cash, tails, pads, classes), 0 none
    bool multiplayer;
    bool secret;              // its name and description hidden until earned, as in the Store release
};

constexpr int kCount = 41;

// settings.toml achievements (on by default): off, nothing is earned or counted and no toast shows; game thread reads
extern bool g_enabled;
const Info& Get(int index);

struct State {
    bool unlocked;
    int64_t when;   // unix seconds it was earned, 0 when locked
    int progress;   // toward Info::goal
};
// any thread
State StateOf(int index);
int UnlockedCount();
// forgets every unlock and count toward one, on disk too; any thread
void Reset();

// the next one earned that no toast has shown yet, -1 none; the toast's thread (ui.cpp, the render thread)
int NextToast();

// the frame timer hook (display.cpp) calls this each frame on the game thread
void Poll(X86Context& ctx);

}  // namespace achievements
