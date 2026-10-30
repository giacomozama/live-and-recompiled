#pragma once

#include "core/input.h"

// The game's menus (src/conker/game/menus.cpp).
namespace menus {

// from keyboard::Pad, after the mouse: while one of the game's menu lists shows, the cursor over an item moves the
// list's cursor onto it (up or down on the pad), a left click picks it (A) and the wheel steps it
void Pad(int port, input::Pad& pad);

// from mouse::Message, window thread: the wheel turned (120 a notch, away from the player positive), which steps a
// shown list's cursor up or down a notch at a time
void Wheel(int turn);

}  // namespace menus
