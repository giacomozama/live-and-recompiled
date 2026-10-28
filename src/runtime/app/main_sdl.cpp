// The host on SDL3, everywhere but Windows (app/main.cpp): window, events, display modes, the Vulkan surface; the
// game is described by app::kGame. The window thread is the main thread, which SDL's video wants. Its events reach
// GameInfo::message as the Win32 window's would (app.h): the game's hooks are the same on both.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <string>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "xdk/dsound.h"

void x86_profile_dump();  // cpu/dispatch.cpp
void x86_profile_mark();

namespace app {

namespace {

std::atomic<bool> g_background{false};  // Background(): set by the window's focus messages

SDL_Window* g_window = nullptr;
int g_instance = 0;
uint32_t g_user = 0;  // the event type Post, ApplyDisplay and OpenGameWindow send to the window thread
enum : int32_t { kUserPost, kUserApply, kUserOpen };
std::atomic<uint64_t> g_desktop{0};  // width << 32 | height, read on any thread

// virtual-key codes and the keys SDL names by position; the generic Shift, Ctrl and Alt take either side
constexpr int kVkShift = 0x10, kVkCtrl = 0x11, kVkAlt = 0x12;
constexpr int kVkF3 = 0x72, kVkF4 = 0x73, kVkF5 = 0x74, kVkF6 = 0x75, kVkF11 = 0x7A;

const std::array<SDL_Scancode, 256>& ScancodeOf() {
    static const auto table = [] {
        std::array<SDL_Scancode, 256> t{};
        t.fill(SDL_SCANCODE_UNKNOWN);
        for (int i = 0; i < 26; i++) t['A' + i] = SDL_Scancode(SDL_SCANCODE_A + i);
        t['0'] = SDL_SCANCODE_0;
        for (int i = 1; i <= 9; i++) t['0' + i] = SDL_Scancode(SDL_SCANCODE_1 + i - 1);
        for (int i = 0; i < 12; i++) t[0x70 + i] = SDL_Scancode(SDL_SCANCODE_F1 + i);
        t[0x60] = SDL_SCANCODE_KP_0;
        for (int i = 1; i <= 9; i++) t[0x60 + i] = SDL_Scancode(SDL_SCANCODE_KP_1 + i - 1);
        const std::pair<int, SDL_Scancode> fixed[] = {
            {0x08, SDL_SCANCODE_BACKSPACE}, {0x09, SDL_SCANCODE_TAB}, {0x0D, SDL_SCANCODE_RETURN},
            {0x13, SDL_SCANCODE_PAUSE}, {0x14, SDL_SCANCODE_CAPSLOCK}, {0x1B, SDL_SCANCODE_ESCAPE},
            {0x20, SDL_SCANCODE_SPACE}, {0x21, SDL_SCANCODE_PAGEUP}, {0x22, SDL_SCANCODE_PAGEDOWN},
            {0x23, SDL_SCANCODE_END}, {0x24, SDL_SCANCODE_HOME}, {0x25, SDL_SCANCODE_LEFT}, {0x26, SDL_SCANCODE_UP},
            {0x27, SDL_SCANCODE_RIGHT}, {0x28, SDL_SCANCODE_DOWN}, {0x2D, SDL_SCANCODE_INSERT},
            {0x2E, SDL_SCANCODE_DELETE}, {0x6A, SDL_SCANCODE_KP_MULTIPLY}, {0x6B, SDL_SCANCODE_KP_PLUS},
            {0x6D, SDL_SCANCODE_KP_MINUS}, {0x6E, SDL_SCANCODE_KP_PERIOD}, {0x6F, SDL_SCANCODE_KP_DIVIDE},
            {0xA0, SDL_SCANCODE_LSHIFT}, {0xA1, SDL_SCANCODE_RSHIFT}, {0xA2, SDL_SCANCODE_LCTRL},
            {0xA3, SDL_SCANCODE_RCTRL}, {0xA4, SDL_SCANCODE_LALT}, {0xA5, SDL_SCANCODE_RALT},
            {0xBA, SDL_SCANCODE_SEMICOLON}, {0xBB, SDL_SCANCODE_EQUALS}, {0xBC, SDL_SCANCODE_COMMA},
            {0xBD, SDL_SCANCODE_MINUS}, {0xBE, SDL_SCANCODE_PERIOD}, {0xBF, SDL_SCANCODE_SLASH},
            {0xC0, SDL_SCANCODE_GRAVE}, {0xDB, SDL_SCANCODE_LEFTBRACKET}, {0xDC, SDL_SCANCODE_BACKSLASH},
            {0xDD, SDL_SCANCODE_RIGHTBRACKET}, {0xDE, SDL_SCANCODE_APOSTROPHE},
        };
        for (const auto& [vk, sc] : fixed) t[size_t(vk)] = sc;
        return t;
    }();
    return table;
}

// the virtual-key code a mouse button stands for: 1 left, 2 right, 4 middle, 5 and 6 the side ones; 0 for the rest
int VkOfButton(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return 1;
        case SDL_BUTTON_RIGHT: return 2;
        case SDL_BUTTON_MIDDLE: return 4;
        case SDL_BUTTON_X1: return 5;
        case SDL_BUTTON_X2: return 6;
        default: return 0;
    }
}

// the players' keyboards and mice (input.h): SDL names each, the Wayland seat's as one
void AddKeyboard(SDL_KeyboardID id) {
    const char* name = SDL_GetKeyboardNameForID(id);
    input::AddDevice(input::Kind::Keyboard, id, name ? name : "");
}
void AddMouse(SDL_MouseID id) {
    const char* name = SDL_GetMouseNameForID(id);
    input::AddDevice(input::Kind::Mouse, id, name ? name : "");
}
void AddInputDevices() {
    int n = 0;
    if (SDL_KeyboardID* ids = SDL_GetKeyboards(&n)) {
        for (int i = 0; i < n; i++) AddKeyboard(ids[i]);
        SDL_free(ids);
    }
    if (SDL_MouseID* ids = SDL_GetMice(&n)) {
        for (int i = 0; i < n; i++) AddMouse(ids[i]);
        SDL_free(ids);
    }
}

// the virtual-key code a key event names, 0 for a key without one
int VkOf(SDL_Scancode sc) {
    static const auto table = [] {
        std::array<uint8_t, SDL_SCANCODE_COUNT> t{};
        const auto& to = ScancodeOf();
        for (int vk = 1; vk < 256; vk++)
            if (to[size_t(vk)] != SDL_SCANCODE_UNKNOWN) t[to[size_t(vk)]] = uint8_t(vk);
        return t;
    }();
    return sc > 0 && sc < SDL_SCANCODE_COUNT ? table[sc] : 0;
}

std::string Utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t wc : w) {
        uint32_t c = uint32_t(wc);
        if (c < 0x80) {
            s += char(c);
        } else if (c < 0x800) {
            s += char(0xC0 | c >> 6);
            s += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += char(0xE0 | c >> 12);
            s += char(0x80 | (c >> 6 & 0x3F));
            s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | c >> 18);
            s += char(0x80 | (c >> 12 & 0x3F));
            s += char(0x80 | (c >> 6 & 0x3F));
            s += char(0x80 | (c & 0x3F));
        }
    }
    return s;
}

