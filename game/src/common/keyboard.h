#pragma once

namespace input {
struct Pad;
}

// Keyboard as a controller on port 0, so the game can be played without a gamepad. Wired through
// GameInfo::pad (see game_info.cpp); implemented in src/game/keyboard.cpp.
namespace keyboard {

// the GameInfo::pad hook: merges the host keyboard into a polled pad
void Pad(int port, input::Pad& pad);

}  // namespace keyboard
