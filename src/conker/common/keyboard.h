#pragma once

#include <string>

#include "core/input.h"

// Keyboards as controllers, so the game can be played without a gamepad: each player's keyboard and mouse buttons
// (input.h) drive their port. Wired through GameInfo::pad (see game_info.cpp); implemented in
// src/conker/game/keyboard.cpp.
namespace keyboard {

// what a key can drive: a stick direction or a controller button
enum Action : int {
    kMoveUp, kMoveDown, kMoveLeft, kMoveRight,  // left stick
    kCamUp, kCamDown, kCamLeft, kCamRight,      // right stick
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
    kA, kB, kX, kY,
    kLB, kRB, kLT, kRT,  // LB is the console's white button, RB its black
    kStart, kBack,
    kLS, kRS,  // stick clicks
    kActions
};

// each player has two sets of controls, keys and mouse settings alike: one for the story and every menu, one for
// multiplayer matches (which load past the story's levels; the multiplayer frontend keeps the first)
enum Mode : int { kSinglePlayer, kMultiplayer, kModes };
int ActiveMode();  // the set the running level uses; any thread

// settings.toml [playerN.keys] (multiplayer: [playerN.multiplayer.keys]): up to two Windows virtual-key codes (a key
// or a mouse button) per action and player, either one drives it, 0 unbound; read live, so the menu rebinds
constexpr int kSlots = 2;
extern int g_keys[kModes][input::kPorts][kActions][kSlots];

const char* ActionLabel(int action);  // for the F1 menu, "Move up"
std::string ActionDefault(int action);  // its default keys' names, "E, Mouse 2"
const char* KeyName(int vk);          // "W", "Space", "Mouse 4"; "-" for 0, null for a key that cannot be bound
bool Bindable(int vk);                // not Escape / Insert, which open the menu
void ResetKeys(int mode, int player);              // the default layout
void ResetKey(int mode, int player, int action);   // one action's default keys
bool IsDefault(int mode, int player, int action);
void CopyToMultiplayer(int player);  // single player's keys and mouse settings into multiplayer's

// the GameInfo::load / save hooks: each [playerN] table's keys and mouse settings in settings.toml, multiplayer's in
// its [playerN.multiplayer] (single player's when it has none); player 1's are also read from the old [keys] table
// and mouse_ keys when its table has none
void Load(const void* table);
void Save(void* table);

// the GameInfo::pad hook: merges the port's keyboards and mice into a polled pad
void Pad(int port, input::Pad& pad);

}  // namespace keyboard
