// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report; the players'
// keyboards and mice from the window thread, each device's keys and buttons apart
#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <iterator>
#include <mutex>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// the connected devices, as they came, and what each port's slots name; under g_lock
struct Dev {
    Kind kind;
    uint64_t id;
    std::string name, key;
    SDL_Gamepad* pad = nullptr;
    std::bitset<256> down;  // keys or buttons held, by virtual-key code
};
std::mutex g_lock;
bool g_sdl = false;
std::vector<Dev> g_devices;
std::string g_slots[kPorts][kKinds] = {{kAll, kAll, ""}};  // player 1 has the keyboards and the mice
std::atomic<bool> g_auto{true};

const char* KindName(Kind k) { return k == Kind::Keyboard ? "keyboard" : k == Kind::Mouse ? "mouse" : "controller"; }

std::string& Slot(int port, Kind kind) { return g_slots[port][int(kind)]; }

Dev* Find(Kind kind, uint64_t id) {
    for (Dev& d : g_devices)
        if (d.kind == kind && d.id == id) return &d;
    return nullptr;
}

// name#n, the lowest n no connected device of the kind and name has
std::string NewKey(Kind kind, const std::string& name) {
    for (int n = 1;; n++) {
        std::string key = name + "#" + std::to_string(n);
        if (std::none_of(g_devices.begin(), g_devices.end(), [&](const Dev& d) { return d.kind == kind && d.key == key; }))
            return key;
    }
}

int OwnerOf(const Dev& d) {
    int all = -1;
    for (int p = 0; p < kPorts; p++) {
        if (Slot(p, d.kind) == d.key) return p;
        if (Slot(p, d.kind) == kAll) all = p;
    }
    return d.kind == Kind::Controller ? -1 : all;
}

bool HasLocked(int port, Kind kind) {
    return std::any_of(g_devices.begin(), g_devices.end(),
                       [&](const Dev& d) { return d.kind == kind && OwnerOf(d) == port; });
}

Dev& Add(Kind kind, uint64_t id, const std::string& name) {
    if (Dev* d = Find(kind, id)) return *d;
    Dev d{kind, id, name.empty() ? std::string(kind == Kind::Keyboard ? "Keyboard" : "Mouse") : name, ""};
    d.key = NewKey(kind, d.name);
    g_devices.push_back(std::move(d));
    LOGI("input: %s %s connected", KindName(kind), g_devices.back().key.c_str());
    return g_devices.back();
}

void AddPad(SDL_JoystickID id) {
    if (Find(Kind::Controller, id)) return;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        LOGI("input: controller failed to open: %s", SDL_GetError());
        return;
    }
    const char* name = SDL_GetGamepadName(pad);
    Dev& d = Add(Kind::Controller, id, name && *name ? name : "Controller");
    d.pad = pad;
    if (!g_auto.load() || std::any_of(std::begin(g_slots), std::end(g_slots), [&](const std::string(&s)[kKinds]) {
            return s[int(Kind::Controller)] == d.key;
        }))
        return;
    for (int p = 0; p < kPorts; p++) {
        if (HasLocked(p, Kind::Controller)) continue;
        Slot(p, Kind::Controller) = d.key;
        LOGI("input: controller %s goes to player %d", d.key.c_str(), p + 1);
        return;
    }
}

void Remove(Kind kind, uint64_t id) {
    auto it = std::find_if(g_devices.begin(), g_devices.end(), [&](const Dev& d) { return d.kind == kind && d.id == id; });
    if (it == g_devices.end()) return;
    if (it->pad) SDL_CloseGamepad(it->pad);
    LOGI("input: %s %s disconnected", KindName(kind), it->key.c_str());
    g_devices.erase(it);
}

SDL_Gamepad* PadOf(int port) {
    if (port < 0 || port >= kPorts) return nullptr;
    const std::string& key = Slot(port, Kind::Controller);
    for (const Dev& d : g_devices)
        if (d.kind == Kind::Controller && d.key == key) return d.pad;
    return nullptr;
}

// once a millisecond at most, however many ports and threads read. With an SDL window (XBE_SDL) its thread pumps the
// events and hands the controllers' over (HandleEvent): SDL's video must not be pumped from another thread.
void UpdatePads() {
#ifndef XBE_SDL
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) Remove(Kind::Controller, e.gdevice.which);
    }
#endif
}

}  // namespace

