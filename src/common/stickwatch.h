#pragma once

// XBE_STICK_WATCH=1: finds every function that reads player 1's right stick (src/game/stickwatch.cpp), for a mouse
// look that turns each camera where it takes the stick. F11 starts watching the stick in player 1's actions, F11 again
// stops and logs each reader ("stickwatch:" lines) with the calls that led to it. Linux only; nothing runs without
// the variable.
namespace stickwatch {

// from keyboard::Pad, port 0
void Tick();

}  // namespace stickwatch
