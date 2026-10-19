#pragma once

#include <cstdint>

// Frame rate and aspect patches (src/game/display.cpp), after the community widescreen and 60 FPS XBE patches.
namespace display {

// settings.toml frame_rate: 30 is the console's (present every second blank), 60 or 120 the patched game's limit;
// XBE_FRAME_RATE overrides it. The headless harness keeps 30 unless asked, so its frame-counted pad scripts hold.
extern uint32_t g_frame_rate;
// the frame rate set, the runtime's fps cap with it: the one limit (GameInfo::init applies the loaded one)
void SetFrameRate(uint32_t rate);
void Init();

// GameInfo::aspect: the camera patch draws any shape, so the frame is the display's; XBE_ASPECT=W:H overrides it,
// and the headless harness keeps the console's 4:3 unless asked
float Aspect(float display);

}  // namespace display
