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

// The sticks, by what they do, so a profile that swaps them takes these along (in a match too):
// [playerN] move_stick_percent: how much a partial tilt of the stick that moves does, kStickMin..kStickMax, 100 the
// game's own: the stick's distance past the dead zone raised to the power 100 / percent, its direction kept. Below
// 100 a small tilt does less, above it more; a full tilt stays full
// [playerN] look_stick_x_percent, look_stick_y_percent: how fast the camera's stick turns it left/right and up/down,
// percent of the game's speed at any tilt, kStickMin..kLookXMax and kLookYMax (Turn)
// [playerN] move_stick_dead_zone, look_stick_dead_zone: how far (percent of full, 0..kDeadZoneMax) a stick goes before
// it counts, kDeadZoneDefault the game's own
constexpr uint32_t kStickDefault = 100, kStickMin = 25, kStickMax = 300, kLookXMax = 200, kLookYMax = 250;
constexpr uint32_t kDeadZoneDefault = 30, kDeadZoneMax = 60;
extern uint32_t g_move_stick_percent[input::kPorts];
extern uint32_t g_look_stick_x_percent[input::kPorts];
extern uint32_t g_look_stick_y_percent[input::kPorts];
extern uint32_t g_move_stick_dead_zone[input::kPorts];
extern uint32_t g_look_stick_dead_zone[input::kPorts];
// the controller's sticks in the player's pad, as their dead zones and percents have them (keyboard::Pad, before the
// keys and the mouse drive them)
void Sticks(int port, input::Pad& pad, bool southpaw);
// 0x2F99A0's turn by the stick (actions, the camera's turn), after it has run and before the mouse is added: the
// frame's turn scaled by the player's look_stick percents; pitch_was the turn's pitch (+0x40) as the call began
void Turn(uint32_t actions, uint32_t turn, float pitch_was);

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
