// Keyboard controls, so Conker can be played without a gamepad.
//
// The runtime polls the pad on the guest's threads and hands the result to GameInfo::pad (see
// app.h), the hook a title gets to change a polled pad. This fills port 0 from the host keyboard
// and reports it connected, so the frontend and the game accept input with no controller plugged
// in. A real gamepad on port 0 keeps working: the keyboard only overrides the axes and buttons it
// is actually driving.
//
// The layout (movement on the left, camera and face buttons on the right):
//
//   left stick   W A S D          camera (right stick)   I J K L
//   d-pad        arrow keys       A / B / X / Y          space / left shift / E / Q
//   LB / RB      1 / 2            LT / RT                3 / 4
//   start        enter            back                   backspace
//   stick clicks C (left)         V (right)
//
// With mouse look on (src/game/mouse.cpp) the mouse also turns the camera, over IJKL while it moves.
#include <cstdint>
#include <cstring>

#include <windows.h>

#include "common/keyboard.h"
#include "common/mouse.h"
#include "common/ui.h"
#include "core/input.h"
#include "core/log.h"

namespace keyboard {

namespace {

// Windows virtual-key codes, spelled out rather than taken from <windows.h>: the headless harness
// compiles this file against a small Win32 shim that declares only a few of them.
enum : int {
    kUp = 0x26, kDown = 0x28, kLeft = 0x25, kRight = 0x27,
    kSpace = 0x20, kEnter = 0x0D, kBackspace = 0x08, kLShift = 0xA0,
    kW = 0x57, kA = 0x41, kS = 0x53, kD = 0x44,
    kI = 0x49, kJ = 0x4A, kK = 0x4B, kL = 0x4C,
    kE = 0x45, kQ = 0x51, kC = 0x43, kV = 0x56,
    k1 = 0x31, k2 = 0x32, k3 = 0x33, k4 = 0x34,
};

// the console controller's button bit (see input::Pad)
enum : uint16_t {
    kPadUp = 1 << 0, kPadDown = 1 << 1, kPadLeft = 1 << 2, kPadRight = 1 << 3,
    kPadStart = 1 << 4, kPadBack = 1 << 5, kPadLS = 1 << 6, kPadRS = 1 << 7,
};

// XINPUT left/right thumb fully deflected
constexpr int16_t kFull = 32767;

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// one axis from its two keys: keyboard strokes are digital, so a held key is full deflection
int16_t Axis(bool negative, bool positive) {
    return int16_t((positive ? kFull : 0) - (negative ? kFull : 0));
}

}  // namespace

void Pad(int port, input::Pad& pad) {
    if (port != 0) return;
    // the keyboard is always present: without this the game sees no controller and ignores input
    pad.connected = true;

    // the settings menu is up: hand the game a neutral pad so it stands still while the player is in the UI
    if (ui::Open()) {
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
        std::memset(pad.analog, 0, sizeof pad.analog);
        return;
    }

    if (Down(kW) || Down(kA) || Down(kS) || Down(kD)) {
        pad.lx = Axis(Down(kA), Down(kD));
        pad.ly = Axis(Down(kS), Down(kW));
    }
    if (Down(kI) || Down(kJ) || Down(kK) || Down(kL)) {
        pad.rx = Axis(Down(kJ), Down(kL));
        pad.ry = Axis(Down(kK), Down(kI));
    }

    if (Down(kUp)) pad.buttons |= kPadUp;
    if (Down(kDown)) pad.buttons |= kPadDown;
    if (Down(kLeft)) pad.buttons |= kPadLeft;
    if (Down(kRight)) pad.buttons |= kPadRight;
    if (Down(kEnter)) pad.buttons |= kPadStart;
    if (Down(kBackspace)) pad.buttons |= kPadBack;
    if (Down(kC)) pad.buttons |= kPadLS;
    if (Down(kV)) pad.buttons |= kPadRS;

    // analog face buttons: A B X Y black(LB) white(RB) LT RT
    if (Down(kSpace)) pad.analog[0] = 0xFF;
    if (Down(kLShift)) pad.analog[1] = 0xFF;
    if (Down(kE)) pad.analog[2] = 0xFF;
    if (Down(kQ)) pad.analog[3] = 0xFF;
    if (Down(k2)) pad.analog[4] = 0xFF;  // black / right bumper
    if (Down(k1)) pad.analog[5] = 0xFF;  // white / left bumper
    if (Down(k3)) pad.analog[6] = 0xFF;  // left trigger
    if (Down(k4)) pad.analog[7] = 0xFF;  // right trigger

    mouse::Pad(pad);  // the mouse, while it looks, takes the right stick

    LOG_FIRST(1, "input: keyboard controls active (WASD move, IJKL camera, space A, shift B, E X, Q Y)");
}

}  // namespace keyboard
