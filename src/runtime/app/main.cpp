// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "xdk/dsound.h"

void x86_profile_dump();  // cpu/dispatch.cpp
void x86_profile_mark();

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

std::atomic<bool> g_background{false};  // Background(): set by the window's focus messages

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2;
bool g_mode_changed = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running

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
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (g_mode_changed) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        g_mode_changed = false;
    }
    Mode out = Output();
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;  // a window held back for a splash or handover stays so
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop);
    // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        g_mode_changed = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
        GetMonitorInfoW(mon, &mi);
    }
    if (s.display_mode == DisplayMode::Windowed) {
        RECT r{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& a = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h,
                     SWP_FRAMECHANGED);
    } else {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    }
}

// Raw input, foreground only (no RIDEV_INPUTSINK): every keyboard and mouse apart, for the players' devices (input.h).
// The usual key and mouse messages still come, for ImGui and the menu.
std::string Utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(std::strlen(s.c_str()));
    return s;
}

// the device's product name as it reports it, else its vendor and product ids, else what it is
std::string RawName(HANDLE device, input::Kind kind) {
    const char* what = kind == input::Kind::Keyboard ? "Keyboard" : "Mouse";
    UINT n = 0;
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &n);
    if (!n) return what;
    std::wstring path(n, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &n) == UINT(-1)) return what;
    path.resize(std::wcslen(path.c_str()));
    using ProductString = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
    static const ProductString product = [] {
        HMODULE hid = LoadLibraryW(L"hid.dll");
        return hid ? reinterpret_cast<ProductString>(reinterpret_cast<void*>(GetProcAddress(hid, "HidD_GetProductString")))
                   : nullptr;
    }();
    HANDLE f = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        wchar_t name[127] = {};
        const bool ok = product && product(f, name, sizeof(name)) && name[0];
        CloseHandle(f);
        if (ok) return Utf8(name);
    }
    for (wchar_t& c : path) c = wchar_t(std::towupper(c));
    size_t vid = path.find(L"VID_"), pid = path.find(L"PID_");
    if (vid == std::wstring::npos || pid == std::wstring::npos) return what;
    return std::string(what) + " (" + Utf8(path.substr(vid + 4, 4).c_str()) + ":" + Utf8(path.substr(pid + 4, 4).c_str()) + ")";
}

void RawDevice(HANDLE device, bool arrived) {
    const uint64_t id = uint64_t(uintptr_t(device));
    if (!arrived) {
        input::RemoveDevice(input::Kind::Keyboard, id);
        input::RemoveDevice(input::Kind::Mouse, id);
        return;
    }
    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT size = sizeof(info);
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) == UINT(-1)) return;
    if (info.dwType != RIM_TYPEKEYBOARD && info.dwType != RIM_TYPEMOUSE) return;
    const input::Kind kind = info.dwType == RIM_TYPEKEYBOARD ? input::Kind::Keyboard : input::Kind::Mouse;
    input::AddDevice(kind, id, RawName(device, kind));
}

void RegisterRawInput(HWND hwnd) {
    // generic desktop keyboards and mice, told of arrivals and removals
    const RAWINPUTDEVICE rid[2] = {{0x01, 0x06, RIDEV_DEVNOTIFY, hwnd}, {0x01, 0x02, RIDEV_DEVNOTIFY, hwnd}};
    if (!RegisterRawInputDevices(rid, 2, sizeof(rid[0]))) LOGE("input: raw input registration failed (%lu)", GetLastError());
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1) || !count) return;
    std::vector<RAWINPUTDEVICELIST> list(count);
    count = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < count && count != UINT(-1); i++) RawDevice(list[i].hDevice, true);
}

