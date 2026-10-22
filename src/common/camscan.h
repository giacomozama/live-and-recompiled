#pragma once

namespace input {
struct Pad;
}

// XBE_CAMSCAN=1: a search for the camera's turn angles (src/game/camscan.cpp). A background thread watches every
// float in guest memory and scores how often it moves the way the right stick points; F6 starts over, F7 logs the
// best candidates ("camscan:" lines), F8 nudges each to see which the camera keeps. Nothing runs without the variable.
namespace camscan {

// from keyboard::Pad, port 0, with the pad the game will get
void Tick(const input::Pad& pad);

}  // namespace camscan
