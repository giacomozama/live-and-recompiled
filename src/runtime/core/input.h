#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot
// window thread, an SDL window's (XBE_SDL): an SDL_Event, the controllers plugged in or out
void HandleEvent(const void* sdl_event);
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
// set on the device scan's polls (xapi.cpp), which only ask what is plugged in; the title's own reads leave it clear
extern thread_local bool t_presence;
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