void Init() {
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices. The Windows build here is Zig's mingw-w64, whose headers have no
    // windows.gaming.input.h, so SDL compiles without WGI: XInput and DirectInput stay on for the Xbox pads it would
    // have handled. HIDAPI's Xbox driver is off by default on Windows, so with them off no Xbox pad is seen at all.
    // RawInput stays off, as XInput covers the same pads and the two together can drop one on the wireless adapter.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

void HandleEvent(const void* sdl_event) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(sdl_event);
    if (e.type != SDL_EVENT_GAMEPAD_ADDED && e.type != SDL_EVENT_GAMEPAD_REMOVED) return;
    std::lock_guard lock(g_lock);
    if (!g_sdl) return;
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else Remove(Kind::Controller, e.gdevice.which);
}

thread_local bool t_presence = false;

Pad Raw(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = PadOf(port)) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    return p;
}

Pad Poll(int port) {
    Pad p = Raw(port);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (SDL_Gamepad* pad = PadOf(port)) SDL_RumbleGamepad(pad, left, right, 0);
}

std::vector<Device> Devices(Kind kind) {
    std::vector<Device> v;
    std::lock_guard lock(g_lock);
    for (const Dev& d : g_devices) {
        if (d.kind != kind) continue;
        const std::string n = d.key.substr(d.key.rfind('#') + 1);
        v.push_back({d.kind, d.id, d.name, d.key, n == "1" ? d.name : d.name + " (" + n + ")"});
    }
    return v;
}

std::string Assigned(int port, Kind kind) {
    if (port < 0 || port >= kPorts) return "";
    std::lock_guard lock(g_lock);
    return Slot(port, kind);
}

void Assign(int port, Kind kind, const std::string& key) {
    if (port < 0 || port >= kPorts || (kind == Kind::Controller && key == kAll)) return;
    std::lock_guard lock(g_lock);
    if (Slot(port, kind) == key) return;
    for (int p = 0; p < kPorts; p++)
        if (p != port && !key.empty() && Slot(p, kind) == key) {
            Slot(p, kind).clear();
            LOGI("input: player %d's %s %s goes to player %d", p + 1, KindName(kind), key.c_str(), port + 1);
        }
    Slot(port, kind) = key;
}

int Holder(Kind kind, const std::string& key) {
    if (key.empty()) return -1;
    std::lock_guard lock(g_lock);
    for (int p = 0; p < kPorts; p++)
        if (Slot(p, kind) == key) return p;
    return -1;
}

int Owner(Kind kind, uint64_t id) {
    std::lock_guard lock(g_lock);
    const Dev* d = Find(kind, id);
    return d ? OwnerOf(*d) : -1;
}

bool Has(int port, Kind kind) {
    std::lock_guard lock(g_lock);
    return HasLocked(port, kind);
}

bool KeyDown(int port, int vk) {
    if (vk <= 0 || vk > 0xFF) return false;
    // the generic Shift, Ctrl and Alt are either side's
    int codes[2] = {vk, 0};
    if (vk == 0x10) codes[0] = 0xA0, codes[1] = 0xA1;
    if (vk == 0x11) codes[0] = 0xA2, codes[1] = 0xA3;
    if (vk == 0x12) codes[0] = 0xA4, codes[1] = 0xA5;
    const Kind kind = vk < 7 ? Kind::Mouse : Kind::Keyboard;
    std::lock_guard lock(g_lock);
    // a button the system holds up is up: the click that brings the window back sends its press but not always its
    // release, and the button stayed held until clicked again
    if (kind == Kind::Mouse && !app::KeyDown(vk)) {
        for (Dev& d : g_devices)
            if (d.kind == Kind::Mouse) d.down[size_t(vk)] = false;
        return false;
    }
    for (const Dev& d : g_devices)
        if (d.kind == kind && (d.down[size_t(codes[0])] || (codes[1] && d.down[size_t(codes[1])])) && OwnerOf(d) == port)
            return true;
    return false;
}

bool AutoAssign() { return g_auto.load(); }
void SetAutoAssign(bool on) { g_auto.store(on); }

void AddDevice(Kind kind, uint64_t id, const std::string& name) {
    if (kind == Kind::Controller) return;
    std::lock_guard lock(g_lock);
    Add(kind, id, name);
}

void RemoveDevice(Kind kind, uint64_t id) {
    if (kind == Kind::Controller) return;
    std::lock_guard lock(g_lock);
    Remove(kind, id);
}

void Key(uint64_t keyboard, int vk, bool down) {
    if (vk <= 0 || vk > 0xFF) return;
    std::lock_guard lock(g_lock);
    Add(Kind::Keyboard, keyboard, "").down[size_t(vk)] = down;
}

void Button(uint64_t mouse, int vk, bool down) {
    if (vk <= 0 || vk >= 7) return;
    std::lock_guard lock(g_lock);
    Add(Kind::Mouse, mouse, "").down[size_t(vk)] = down;
}

void ReleaseAll() {
    std::lock_guard lock(g_lock);
    for (Dev& d : g_devices) d.down.reset();
}

}  // namespace input