void StoreDesktop() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(d))
        g_desktop = uint64_t(uint32_t(m->w)) << 32 | uint32_t(m->h);
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGE("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
    kernel::LogPendingIo();
    dsound::LogState();
}

// window thread only
void ApplyNow() {
    const Settings& s = Get();
    Mode out = Output();
    if (s.display_mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowSize(g_window, int(out.width), int(out.height));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        return;
    }
    // fullscreen at the mode the settings name, the desktop's own one when that is it; borderless keeps the desktop's
    SDL_DisplayMode mode{};
    Mode desk = Desktop();
    bool change = s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
                  (s.width != desk.width || s.height != desk.height) &&
                  SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(g_window), int(s.width), int(s.height), 0,
                                                      true, &mode);
    SDL_SetWindowFullscreenMode(g_window, change ? &mode : nullptr);
    SDL_SetWindowFullscreen(g_window, true);
}

// to GameInfo::message; true: the game took it
bool Send(uint32_t msg, uintptr_t wp = 0, intptr_t lp = 0) {
    intptr_t result = 0;
    return kGame.message && kGame.message(g_window, msg, wp, lp, &result);
}

void PushUser(int32_t code, uintptr_t data) {
    if (!g_user) return;
    SDL_Event e{};
    e.type = g_user;
    e.user.code = code;
    e.user.data1 = reinterpret_cast<void*>(data);
    SDL_PushEvent(&e);
}

