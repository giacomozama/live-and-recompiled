// The host on SDL3, on every platform: window, events, display modes, the Vulkan surface; the game is described by
// app::kGame. The window thread is the main thread, which SDL's video wants. Its events reach GameInfo::message as a
// Win32 window's would (app.h). Windows adds its boot splash and relaunch handover (splash.cpp, handover.cpp), a
// console when started from one, and raw keyboard input, so each keyboard is told apart as on Linux.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#else
#include <unistd.h>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/shader_gen.h"
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
int g_port = 0;
std::string g_join;
uint32_t g_user = 0;  // the event type Post, ApplyDisplay and OpenGameWindow send to the window thread
enum : int32_t { kUserPost, kUserApply, kUserOpen, kUserText };
std::atomic<uint64_t> g_desktop{0};  // width << 32 | height, read on any thread
std::atomic<uint32_t> g_refresh{60};  // the window's display's refresh rate in Hz, read on any thread
std::atomic<double> g_refresh_hz{60};  // the same, unrounded

// virtual-key codes and the keys SDL names by position; the generic Shift, Ctrl and Alt take either side
constexpr int kVkShift = 0x10, kVkCtrl = 0x11, kVkAlt = 0x12;
constexpr int kVkF3 = 0x72, kVkF4 = 0x73, kVkF5 = 0x74, kVkF6 = 0x75, kVkF7 = 0x76, kVkF8 = 0x77, kVkF11 = 0x7A;

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
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d))
        if (m->refresh_rate >= 24.0f) {
            g_refresh = uint32_t(std::lround(m->refresh_rate));
            g_refresh_hz = m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0
                               ? double(m->refresh_rate_numerator) / double(m->refresh_rate_denominator)
                               : double(m->refresh_rate);
        }
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
            if constexpr (diag::kDebugFeatures) {
                if (vk == kVkF3) DumpThreads();
                if (vk == kVkF4 && !e.key.repeat) gpu::g_record_dump = true;
                if (vk == kVkF5 && !e.key.repeat) x86_profile_dump();
                if (vk == kVkF6 && !e.key.repeat) x86_profile_mark();
                if (vk == kVkF7 && !e.key.repeat) gpu::ToggleFreeze();
                if (vk == kVkF8) gpu::StepFrozen();  // on auto repeat too: frame by frame while held
            }
            break;
        }
        case SDL_EVENT_TEXT_INPUT:  // UTF-8, a character at a time to the game
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(e.text.text); p && *p;) {
                uint32_t c = *p++;
                int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
                for (; more && (*p & 0xC0) == 0x80; more--) c = c << 6 | (*p++ & 0x3F);
                if (!more) Send(kMsgChar, c);
            }
            break;
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
        case SDL_EVENT_MOUSE_WHEEL: {
            // in notches, which may come as fractions: the rest is kept for the next
            static float rest = 0;
            rest += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            const int turn = int(rest * 120);
            rest -= float(turn) / 120;
            if (turn) Send(kMsgMouseWheel, uintptr_t(uint16_t(int16_t(std::clamp(turn, -32768, 32767)))) << 16);
            break;
        }
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
            } else if (e.user.code == kUserText) {
                if (e.user.data1) SDL_StartTextInput(g_window);
                else SDL_StopTextInput(g_window);
            } else if (e.user.code == kUserOpen) {
                CloseSplash();
                if (e.user.data1) ApplyNow();
                SDL_ShowWindow(g_window);
                SDL_RaiseWindow(g_window);
            }
            break;
    }
    return true;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Two Wayland protocols SDL doesn't speak. content-type-v1 (wp_content_type_v1) marks the window's surface as a game's,