void RawInput(HWND hwnd, HRAWINPUT handle) {
    RAWINPUT raw;
    UINT size = sizeof(raw);
    if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    // input sent by a program (an on-screen keyboard, a remote desktop) has no device: it is one of its own, 0
    const uint64_t id = uint64_t(uintptr_t(raw.header.hDevice));
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = raw.data.keyboard;
        int vk = k.VKey;
        if (vk == VK_SHIFT) vk = k.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        else if (vk == VK_CONTROL) vk = (k.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = (k.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        if (vk > 0 && vk < 0xFF) input::Key(id, vk, !(k.Flags & RI_KEY_BREAK));
        return;
    }
    if (raw.header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& m = raw.data.mouse;
    // down and up flags, a pair a button: left, right, middle, then the side ones
    constexpr int kVk[5] = {1, 2, 4, 5, 6};
    for (int b = 0; b < 5; b++) {
        if (m.usButtonFlags & (1u << (2 * b))) input::Button(id, kVk[b], true);
        if (m.usButtonFlags & (2u << (2 * b))) input::Button(id, kVk[b], false);
    }
    // absolute motion is a tablet or a remote desktop, not a mouse to look with
    if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) || (!m.lLastX && !m.lLastY) || !kGame.message) return;
    intptr_t result = 0;
    kGame.message(hwnd, kMsgMouseMotion, MotionWp(int32_t(m.lLastX), int32_t(m.lLastY)), intptr_t(id), &result);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && !(lp & (1 << 30))) {
                ToggleWindowed();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp == VK_F11 && !(lp & (1 << 30))) ToggleWindowed();
            if (wp == VK_F3) DumpThreads();
            if (wp == VK_F4 && !(lp & (1 << 30))) gpu::g_record_dump = true;
            if (wp == VK_F5 && !(lp & (1 << 30))) x86_profile_dump();  // not on auto repeat
            if (wp == VK_F6 && !(lp & (1 << 30))) x86_profile_mark();
            return 0;
        case WM_INPUT:
            RawInput(hwnd, reinterpret_cast<HRAWINPUT>(lp));
            break;  // DefWindowProc cleans up after it
        case WM_INPUT_DEVICE_CHANGE:
            RawDevice(reinterpret_cast<HANDLE>(lp), wp == GIDC_ARRIVAL);
            return 0;
        case WM_ACTIVATE:
            g_background = LOWORD(wp) == WA_INACTIVE;
            if (LOWORD(wp) == WA_INACTIVE) {
                input::ReleaseAll();  // the keys let go while another window has them never come
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
                    g_mode_changed = false;
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (Get().display_mode == DisplayMode::Fullscreen && Get().width && Get().height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        case kApplyDisplay:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ApplyNow();
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::filesystem::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

int PortArg() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--port");
        return p ? std::clamp(_wtoi(p + 6), 0, 65535) : 0;
    }();
    return n;
}

const std::string& JoinArg() {
    static const std::string s = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--join");
        std::string out;
        if (!p) return out;
        for (p += 6; *p == L' ' || *p == L'='; p++) {}
        for (; *p && *p != L' ' && *p != L'"'; p++) out += char(*p);
        return out;
    }();
    return s;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool Cursor(float& x, float& y, float& w, float& h) {
    POINT p;
    RECT r;
    if (!g_hwnd || !GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r)) return false;
    x = float(p.x), y = float(p.y), w = float(r.right), h = float(r.bottom);
    return p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
}
void Post(uint32_t msg) {
    if (g_hwnd) PostMessageW(g_hwnd, msg, 0, 0);
}
void Quit() { Post(WM_CLOSE); }
void SetTitle(const std::wstring& title) { SetWindowTextW(g_hwnd, title.c_str()); }
bool Focused() { return g_hwnd && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd); }
bool Background() { return g_background.load(std::memory_order_relaxed); }
void HoldMouse(bool) {}  // the game's own mouse code holds it on Windows

bool Alert(const std::wstring& text, bool ask) {
    UINT type = ask ? MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2 : MB_ICONERROR;
    return MessageBoxW(g_hwnd, text.c_str(), kGame.short_title, type) == IDYES;
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

std::vector<Mode> DisplayModes() {
    std::vector<Mode> v;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        if (!dm.dmPelsWidth || !dm.dmPelsHeight) continue;
        Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (std::find_if(v.begin(), v.end(), [&](const Mode& e) { return e.width == m.width && e.height == m.height; }) ==
            v.end())
            v.push_back(m);
    }
    std::sort(v.begin(), v.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    return v;
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
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    app::InstallCrashHandler();
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::StateDir() / "logs").string(), log, relaunched);
    app::MoveOldData();
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, 0, 0, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, SW_SHOW);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    app::UiFont();
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::Init();
    app::RegisterRawInput(app::g_hwnd);
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::wstring hdd = app::Instance() > 1 ? L"hdd" + std::to_wstring(app::Instance()) : L"hdd";
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::DataDir() / hdd).string())) {
        app::Alert(L"The game failed to start, see the log: " + std::filesystem::path(logx::Path()).wstring());
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app::g_mode_changed) ChangeDisplaySettingsW(nullptr, 0);
    logx::Flush();
    kernel::Shutdown(0);
}
