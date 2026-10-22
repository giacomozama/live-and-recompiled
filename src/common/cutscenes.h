#pragma once

// In-engine cutscene skipping (src/game/hooks.cpp, the hook on 0x4FF00).
namespace cutscenes {

// settings.toml skippable_cutscenes: LT skips any in-engine cutscene, seen or not, not just the ones seen this
// playthrough; XBE_SKIPPABLE_CUTSCENES turns it on as well. Read on each test, so the F1 menu applies it live.
extern bool g_skippable;

}  // namespace cutscenes