// which compositors read for their own choices: Hyprland's VRR on fullscreen video and games (misc:vrr 3) and direct
// scanout for games alone (render:direct_scanout 2). color-management-v1 (wp_color_manager_v1) tells the HDR levels
// the compositor would have the surface drawn for, its preferred image description: the SDR white it maps (the
// reference luminance) and the display's peak (the target luminance), the defaults of paper white and peak.
// libwayland-client is SDL's own, loaded at run time, so its few calls come by name and the protocols' interfaces are
// written out here (wl_interface's layout is libwayland's stable ABI). On a private queue, so SDL's own events aren't
// dispatched from here
namespace wl {
struct Interface;
struct Message {
    const char* name;
    const char* signature;
    const Interface* const* types;
};
struct Interface {
    const char* name;
    int version;
    int method_count;
    const Message* methods;
    int event_count;
    const Message* events;
};
struct RegistryListener {
    void (*global)(void* data, void* registry, uint32_t name, const char* interface, uint32_t version);
    void (*global_remove)(void* data, void* registry, uint32_t name);
};
extern const Interface kContentType, kFeedback, kDesc, kInfo;
const Interface* const kNone[8] = {};
const Interface* const kGetType[] = {&kContentType, nullptr};
const Message kManagerRequests[] = {{"destroy", "", kNone}, {"get_surface_content_type", "no", kGetType}};
const Interface kManager = {"wp_content_type_manager_v1", 1, 2, kManagerRequests, 0, nullptr};
const Message kTypeRequests[] = {{"destroy", "", kNone}, {"set_content_type", "u", kNone}};
const Interface kContentType = {"wp_content_type_v1", 1, 2, kTypeRequests, 0, nullptr};
// color-management-v1 at version 1: the requests as far as get_surface_feedback, every event
const Interface* const kGetFeedback[] = {&kFeedback, nullptr};
const Message kColorRequests[] = {{"destroy", "", kNone},
                                  {"get_output", "no", kNone},
                                  {"get_surface", "no", kNone},
                                  {"get_surface_feedback", "no", kGetFeedback}};
const Message kColorEvents[] = {{"supported_intent", "u", kNone},
                                {"supported_feature", "u", kNone},
                                {"supported_tf_named", "u", kNone},
                                {"supported_primaries_named", "u", kNone},
                                {"done", "", kNone}};
const Interface kColor = {"wp_color_manager_v1", 1, 4, kColorRequests, 5, kColorEvents};
const Interface* const kGetDesc[] = {&kDesc};
const Message kFeedbackRequests[] = {{"destroy", "", kNone}, {"get_preferred", "n", kGetDesc}};
const Message kFeedbackEvents[] = {{"preferred_changed", "u", kNone}, {"preferred_changed2", "2uu", kNone}};
const Interface kFeedback = {"wp_color_management_surface_feedback_v1", 1, 2, kFeedbackRequests, 2, kFeedbackEvents};
const Interface* const kGetInfo[] = {&kInfo};
const Message kDescRequests[] = {{"destroy", "", kNone}, {"get_information", "n", kGetInfo}};
const Message kDescEvents[] = {{"failed", "us", kNone}, {"ready", "u", kNone}, {"ready2", "2uu", kNone}};
const Interface kDesc = {"wp_image_description_v1", 1, 2, kDescRequests, 3, kDescEvents};
const Message kInfoEvents[] = {{"done", "", kNone},
                               {"icc_file", "hu", kNone},
                               {"primaries", "iiiiiiii", kNone},
                               {"primaries_named", "u", kNone},
                               {"tf_power", "u", kNone},
                               {"tf_named", "u", kNone},
                               {"luminances", "uuu", kNone},
                               {"target_primaries", "iiiiiiii", kNone},
                               {"target_luminance", "uu", kNone},
                               {"target_max_cll", "u", kNone},
                               {"target_max_fall", "u", kNone}};
const Interface kInfo = {"wp_image_description_info_v1", 1, 0, nullptr, 11, kInfoEvents};
constexpr uint32_t kDisplayGetRegistry = 1, kRegistryBind = 0, kManagerDestroy = 0, kManagerGetType = 1,
                   kTypeSet = 1, kTypeGame = 3, kMarshalDestroy = 1, kColorDestroy = 0, kColorGetFeedback = 3,
                   kFeedbackGetPreferred = 1, kDescDestroy = 0, kDescGetInformation = 1, kTfPq = 11;

// what the image description's events tell
struct Desc {
    bool ready = false, failed = false, done = false;
    uint32_t tf = 0, reference = 0, target_max = 0;
};
using Fn = void (*)();
template <class... A>
Fn F(void (*f)(void*, void*, A...)) {
    return reinterpret_cast<Fn>(f);
}
const Fn kDescListener[] = {
    F(+[](void* d, void*, uint32_t, const char*) { static_cast<Desc*>(d)->failed = true; }),
    F(+[](void* d, void*, uint32_t) { static_cast<Desc*>(d)->ready = true; }),
    F(+[](void* d, void*, uint32_t, uint32_t) { static_cast<Desc*>(d)->ready = true; })};
using I = int32_t;
using U = uint32_t;
const Fn kInfoListener[] = {
    F(+[](void* d, void*) { static_cast<Desc*>(d)->done = true; }),
    F(+[](void*, void*, I fd, U) { close(fd); }),
    F(+[](void*, void*, I, I, I, I, I, I, I, I) {}),
    F(+[](void*, void*, U) {}),
    F(+[](void*, void*, U) {}),
    F(+[](void* d, void*, U tf) { static_cast<Desc*>(d)->tf = tf; }),
    F(+[](void* d, void*, U, U, U reference) { static_cast<Desc*>(d)->reference = reference; }),
    F(+[](void*, void*, I, I, I, I, I, I, I, I) {}),
    F(+[](void* d, void*, U, U max) { static_cast<Desc*>(d)->target_max = max; }),
    F(+[](void*, void*, U) {}),
    F(+[](void*, void*, U) {})};

// the calls, the display and the surface's color feedback, kept for ReadDisplayHdr
struct Client {
    void* (*marshal)(void*, uint32_t, const Interface*, uint32_t, uint32_t, ...) = nullptr;
    int (*add_listener)(void*, Fn*, void*) = nullptr;
    uint32_t (*version)(void*) = nullptr;
    void (*destroy)(void*) = nullptr;
    int (*roundtrip)(void*, void*) = nullptr;
    void* display = nullptr;
    void* queue = nullptr;     // the feedback's, and the descriptions asked through it
    void* feedback = nullptr;  // none without color-management-v1
    std::mutex lock;
};
Client g_client;
}  // namespace wl

