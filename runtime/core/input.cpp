#include "core/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include "app/app.h"

namespace input {

Pad Poll(int port) {
    Pad p;
    XINPUT_STATE s{};
    p.connected = XInputGetState(DWORD(port), &s) == ERROR_SUCCESS;
    if (p.connected) {
        const XINPUT_GAMEPAD& g = s.Gamepad;
        auto b = [&](WORD m) { return (g.wButtons & m) != 0; };
        p.buttons = uint16_t((b(XINPUT_GAMEPAD_DPAD_UP) << 0) | (b(XINPUT_GAMEPAD_DPAD_DOWN) << 1) |
                             (b(XINPUT_GAMEPAD_DPAD_LEFT) << 2) | (b(XINPUT_GAMEPAD_DPAD_RIGHT) << 3) |
                             (b(XINPUT_GAMEPAD_START) << 4) | (b(XINPUT_GAMEPAD_BACK) << 5) |
                             (b(XINPUT_GAMEPAD_LEFT_THUMB) << 6) | (b(XINPUT_GAMEPAD_RIGHT_THUMB) << 7));
        auto a = [&](WORD m) { return uint8_t(b(m) ? 0xFF : 0); };
        p.analog[0] = a(XINPUT_GAMEPAD_A);
        p.analog[1] = a(XINPUT_GAMEPAD_B);
        p.analog[2] = a(XINPUT_GAMEPAD_X);
        p.analog[3] = a(XINPUT_GAMEPAD_Y);
        p.analog[4] = a(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // black
        p.analog[5] = a(XINPUT_GAMEPAD_LEFT_SHOULDER);   // white
        p.analog[6] = g.bLeftTrigger;
        p.analog[7] = g.bRightTrigger;
        p.lx = g.sThumbLX;
        p.ly = g.sThumbLY;
        p.rx = g.sThumbRX;
        p.ry = g.sThumbRY;
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    XINPUT_VIBRATION v{left, right};
    XInputSetState(DWORD(port), &v);
}

}  // namespace input
