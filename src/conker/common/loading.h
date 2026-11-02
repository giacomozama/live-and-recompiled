#pragma once

// Loading screens (src/conker/game/hooks.cpp, the hook on 0x2ACB20).
namespace loading {

// settings.toml fade_loads: a load fades to black and back instead of drawing the loading screen, whose presents the
// loader waited on (most of a load). Off: the loading screen as on the console. Read on each screen, so the F1 menu
// applies it live.
extern bool g_fade;

}  // namespace loading