void TagGameContent() {
    if (std::strcmp(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "", "wayland")) return;
    SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
    void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    void* surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    void* lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!display || !surface || !lib) return;
    auto sym = [&](const char* name) { return dlsym(lib, name); };
    auto marshal = reinterpret_cast<void* (*)(void*, uint32_t, const wl::Interface*, uint32_t, uint32_t, ...)>(
        sym("wl_proxy_marshal_flags"));  // libwayland 1.20 on
    auto add_listener = reinterpret_cast<int (*)(void*, wl::Fn*, void*)>(sym("wl_proxy_add_listener"));
    auto version = reinterpret_cast<uint32_t (*)(void*)>(sym("wl_proxy_get_version"));
    auto destroy = reinterpret_cast<void (*)(void*)>(sym("wl_proxy_destroy"));
    auto set_queue = reinterpret_cast<void (*)(void*, void*)>(sym("wl_proxy_set_queue"));
    auto create_wrapper = reinterpret_cast<void* (*)(void*)>(sym("wl_proxy_create_wrapper"));
    auto wrapper_destroy = reinterpret_cast<void (*)(void*)>(sym("wl_proxy_wrapper_destroy"));
    auto create_queue = reinterpret_cast<void* (*)(void*)>(sym("wl_display_create_queue"));
    auto queue_destroy = reinterpret_cast<void (*)(void*)>(sym("wl_event_queue_destroy"));
    auto roundtrip = reinterpret_cast<int (*)(void*, void*)>(sym("wl_display_roundtrip_queue"));
    auto flush = reinterpret_cast<int (*)(void*)>(sym("wl_display_flush"));
    auto registry_iface = static_cast<const wl::Interface*>(sym("wl_registry_interface"));
    if (!marshal || !add_listener || !version || !destroy || !set_queue || !create_wrapper || !wrapper_destroy ||
        !create_queue || !queue_destroy || !roundtrip || !flush || !registry_iface) {
        LOGI("Wayland: content type not set, libwayland-client lacks a call");
        dlclose(lib);
        return;
    }
    struct Found {
        decltype(marshal) bind;
        void* manager = nullptr;
        void* color = nullptr;
    } found{marshal};
    static const wl::RegistryListener listener = {
        [](void* data, void* registry, uint32_t name, const char* interface, uint32_t) {
            auto* f = static_cast<Found*>(data);
            for (auto [slot, iface] : {std::pair{&f->manager, &wl::kManager}, std::pair{&f->color, &wl::kColor}}) {
                if (*slot || std::strcmp(interface, iface->name)) continue;
                *slot = f->bind(registry, wl::kRegistryBind, iface, 1, 0, name, iface->name, 1u, nullptr);
            }
        },
        [](void*, void*, uint32_t) {}};
    void* queue = create_queue(display);
    void* wrapped = create_wrapper(display);
    set_queue(wrapped, queue);
    void* registry = marshal(wrapped, wl::kDisplayGetRegistry, registry_iface, version(wrapped), 0, nullptr);
    add_listener(registry, reinterpret_cast<wl::Fn*>(const_cast<wl::RegistryListener*>(&listener)), &found);
    roundtrip(display, queue);
    if (found.manager) {
        // kept for the window's life: the object's destruction would take the content type back to none
        void* type = marshal(found.manager, wl::kManagerGetType, &wl::kContentType, version(found.manager), 0,
                             nullptr, surface);
        marshal(type, wl::kTypeSet, nullptr, version(type), 0, wl::kTypeGame);
        set_queue(type, nullptr);
        marshal(found.manager, wl::kManagerDestroy, nullptr, version(found.manager), wl::kMarshalDestroy);
        flush(display);
        LOGR("Wayland: window marked as game content");
    } else {
        LOGI("Wayland: the compositor has no content-type-v1");
    }
    destroy(registry);
    wrapper_destroy(wrapped);
    if (found.color) {
        // the feedback kept on the private queue for ReadDisplayHdr, its preferred_changed events dropped there
        wl::Client& c = wl::g_client;
        c.marshal = marshal, c.add_listener = add_listener, c.version = version, c.destroy = destroy;
        c.roundtrip = roundtrip, c.display = display, c.queue = queue;
        c.feedback = marshal(found.color, wl::kColorGetFeedback, &wl::kFeedback, version(found.color), 0, nullptr,
                             surface);
        marshal(found.color, wl::kColorDestroy, nullptr, version(found.color), wl::kMarshalDestroy);
        ReadDisplayHdr();
    } else {
        LOGI("Wayland: the compositor has no color-management-v1");
        queue_destroy(queue);
    }
    // the library stays loaded: SDL holds it too, and the content type object lives on
}
#else
void TagGameContent() {}
#endif

}  // namespace

