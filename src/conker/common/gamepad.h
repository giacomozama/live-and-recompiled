#pragma once

#include <cstdint>

#include "core/input.h"

// The game's own two options, Invert Aim and Vibration, set per player from the settings menu
// (src/conker/game/hooks.cpp, the hook on 0x4D1F0).
namespace gamepad {

// [playerN] invert_aim: the right stick's up aims down while aiming (the game's Invert Aim; the camera and the mouse
// keep theirs)
extern bool g_invert_aim[input::kPorts];
// [playerN] vibration: the player's controller rumbles (the game's Vibration)
extern bool g_vibration[input::kPorts];
// [playerN] vibration_percent: how hard it rumbles, 0..100 of the game's strength
extern uint32_t g_vibration_percent[input::kPorts];

// GameInfo::rumble: a player's rumble at their vibration_percent
void Rumble(int port, uint16_t& left, uint16_t& right);

}  // namespace gamepad
