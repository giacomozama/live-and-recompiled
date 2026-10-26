#pragma once

#include <cstdint>

// Frame rate and aspect patches (src/conker/game/display.cpp), after the community widescreen and 60 FPS XBE patches.
namespace display {

// settings.toml frame_rate, the pictures shown a second: 30 is the console's (present every second blank), 60 the
// patched game's limit; 120, 180 and 240 run the game at 60 and draw the frames between again (the runtime's frame
// generation). XBE_FRAME_RATE overrides it. The headless harness keeps 30 unless asked, so its frame-counted pad
// scripts hold.
extern uint32_t g_frame_rate;
// the frame rate set, the runtime's fps cap and frame generation with it: the one limit (GameInfo::init applies the
// loaded one)
void SetFrameRate(uint32_t rate);
// the frames the game itself runs a second: 30 or 60
uint32_t GameRate();
// the 60 Hz ticks the game has stepped since boot, this frame's among them (the frame timer adds them as it hands them
// out): for counting in console frames, two ticks each, what the game counts in frames
uint32_t Ticks();
void Init();

// settings.toml side_by_side: a two player match splits the screen left and right instead of top and bottom, from
// the next time the match lays out its views. Experimental: the HUD pieces the game places at fixed pixels for top
// and bottom (the health frame, the blockade count) and the radar's shape are not adapted
extern bool g_side_by_side;

// GameInfo::aspect: the camera patch draws any shape, so the frame is the display's; XBE_ASPECT=W:H overrides it,
// and the headless harness keeps the console's 4:3 unless asked
float Aspect(float display);

}  // namespace display
