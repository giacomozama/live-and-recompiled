#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init(void* hwnd);
// keyboard and mouse buttons are merged into port 0, which is always connected
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);
void SetBlocked(bool blocked);  // the options panel takes the input

// window thread
void OnRawInput(void* handle);
void OnClick();
void ReleaseCursor();
// raw mouse counts since the last call, scaled by the settings; false when there are none
bool TakeMouse(float& dx, float& dy);

// a game menu takes the input: the cursor is free and the mouse buttons stop firing
void SetMenu(bool menu);
// the directions the move keys push port 0's stick (up, down, left, right as bits 0-3), whatever its length
uint32_t StickKeys();
bool TakeEvent(int event);  // an event action pressed since the last call
// holds pad buttons (Pad::buttons bits) long enough for the game to read one press
void TapButtons(uint16_t buttons);

// blocks until a key, mouse button or pad button goes down in the game window; those held on entry must come up
void WaitForPress();
// the same press, polled: one went down since the last call (the first call only notes what is held)
bool Pressed();
bool InMenu();
// the keyboard or mouse was used after the pad's last new press
bool KeyboardActive();
// the cursor in window client pixels and the client size; false when the game is not taking input
bool Cursor(int& x, int& y, int& w, int& h);
uint32_t Clicks();

// actions the keyboard and mouse can be bound to, key names comma separated
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
    // what it presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits, 16-19 the move keys
    int target = -1;
    bool play_only = false;  // a pad button that also moves through menus, pressed only in play
    int event = 0;  // not 0: no pad button, the game side acts on it once per press (TakeEvent)
};
constexpr size_t kMaxActions = 27;  // the keyboard activity mask holds the actions in bits 0-26

// what the game adds to the keyboard, all optional, called while port 0 polls
struct Game {
    const Action* actions = nullptr;
    size_t count = 0;
    void (*begin)(bool menu) = nullptr;  // before the actions
    int (*press)(int target, bool menu) = nullptr;  // an action going down: the target it goes down as
    bool (*event)(Pad& p, int event) = nullptr;  // an event action held: true when it went to the pad instead
    void (*buttons)(Pad& p, uint32_t targets, bool menu) = nullptr;  // after the actions, the targets the keys pressed
    float (*speed)(int& y, bool menu) = nullptr;  // the move keys' stick length, before the arrows
    float (*diagonal)(int x, int y, float v) = nullptr;
    bool (*stick)(Pad& p, const int move[4], bool menu) = nullptr;  // true when it took the move keys
    void (*wheel)(int notch) = nullptr;  // window thread, out of menus
};
void SetGame(const Game& game);  // before LoadBindings

const Action* Actions(size_t* count);
std::string Binding(size_t action);
void Bind(size_t action, int vk);
void ResetBindings();
void LoadBindings();  // after the settings load
int PressedKey();     // a bindable key or mouse button held now, 0 for none

}  // namespace input
