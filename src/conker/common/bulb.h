#pragma once

// The B pad's idea bulb (src/conker/game/hooks.cpp, the hook on 0x12338E).
namespace bulb {

// settings.toml lasting_bulb: the bulb over Conker's head stays while he stands on a B pad, instead of the console's
// two seconds (after which it shows again only once he steps off and on). Read each frame, so the F1 menu applies it
// live.
extern bool g_lasting;

}  // namespace bulb
