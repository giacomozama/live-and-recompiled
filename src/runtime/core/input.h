#pragma once
#include <cstdint>
#include <string>
#include <vector>

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

// The players' devices. Each of the console's four ports (a player) holds up to one keyboard, one mouse and one
// controller. A device is named by its key: its name, then #n among the connected devices of that name ("Xbox Series
// X Controller#2"), which the settings file keeps. A keyboard or mouse slot may hold kAll instead: every device of
// that kind no other port names, which is how one keyboard the system shows as several devices stays one. The
// Wayland seat's keyboard and mouse are one device each, the seat's; X11 and Windows tell each one apart.
enum class Kind : uint8_t { Keyboard, Mouse, Controller };
constexpr int kKinds = 3;
constexpr int kPorts = 4;
inline constexpr const char* kAll = "all";

struct Device {
    Kind kind;
    uint64_t id;  // SDL's id, or the raw input handle on Windows
    std::string name, key;
    std::string label;  // the name, then (2) for the second of a name
};
std::vector<Device> Devices(Kind kind);  // connected, as they came
std::string Assigned(int port, Kind kind);  // the slot's key, kAll, or "" for none
// puts the device (a key, kAll or "") in the port's slot; another port that held it has none. Any thread
void Assign(int port, Kind kind, const std::string& key);
int Holder(Kind kind, const std::string& key);  // the port whose slot names it, -1 none
int Owner(Kind kind, uint64_t id);  // the port a connected device drives, by its key or a kAll slot; -1 none
bool Has(int port, Kind kind);  // a connected device of that kind drives the port
// a key held on the port's keyboards, or a mouse button (virtual-key codes 1 to 6) on its mice; Windows codes, the
// generic Shift, Ctrl and Alt take either side
bool KeyDown(int port, int vk);
// a controller plugged in that no port names goes to the first port without one; the settings' auto_controllers
bool AutoAssign();
void SetAutoAssign(bool on);

// the window thread's keyboards and mice; a device not added before its first key or button is added then
void AddDevice(Kind kind, uint64_t id, const std::string& name);
void RemoveDevice(Kind kind, uint64_t id);
void Key(uint64_t keyboard, int vk, bool down);
void Button(uint64_t mouse, int vk, bool down);
void ReleaseAll();  // the window lost the focus: every key and button is up

}  // namespace input
