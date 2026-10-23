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

// settings.toml [playerN.keys]: up to two Windows virtual-key codes (a key or a mouse button) per action and player,
// either one drives it, 0 unbound; read live, so the menu rebinds
constexpr int kSlots = 2;
extern int g_keys[input::kPorts][kActions][kSlots];

const char* ActionLabel(int action);  // for the F1 menu, "Move up"
std::string ActionDefault(int action);  // its default keys' names, "E, Mouse 2"
const char* KeyName(int vk);          // "W", "Space", "Mouse 4"; "-" for 0, null for a key that cannot be bound
bool Bindable(int vk);                // not Escape / Insert, which open the menu
void ResetKeys(int player);           // the default layout
void ResetKey(int player, int action);  // one action's default keys
bool IsDefault(int player, int action);

// the GameInfo::load / save hooks: each [playerN] table's keys and mouse settings in settings.toml; player 1's are
// also read from the old [keys] table and mouse_ keys when its table has none
void Load(const void* table);
void Save(void* table);

// the GameInfo::pad hook: merges the port's keyboards and mice into a polled pad
void Pad(int port, input::Pad& pad);

}  // namespace keyboard
