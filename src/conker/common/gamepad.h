#pragma once

#include <cstdint>

#include "core/input.h"

// The game's own two options, Invert Aim and Vibration, set per player from the settings menu, which its Options
// pages open instead (src/conker/game/gamepad.cpp). Single player's alone: in a match the player's profile has its own
// (the game's menu for it), which the game reads instead.
namespace gamepad {

// [playerN] invert_aim: the right stick's up aims down while aiming (the game's Invert Aim; the camera and the mouse
// keep theirs)
extern bool g_invert_aim[input::kPorts];
// [playerN] vibration: the player's controller rumbles (the game's Vibration)
extern bool g_vibration[input::kPorts];
// [playerN] vibration_percent: how hard it rumbles, 0..100 of the game's strength (in a match too)
extern uint32_t g_vibration_percent[input::kPorts];

// GameInfo::rumble: a player's rumble at their vibration_percent, in a match too
void Rumble(int port, uint16_t& left, uint16_t& right);

// a match: the player's profile inverts the camera's y (which the game read last frame)
bool MatchInvert(int port);

// a player's pad, with their keys in it, before the game gets it (keyboard::Pad): A on the frontend's Options
// opens the settings menu instead
void Pad(int port, input::Pad& pad);

// the pub's pictures (the frontend's own screen, not its multiplayer menus): Start, Escape among the keyboard's,
// opens the settings menu there. Game thread
bool Pub();

}  // namespace gamepad