#if defined(__linux__) && !defined(__ANDROID__)
// the surface's preferred image description asked for again: its levels, when it is an HDR one (PQ), the display's
void ReadDisplayHdr() {
    wl::Client& c = wl::g_client;
    std::lock_guard lock(c.lock);
    if (!c.feedback) return;
    wl::Desc d;
    void* desc = c.marshal(c.feedback, wl::kFeedbackGetPreferred, &wl::kDesc, c.version(c.feedback), 0, nullptr);
    c.add_listener(desc, const_cast<wl::Fn*>(wl::kDescListener), &d);
    for (int i = 0; i < 4 && !d.ready && !d.failed; i++)
        if (c.roundtrip(c.display, c.queue) < 0) break;
    if (d.ready) {
        void* info = c.marshal(desc, wl::kDescGetInformation, &wl::kInfo, c.version(desc), 0, nullptr);
        c.add_listener(info, const_cast<wl::Fn*>(wl::kInfoListener), &d);
        for (int i = 0; i < 4 && !d.done; i++)
            if (c.roundtrip(c.display, c.queue) < 0) break;
        c.destroy(info);  // done is its destructor event: the proxy is the client's to free
    }
    c.marshal(desc, wl::kDescDestroy, nullptr, c.version(desc), wl::kMarshalDestroy);
    const bool hdr = d.done && d.tf == wl::kTfPq;
    const uint32_t paper = hdr ? d.reference : 0, peak = hdr ? d.target_max : 0;
    static bool told = false;
    if (!std::exchange(told, true) || paper != DisplayHdrPaperWhite() || peak != DisplayHdrPeak()) {
        if (hdr) LOGR("Wayland: the compositor's HDR levels: SDR white %u nits, peak %u nits", paper, peak);
        else LOGR("Wayland: the compositor's preferred image description is %s", d.done ? "not HDR" : "not known");
    }
    SetDisplayHdr(paper, peak);
}
#else
void ReadDisplayHdr() {}
#endif