// false once the window is to close
bool Handle(const SDL_Event& e) {
    {
        std::lock_guard lock(UiLock());
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_KEY_UP:
            input::Key(e.key.which, VkOf(e.key.scancode), false);
            break;
        case SDL_EVENT_KEY_DOWN: {
            int vk = VkOf(e.key.scancode);
            input::Key(e.key.which, vk, true);
            if (!vk || Send(kMsgKeyDown, uintptr_t(vk), e.key.repeat ? 1 << 30 : 0)) break;
            if (!e.key.repeat && (vk == kVkF11 || (vk == 0x0D && (e.key.mod & SDL_KMOD_ALT)))) ToggleWindowed();
            if (vk == kVkF3) DumpThreads();
            if (vk == kVkF4 && !e.key.repeat) gpu::g_record_dump = true;
            if (vk == kVkF5 && !e.key.repeat) x86_profile_dump();
            if (vk == kVkF6 && !e.key.repeat) x86_profile_mark();
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            g_background = false;
            Send(kMsgActivate, 1);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            input::ReleaseAll();  // SDL lets the keys go as no device's
            g_background = true;
            Send(kMsgActivate, 0);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.which != SDL_PEN_MOUSEID)
                input::Button(e.button.which, VkOfButton(e.button.button), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_KEYBOARD_ADDED:
            AddKeyboard(e.kdevice.which);
            break;
        case SDL_EVENT_KEYBOARD_REMOVED:
            input::RemoveDevice(input::Kind::Keyboard, e.kdevice.which);
            break;
        case SDL_EVENT_MOUSE_ADDED:
            AddMouse(e.mdevice.which);
            break;
        case SDL_EVENT_MOUSE_REMOVED:
            input::RemoveDevice(input::Kind::Mouse, e.mdevice.which);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            Send(kMsgSize);
            break;
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            StoreDesktop();
            Send(kMsgMove);
            break;
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            StoreDesktop();
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            // relative motion in counts, which may come as fractions: each mouse's rest is kept for its next event
            static std::vector<std::pair<SDL_MouseID, std::array<float, 2>>> rests;
            if (!SDL_GetWindowRelativeMouseMode(g_window) || e.motion.which == SDL_TOUCH_MOUSEID ||
                e.motion.which == SDL_PEN_MOUSEID)
                break;
            auto it = std::find_if(rests.begin(), rests.end(), [&](const auto& r) { return r.first == e.motion.which; });
            if (it == rests.end()) it = rests.insert(rests.end(), {e.motion.which, {0.0f, 0.0f}});
            float& fx = it->second[0];
            float& fy = it->second[1];
            fx += e.motion.xrel;
            fy += e.motion.yrel;
            int32_t dx = int32_t(fx), dy = int32_t(fy);
            fx -= float(dx);
            fy -= float(dy);
            if (dx || dy) Send(kMsgMouseMotion, MotionWp(dx, dy), intptr_t(e.motion.which));
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::HandleEvent(&e);
            break;
        default:
            if (e.type != g_user) break;
            if (e.user.code == kUserPost) {
                Send(uint32_t(reinterpret_cast<uintptr_t>(e.user.data1)));
            } else if (e.user.code == kUserApply) {
                ApplyNow();
            } else if (e.user.code == kUserOpen) {
                if (e.user.data1) ApplyNow();
                SDL_ShowWindow(g_window);
                SDL_RaiseWindow(g_window);
            }
            break;
    }
    return true;
}

}  // namespace

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        char p[PATH_MAX] = {};
        ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
        return n > 0 ? std::filesystem::path(std::string(p, size_t(n))).parent_path() : std::filesystem::current_path();
    }();
    return dir;
}

int Instance() { return g_instance; }

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PushUser(kUserApply, 0); }
void OpenGameWindow(bool apply) { PushUser(kUserOpen, apply); }

// no splash, and a relaunch execs in place: there is no window to hand over (kernel::Relaunch)
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

bool KeyDown(int vk) {
    if (vk <= 0 || vk > 0xFF) return false;
    // mouse buttons: 1 left, 2 right, 4 middle, 5 and 6 the side ones
    constexpr int kButton[7] = {0, SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, 0, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
    if (vk < 7) return kButton[vk] && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(kButton[vk]));
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) return false;
    if (vk == kVkShift) return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (vk == kVkCtrl) return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    if (vk == kVkAlt) return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    SDL_Scancode sc = ScancodeOf()[size_t(vk)];
    return sc != SDL_SCANCODE_UNKNOWN && keys[sc];
}

