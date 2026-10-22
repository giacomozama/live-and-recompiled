#pragma once

#include <string>

namespace input {
struct Pad;
}

// Keyboard as a controller on port 0, so the game can be played without a gamepad. Wired through
// GameInfo::pad (see game_info.cpp); implemented in src/game/keyboard.cpp.
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

// settings.toml [keys]: up to two Windows virtual-key codes (a key or a mouse button) per action, either one drives
// it, 0 unbound; read live, so the F1 menu rebinds
constexpr int kSlots = 2;
extern int g_keys[kActions][kSlots];

const char* ActionLabel(int action);  // for the F1 menu, "Move up"
std::string ActionDefault(int action);  // its default keys' names, "E, Mouse 2"
const char* KeyName(int vk);          // "W", "Space", "Mouse 4"; "-" for 0, null for a key that cannot be bound
bool Bindable(int vk);                // not Escape / Insert, which open the menu
void ResetKeys();                     // the default layout
void ResetKey(int action);            // one action's default: its key in the first slot, the second empty
bool IsDefault(int action);

// the GameInfo::load / save hooks: the [keys] table of settings.toml
void Load(const void* table);
void Save(void* table);

// the GameInfo::pad hook: merges the host keyboard into a polled pad
void Pad(int port, input::Pad& pad);

}  // namespace keyboard