namespace {
// this executable's path, empty when the system won't say
const std::filesystem::path& ExePath() {
    static const std::filesystem::path path = [] {
#ifdef _WIN32
        wchar_t p[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
        return n ? std::filesystem::path(p) : std::filesystem::path();
#else
        char p[PATH_MAX] = {};
        ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
        return n > 0 ? std::filesystem::path(std::string(p, size_t(n))) : std::filesystem::path();
#endif
    }();
    return path;
}
}  // namespace

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir =
        ExePath().empty() ? std::filesystem::current_path() : ExePath().parent_path();
    return dir;
}

int RunSelf(const std::vector<std::string>& args, const std::atomic<bool>* stop) {
    if (ExePath().empty()) return -1;
    const std::u8string exe = ExePath().u8string();
    std::vector<std::string> all{std::string(exe.begin(), exe.end())};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    for (const std::string& a : all) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(argv.data(), false);
    if (!p) {
        LOGW("run: %s", SDL_GetError());
        return -1;
    }
    int code = -1;
    while (!SDL_WaitProcess(p, false, &code)) {
        if (stop && stop->load()) {
            SDL_KillProcess(p, true);
            SDL_WaitProcess(p, true, &code);
            code = -1;
            break;
        }
        SDL_Delay(50);
    }
    SDL_DestroyProcess(p);
    return code;
}

int Instance() { return g_instance; }
int PortArg() { return g_port; }
const std::string& JoinArg() { return g_join; }

void* Window() { return g_window; }
void* NativeWindow() {
#ifdef _WIN32
    if (g_window) return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return nullptr;
}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PushUser(kUserApply, 0); }

