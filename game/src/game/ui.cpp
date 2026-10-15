// ImGui settings menu and fps readout, for GameInfo::ui and GameInfo::message (see game_info.cpp).
//
// The render thread draws the frame with app::UiLock held (d3d12.cpp RenderUi), and the window thread runs
// Message for every message after ImGui's. Draw must never make a blocking cross-thread call: it only posts.
// So the fps in the title is posted (kFpsTitle), and the window thread does the SetWindowTextW.
//
// On the headless harness there is no ImGui and no real <windows.h>, so everything but the option globals
// compiles out; keyboard.cpp can still ask Open().
#include "common/ui.h"

#if __has_include(<imgui.h>) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"

namespace ui {

bool g_fps_overlay = false;
bool g_fps_title = true;

namespace {

// the window owns the title; Draw posts this, Message (window thread) writes it
constexpr UINT kFpsTitle = WM_APP + 0x40;

std::atomic<bool> g_open{false};
std::atomic<double> g_fps{0.0};

// the display settings while the menu is editing them; copied back on Apply
app::DisplayMode pending_mode = app::DisplayMode::Borderless;
uint32_t pending_w = 0, pending_h = 0;

// enumerated once, de-duplicated by w x h and sorted largest first
const std::vector<app::Mode>& Modes() {
    static const std::vector<app::Mode> modes = [] {
        std::vector<app::Mode> v;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
            if (!dm.dmPelsWidth || !dm.dmPelsHeight) continue;
            app::Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
            if (std::find_if(v.begin(), v.end(), [&](const app::Mode& e) {
                    return e.width == m.width && e.height == m.height;
                }) == v.end())
                v.push_back(m);
        }
        std::sort(v.begin(), v.end(), [](const app::Mode& a, const app::Mode& b) {
            return a.width != b.width ? a.width > b.width : a.height > b.height;
        });
        return v;
    }();
    return modes;
}

}  // namespace

void Draw(double fps) {
    // the menu's pending display settings follow the live ones each time it opens
    static bool was_open = false;
    static double last_fps = -1.0;
    static bool last_title = !g_fps_title;
    const bool open = g_open.load();
    if (open && !was_open) {
        pending_mode = app::Get().display_mode;
        pending_w = app::Get().width;
        pending_h = app::Get().height;
    }
    was_open = open;

    // not from here: SetWindowTextW from the render thread could deadlock on WndProc's UiLock; post instead
    if (fps != last_fps || g_fps_title != last_title) {
        last_fps = fps;
        last_title = g_fps_title;
        g_fps.store(fps);
        if (HWND w = static_cast<HWND>(app::Window())) PostMessageW(w, kFpsTitle, 0, 0);
    }

    if (g_fps_overlay) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##fps", nullptr, flags))
            ImGui::Text("%.0f FPS (%.2f ms)", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
        ImGui::End();
    }

    if (!open) return;

    app::Settings& s = app::Get();
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings")) {
        ImGui::SeparatorText("Display");
        int mode = int(pending_mode);
        if (ImGui::Combo("Mode", &mode, "Fullscreen\0Borderless\0Windowed\0"))
            pending_mode = app::DisplayMode(mode);

        const std::vector<app::Mode>& modes = Modes();
        std::string res = "Desktop";
        int res_idx = 0;
        if (pending_w && pending_h)
            for (size_t i = 0; i < modes.size(); i++) {
                res += '\0';
                res += std::to_string(modes[i].width) + "x" + std::to_string(modes[i].height);
                if (modes[i].width == pending_w && modes[i].height == pending_h) res_idx = int(i) + 1;
            }
        res += '\0';
        if (ImGui::Combo("Resolution", &res_idx, res.c_str())) {
            if (res_idx == 0) pending_w = pending_h = 0;
            else pending_w = modes[res_idx - 1].width, pending_h = modes[res_idx - 1].height;
        }
        if (ImGui::Button("Apply display")) {
            s.display_mode = pending_mode;
            s.width = pending_w;
            s.height = pending_h;
            app::ApplyDisplay();
        }

        ImGui::SeparatorText("Rendering");
        const uint32_t render_min = 25, render_max = 400;
        ImGui::SliderScalar("Render %", ImGuiDataType_U32, &s.render_percent, &render_min, &render_max, "%u");
        ImGui::Checkbox("V-Sync", &s.vsync);

        constexpr uint32_t kCaps[] = {0, 30, 60, 120, 144, 240};
        int cap = 0;
        for (int i = 0; i < 6; i++)
            if (kCaps[i] == s.fps_cap) cap = i;
        if (ImGui::Combo("FPS cap", &cap, "Unlimited\0 30\0 60\0 120\0 144\0 240\0")) s.fps_cap = kCaps[cap];

        constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
        int aniso = 0;
        for (int i = 0; i < 5; i++)
            if (kAniso[i] == s.aniso) aniso = i;
        if (ImGui::Combo("Anisotropic", &aniso, "Game default\0 2x\0 4x\0 8x\0 16x\0")) s.aniso = kAniso[aniso];

        ImGui::SeparatorText("Aspect");
        const char* aspects[] = {"auto", "4:3", "16:9"};
        int aspect = s.aspect == "4:3" ? 1 : s.aspect == "16:9" ? 2 : 0;
        if (ImGui::Combo("Aspect", &aspect, "auto\0 4:3\0 16:9\0")) s.aspect = aspects[aspect];
        ImGui::SameLine();
        ImGui::TextDisabled("(restart required)");

        ImGui::SeparatorText("FPS");
        ImGui::Checkbox("Overlay", &g_fps_overlay);
        ImGui::SameLine();
        ImGui::Checkbox("Window title", &g_fps_title);

        ImGui::Separator();
        if (ImGui::Button("Save")) app::Save();
        ImGui::SameLine();
        if (ImGui::Button("Close")) g_open.store(false);
        ImGui::TextDisabled("F1 toggles this menu");
    }
    ImGui::End();
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case WM_KEYDOWN:
            // not on auto repeat; the menu window and the game both take the key, the menu only when open
            if ((wp == VK_F1 || wp == VK_INSERT) && !(lp & (1 << 30))) {
                g_open.store(!g_open.load());
                if (result) *result = 0;
                return true;
            }
            break;
        case kFpsTitle: {
            wchar_t title[256];
            const int instance = app::Instance();
            if (g_fps_title) {
                if (instance)
                    swprintf(title, 256, L"%ls [%d] | %.0f FPS", app::kGame.title, instance, g_fps.load());
                else
                    swprintf(title, 256, L"%ls | %.0f FPS", app::kGame.title, g_fps.load());
            } else if (instance) {
                swprintf(title, 256, L"%ls [%d]", app::kGame.title, instance);
            } else {
                swprintf(title, 256, L"%ls", app::kGame.title);
            }
            SetWindowTextW(static_cast<HWND>(hwnd), title);
            if (result) *result = 0;
            return true;
        }
        default:
            break;
    }
    return false;
}

bool Open() { return g_open.load(); }

}  // namespace ui

#else  // no ImGui here (the headless harness): the menu and the hook do nothing

namespace ui {

bool g_fps_overlay = false;
bool g_fps_title = true;

void Draw(double) {}

bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }

bool Open() { return false; }

}  // namespace ui

#endif