void Post(uint32_t msg) { PushUser(kUserPost, msg); }

void Quit() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

void SetTitle(const std::wstring& title) {
    if (g_window) SDL_SetWindowTitle(g_window, Utf8(title).c_str());
}

bool Background() { return g_background.load(std::memory_order_relaxed); }

bool Focused() {
    if (!g_window) return false;
    SDL_WindowFlags f = SDL_GetWindowFlags(g_window);
    return (f & SDL_WINDOW_INPUT_FOCUS) && !(f & SDL_WINDOW_MINIMIZED);
}

// relative mode: the cursor hidden and held, the motion unaccelerated
void HoldMouse(bool hold) {
    if (g_window) SDL_SetWindowRelativeMouseMode(g_window, hold);
}

bool Alert(const std::wstring& text, bool ask) {
    std::string title = Utf8(kGame.short_title), message = Utf8(text);
    if (!ask) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), message.c_str(), g_window);
        return false;
    }
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
    };
    SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, g_window, title.c_str(), message.c_str(), 2, buttons, nullptr};
    int chosen = 0;
    return SDL_ShowMessageBox(&box, &chosen) && chosen == 1;
}

Mode Desktop() {
    uint64_t d = g_desktop.load();
    if (!d) return {1280, 720};
    return {uint32_t(d >> 32), uint32_t(d)};
}

std::vector<Mode> DisplayModes() {
    std::vector<Mode> v;
    int n = 0;
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(d, &n)) {
        for (int i = 0; i < n; i++) {
            Mode m{uint32_t(modes[i]->w), uint32_t(modes[i]->h)};
            if (std::find_if(v.begin(), v.end(), [&](const Mode& e) {
                    return e.width == m.width && e.height == m.height;
                }) == v.end())
                v.push_back(m);
        }
        SDL_free(modes);
    }
    if (v.empty()) v.push_back(Desktop());
    std::sort(v.begin(), v.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    return v;
}

std::vector<const char*> VulkanExtensions() {
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    return names ? std::vector<const char*>(names, names + n) : std::vector<const char*>{};
}

bool VulkanSurface(void* instance, void* surface) {
    return g_window && SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(instance), nullptr,
                                                static_cast<VkSurfaceKHR*>(surface));
}

void PixelSize(uint32_t& w, uint32_t& h) {
    int pw = 0, ph = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    w = uint32_t(std::max(pw, 0));
    h = uint32_t(std::max(ph, 0));
}

void UiNewFrame() { ImGui_ImplSDL3_NewFrame(); }

void Pace(uint32_t vblanks) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (vblanks > 1) cap = cap ? std::min(cap, 60 / vblanks) : 60 / vblanks;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int main(int argc, char** argv) {
    bool relaunched = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) app::g_instance = std::clamp(std::atoi(argv[++i]), 0, 8);
        else if (!std::strcmp(argv[i], "--relaunched")) relaunched = true;
    }
    app::InstallCrashHandler();
    // one log a session: a relaunch into another image carries on its predecessor's
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    SDL_SetHint(SDL_HINT_APP_NAME, app::Utf8(app::kGame.title).c_str());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("SDL: no video: %s", SDL_GetError());
        app::Alert(L"The game window could not open (" + std::wstring(SDL_GetError(), SDL_GetError() + std::strlen(SDL_GetError())) + L").");
        return 1;
    }
    app::StoreDesktop();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    app::g_user = SDL_RegisterEvents(1);
    std::wstring title = app::kGame.title;
    if (app::Instance()) title += L" [" + std::to_wstring(app::Instance()) + L"]";
    app::g_window = SDL_CreateWindow(app::Utf8(title).c_str(), 1280, 720,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                         SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app::g_window) {
        LOGE("SDL: no window: %s", SDL_GetError());
        app::Alert(L"The game window could not open, see the log in the logs folder.");
        return 1;
    }
    app::StoreDesktop();
    app::ApplyNow();
    SDL_ShowWindow(app::g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    app::UiFont();
    // the renderer's thread starts each frame: the cursor's shape is set on the window thread alone
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_InitForVulkan(app::g_window);

    input::Init();
    app::AddInputDevices();
    if (app::kGame.window) app::kGame.window(app::g_window);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        app::Alert(L"The game failed to start, see the log in the logs folder.");
        return 1;
    }

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100) && !app::Handle(e)) break;
    }
    logx::Flush();
    kernel::Shutdown(0);
}