void OpenFolder(const std::filesystem::path& dir) {
#ifdef _WIN32
    std::wstring cmd = L"explorer.exe \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    // a file URL with what a path may hold escaped; the desktop opens it in its file manager
    std::string url = "file://";
    for (unsigned char c : dir.string()) {
        if (std::isalnum(c) || std::strchr("/-._~", c)) url += char(c);
        else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
    if (!SDL_OpenURL(url.c_str())) LOGE("open folder: %s", SDL_GetError());
#endif
}
void OpenGameWindow(bool apply) { PushUser(kUserOpen, apply); }

#ifndef _WIN32
// no splash, and a relaunch execs in place: there is no window to hand over (kernel::Relaunch)
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
#endif

void KeepFullscreenOnFocusLoss() { SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0"); }

bool Pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (!Handle(e)) return false;
    return true;
}

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

bool Cursor(float& x, float& y, float& w, float& h) {
    if (!g_window || SDL_GetMouseFocus() != g_window) return false;
    int ww = 0, wh = 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    w = float(ww), h = float(wh);
    return x >= 0 && y >= 0 && x < w && y < h;
}

void Post(uint32_t msg) { PushUser(kUserPost, msg); }

void TextInput(bool on) { PushUser(kUserText, on); }

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

void UiNewFrame() {
    ImGui_ImplSDL3_NewFrame();
    // the backend, opening no pads of its own (Manual), says there are none: ImGui would drop the pad keys the menu
    // feeds it (ui.cpp)
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
}

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

uint32_t MonitorRefresh() { return std::max(g_refresh.load(), 24u); }
double MonitorHz() { return g_refresh.load() >= 24 ? std::max(g_refresh_hz.load(), 24.0) : 24.0; }

void PaceMenu() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    auto period = std::chrono::nanoseconds(1000000000ull / std::max(g_refresh.load(), 24u));
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

// "--name value" or "--name=value" at argv[i]; i steps past the value
const char* ArgValue(int argc, char** argv, int& i, const char* name) {
    const size_t n = std::strlen(name);
    if (std::strncmp(argv[i], name, n)) return nullptr;
    if (argv[i][n] == '=') return argv[i] + n + 1;
    if (argv[i][n] || i + 1 >= argc) return nullptr;
    return argv[++i];
}

#ifdef _WIN32
// a GUI program has no console of its own: started from a shell, print into that shell's (unless output is redirected)
void AttachParentConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    bool need_out = !out || out == INVALID_HANDLE_VALUE, need_err = !err || err == INVALID_HANDLE_VALUE;
    if (!(need_out || need_err) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    if (need_out) std::freopen("CONOUT$", "w", stdout);
    if (need_err) std::freopen("CONOUT$", "w", stderr);
}
#endif

int Run(int argc, char** argv) {
    bool relaunched = false;
    std::string gen_out;  // --make-shader-records PATH (gpu/shader_gen.h), the renderer's state with --gen-*
    gpu::shader_gen::Context gen;
    for (int i = 1; i < argc; i++) {
        if (const char* v = ArgValue(argc, argv, i, "--make-shader-records")) gen_out = v;
        else if (const char* v = ArgValue(argc, argv, i, "--gen-rtfmt")) gen.rtfmt = uint32_t(std::atoi(v));
        else if (const char* v = ArgValue(argc, argv, i, "--gen-samples")) gen.samples = uint32_t(std::max(1, std::atoi(v)));
        else if (!std::strcmp(argv[i], "--gen-tess")) gen.tess = true;
        else if (!std::strcmp(argv[i], "--gen-vrs")) gen.vrs = true;
        else if (const char* v = ArgValue(argc, argv, i, "--instance")) app::g_instance = std::clamp(std::atoi(v), 0, 8);
        else if (const char* v = ArgValue(argc, argv, i, "--port")) app::g_port = std::clamp(std::atoi(v), 0, 65535);
        else if (const char* v = ArgValue(argc, argv, i, "--join")) app::g_join = v;
        else if (!std::strcmp(argv[i], "--relaunched")) relaunched = true;
    }
#ifdef _WIN32
    AttachParentConsole();
    timeBeginPeriod(1);  // Sleep(1) a millisecond, for Pace
    // each keyboard its own device, for the players' keyboards (input.h), as Linux has them; Windows' own messages
    // tell them all as one
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
#endif
    app::InstallCrashHandler();
    // one log a session: a relaunch into another image carries on its predecessor's
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    if (!gen_out.empty()) log += "-shadergen";
    logx::Init((app::StateDir() / "logs").string(), log, relaunched);
    if (gen_out.empty()) app::MoveOldData();
    app::Load();
    if (app::kGame.init) app::kGame.init();
    // the records the disc's static push buffers make, written to gen_out, and no more: no window, no title
    if (!gen_out.empty()) {
        app::GameFiles game;
        const int code = app::FindGame(game) && kernel::LoadTitleImage(std::move(game.xbe), IMAGE_XBE)
                             ? gpu::shader_gen::Run(std::u8string(gen_out.begin(), gen_out.end()), gen)
                             : 1;
        logx::Flush();
        std::_Exit(code);
    }
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
    app::MountMods();

    // the welcome screen (RunWelcome) shows in the game window from the start: no splash in front of it
    const bool welcome = app::FirstRun() && app::kGame.welcome;
    const bool splash = !welcome && app::OpenSplash(), handover = app::StartHandover();

    app::g_user = SDL_RegisterEvents(1);
    std::wstring title = app::kGame.title;
    if (app::Instance()) title += L" [" + std::to_wstring(app::Instance()) + L"]";
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    // the OpenGL renderer's window is an OpenGL one (a window is made for one API): a GL 4.1 core context's
    // framebuffer, double buffered, no depth of its own (gpu/opengl.cpp draws into its own targets)
    if (gpu::RendererName() == "opengl") {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
        app::g_window = SDL_CreateWindow(app::Utf8(title).c_str(), 1280, 720, flags | SDL_WINDOW_OPENGL);
        if (!app::g_window) LOGW("SDL: no OpenGL window (%s), opening one for Vulkan", SDL_GetError());
    }
    if (!app::g_window)
        app::g_window = SDL_CreateWindow(app::Utf8(title).c_str(), 1280, 720, flags | SDL_WINDOW_VULKAN);
#ifdef _WIN32
    // no Vulkan loader on the PC (vulkan-1.dll): the window without it, for the D3D11 renderer (gpu::CreateBackend)
    if (!app::g_window) {
        LOGW("SDL: no Vulkan window (%s), opening one for D3D11", SDL_GetError());
        app::g_window = SDL_CreateWindow(app::Utf8(title).c_str(), 1280, 720, flags);
    }
#endif
    if (!app::g_window) {
        LOGE("SDL: no window: %s", SDL_GetError());
        app::Alert(L"The game window could not open, see the log: " + std::filesystem::path(logx::Path()).wstring());
        return 1;
    }
    app::TagGameContent();
    app::StoreDesktop();
    app::ApplyNow();
    // behind a splash the window stays hidden until the game's first frame, handed over until the new instance's
    // (OpenGameWindow); Windows alone has either
    if (!splash && !handover) SDL_ShowWindow(app::g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    app::UiFont();
    // the renderer's thread starts each frame: the cursor's shape is set on the window thread alone
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_InitForVulkan(app::g_window);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);  // the menu feeds its own (ui.cpp)

    input::Init();
    app::AddInputDevices();
    if (app::kGame.window) app::kGame.window(app::g_window);
    if (!app::RunWelcome()) {
        logx::Flush();
        kernel::Shutdown(0);
    }
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::DataDir() / hdd).string())) {
        app::Alert(L"The game failed to start, see the log: " + std::filesystem::path(logx::Path()).wstring());
        return 1;
    }

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100) && !app::Handle(e)) break;
    }
    logx::Flush();
    kernel::Shutdown(0);
    return 0;
}

}  // namespace

#ifdef _WIN32
// the arguments in UTF-8, as main has them elsewhere
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; wide && i < argc; i++) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(size_t(std::max(n, 1)), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, a.data(), n, nullptr, nullptr);
        a.resize(std::strlen(a.c_str()));
        args.push_back(std::move(a));
    }
    if (wide) LocalFree(wide);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    return Run(int(args.size()), argv.data());
}
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
