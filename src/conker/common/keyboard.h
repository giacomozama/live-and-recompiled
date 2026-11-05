#pragma once

#include <string>

#include "core/input.h"

// Keyboards as controllers, so the game can be played without a gamepad: each player's keyboard and mouse buttons
// (input.h) drive their port. Wired through GameInfo::pad (see game_info.cpp); implemented in
// src/conker/game/keyboard.cpp.
//
// Keys are bound to the game's actions, not to the controller's buttons: the button an action presses follows the
// controls the game runs for the player (Scheme), single player's or their multiplayer profile's.
namespace keyboard {

// what a key can be bound to
enum Action : int {
    kMoveUp, kMoveDown, kMoveLeft, kMoveRight,  // the stick that moves
    kLookUp, kLookDown, kLookLeft, kLookRight,  // the stick that turns the camera
    kJump, kCrouch, kAttack,
    kUse,         // single player's B: the context's action (its prompts show this key)
    kAim,         // single player's B too, taking a weapon out; a match's zoom click, aiming
    kWeaponMode,  // LT: single player's first person, a match's weapon mode
    kReload,
    kZoom,        // single player's camera zoom, the right stick's click
    kAbility, kCycle, kVote, kRadio,  // multiplayer's own
    kPause, kXboxLive,
    kActions
};

// the controller's buttons, A to white in the order of input::Pad's analog buttons
enum Button : int {
    kButtonA, kButtonB, kButtonX, kButtonY, kButtonBlack, kButtonWhite, kButtonLT, kButtonRT,
    kButtonStart, kButtonBack, kButtonLS, kButtonRS,  // LS / RS the stick clicks (the d-pad does nothing in the game)
    kButtons
};

// The controls the game runs for a player, as 0x4D1F0 picks them for 0x4D320 each frame: 0 single player's, 1 to 4 a
// multiplayer profile's controller scheme (Emily, Victoria, Samantha, Elspeth). Out of a match it is single
// player's. Any thread
constexpr int kSchemes = 5;
int Scheme(int port);
const char* SchemeName(int scheme);  // "Single player", "Emily", ...
void SetScheme(int port, int scheme);  // from the 0x4D1F0 hook, game thread
// the button an action presses in a scheme, -1 for a stick's direction or an action the scheme has no button for
int Drives(int scheme, int action);
// the scheme moves with the right stick and turns the camera with the left (Samantha, Elspeth)
bool Southpaw(int scheme);

// settings.toml [playerN.keys]: up to two Windows virtual-key codes (a key or a mouse button) per action and player,
// either one drives it, 0 unbound; read live, so the menu rebinds
constexpr int kSlots = 2;
extern int g_keys[input::kPorts][kActions][kSlots];

const char* ActionLabel(int action);   // for the F1 menu, "Jump"
std::string ActionDefault(int action);  // its default keys' names, "E, Mouse 2"
const char* KeyName(int vk);           // "W", "Space", "Mouse 4"; "-" for 0, null for a key that cannot be bound
bool Bindable(int vk);                 // not Escape / F1, which open the menu
void ResetKeys(int player);              // the default layout
void ResetKey(int player, int action);   // one action's default keys
bool IsDefault(int player, int action);

// the GameInfo::load / save hooks: each [playerN] table's keys and mouse settings in settings.toml; player 1's are
// also read from the old [keys] table and mouse_ keys when its table has none
void Load(const void* table);
void Save(void* table);

// the GameInfo::pad hook: merges the port's keyboards and mice into a polled pad
void Pad(int port, input::Pad& pad);

}  // namespace keyboard
