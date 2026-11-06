#pragma once

// In-engine cutscene skipping (src/conker/game/hooks.cpp, the hook on 0x4FF00).
namespace cutscenes {

// settings.toml skippable_cutscenes (on by default): holding any button but pause and quit for a second skips
// any in-engine cutscene, seen or not, not just the ones seen this playthrough (LT, with it off); the
// XBE_SKIPPABLE_CUTSCENES variable turns it on as well. Read on each test, so the F1 menu applies it live.
extern bool g_skippable;

// how far the hold to skip has come, 0 to 1; 0 when nothing is held or no cutscene is testing for it. Any thread
float Hold();

}  // namespace cutscenes
