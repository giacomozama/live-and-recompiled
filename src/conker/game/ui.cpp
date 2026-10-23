// ImGui settings menu and fps readout, for GameInfo::ui and GameInfo::message (see game_info.cpp).
//
// The render thread draws the frame with app::UiLock held (vulkan.cpp RenderUi), and the window thread runs
// Message for every message after ImGui's. Draw must never make a blocking cross-thread call: it only posts.
// So the fps in the title is posted (kFpsTitle), and the window thread sets it.
//
// On the headless harness there is no ImGui and no window, so everything but the option globals compiles out;
// keyboard.cpp can still ask Open().
#include "common/ui.h"

#if __has_include(<imgui.h>) && !defined(XBE_HEADLESS)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/glass.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/metal.h"
#include "common/mouse.h"
#include "common/warp.h"
#include "common/water.h"
#include "core/input.h"

namespace ui {

bool g_fps_overlay = false;
bool g_fps_title = true;

namespace {

// the window owns the title; Draw posts this, Message (window thread) writes it
constexpr uint32_t kFpsTitle = app::kMsgApp + 0x40;

// virtual-key codes the menu reads
constexpr int kMouse1 = 0x01, kInsert = 0x2D, kEscape = 0x1B;

std::atomic<bool> g_open{false};
std::atomic<double> g_fps{0.0};

// the display settings while the menu is editing them; copied back on Apply
app::DisplayMode pending_mode = app::DisplayMode::Borderless;
uint32_t pending_w = 0, pending_h = 0;

// the display's modes, asked once
const std::vector<app::Mode>& Modes() {
    static const std::vector<app::Mode> modes = app::DisplayModes();
    return modes;
}

// the keyboard action waiting for its new key, -1 none, whose and which of its keys; render thread only
int g_rebind = -1;
int g_rebind_player = 0;
int g_rebind_slot = 0;
// a rebind waits for every key to be up before it takes one, so the click that started it is not the key
bool g_rebind_armed = false;
// after a rebind, frames left until its key counts as let go: ImGui would otherwise take it as a click too (a
// mouse button is released to ImGui a frame or so after app::KeyDown says so)
int g_rebind_settle = 0;
constexpr int kSettleFrames = 3;

// a rebind is waiting, for the window thread: Escape then cancels it instead of shutting the menu
std::atomic<bool> g_rebinding{false};

// how long the menu's settings hold still before it saves them
constexpr double kSaveDelay = 0.5;

// the waiting row's Cancel was under the mouse last frame: a left click there cancels instead of binding Mouse 1
bool g_cancel_hovered = false;

bool AnyDown() {
    for (int vk = 1; vk < 0x100; vk++)
        if (keyboard::Bindable(vk) && app::KeyDown(vk)) return true;
    return false;
}

// the menu is up and an action is waiting: the first key or button pressed is its new one
void PollRebind() {
    struct Mirror {
        ~Mirror() { g_rebinding.store(g_rebind >= 0); }
    } mirror;
    g_rebind_settle = AnyDown() ? (g_rebind_settle ? kSettleFrames : 0) : std::max(g_rebind_settle - 1, 0);
    if (g_rebind < 0) return;
    if (app::KeyDown(kEscape)) {
        g_rebind = -1;
        g_rebind_settle = kSettleFrames;
        return;
    }
    if (!g_rebind_armed) {
        g_rebind_armed = !AnyDown();
        return;
    }
    for (int vk = 1; vk < 0x100; vk++) {
        if (!keyboard::Bindable(vk) || !app::KeyDown(vk)) continue;
        if (vk == kMouse1 && g_cancel_hovered) {
            g_rebind = -1;
            g_rebind_settle = kSettleFrames;
            return;
        }
        // a key drives one of the player's actions once: wherever it was, it swaps with the key it replaces (this
        // action's other slot included)
        int& target = keyboard::g_keys[g_rebind_player][g_rebind][g_rebind_slot];
        for (auto& keys : keyboard::g_keys[g_rebind_player])
            for (int& k : keys)
                if (&k != &target && k == vk) k = target;
        target = vk;
        g_rebind = -1;
        g_rebind_settle = kSettleFrames;
        return;
    }
}

// a percent slider with a Reset beside it, greyed out while the value is its default
void Slider(const char* label, uint32_t* v, uint32_t min, uint32_t max, uint32_t def) {
    ImGui::PushID(label);
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - reset - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SliderScalar("##slider", ImGuiDataType_U32, v, &min, &max, "%u");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(*v == def);
    if (ImGui::Button("Reset")) *v = def;
    ImGui::EndDisabled();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
}

// the device in a player's keyboard, mouse or controller slot; picking one another player has moves it here
void DeviceCombo(int player, input::Kind kind) {
    using input::Kind;
    const std::vector<input::Device> devices = input::Devices(kind);
    const std::string held = input::Assigned(player, kind);
    const char* all = kind == Kind::Keyboard ? "All other keyboards" : "All other mice";
    // a device no longer connected keeps its name, without the #n
    auto label = [&](const std::string& key) -> std::string {
        if (key.empty()) return "None";
        if (key == input::kAll) return all;
        for (const input::Device& d : devices)
            if (d.key == key) return d.label;
        return key.substr(0, key.rfind('#')) + " (not connected)";
    };
    // whose it is now, when another player's
    auto whose = [&](const std::string& key) -> std::string {
        const int p = input::Holder(kind, key);
        return p >= 0 && p != player ? "  (player " + std::to_string(p + 1) + ")" : "";
    };
    ImGui::PushID(int(kind));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##device", label(held).c_str())) {
        auto option = [&](const std::string& key, const std::string& text) {
            if (ImGui::Selectable((text + whose(key) + "##" + key).c_str(), held == key)) input::Assign(player, kind, key);
        };
        option("", "None");
        if (kind != Kind::Controller) option(input::kAll, all);
        for (const input::Device& d : devices) option(d.key, d.label);
        ImGui::EndCombo();
    }
    if (kind != Kind::Controller && held == input::kAll && ImGui::IsItemHovered())
        ImGui::SetTooltip("every %s no other player has", kind == Kind::Keyboard ? "keyboard" : "mouse");
    ImGui::PopID();
}

// a player's devices, and whether new controllers find a player on their own (shared by all, under each tab)
void DrawDevices(int player) {
    ImGui::SeparatorText("Devices");
    for (input::Kind kind : {input::Kind::Keyboard, input::Kind::Mouse, input::Kind::Controller}) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kind == input::Kind::Keyboard ? "Keyboard" : kind == input::Kind::Mouse ? "Mouse" : "Controller");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.2f);
        DeviceCombo(player, kind);
    }
    bool auto_assign = input::AutoAssign();
    if (ImGui::Checkbox("New controllers go to the first player without one", &auto_assign))
        input::SetAutoAssign(auto_assign);
}

// a player's mouse look
void DrawMouse(int player) {
    ImGui::SeparatorText("Mouse");
    ImGui::Checkbox("Mouse look", &mouse::g_look[player]);
    ImGui::SameLine();
    ImGui::TextDisabled("turns the camera; Esc or alt-tab frees the cursor");
    Slider("Horizontal %", &mouse::g_sensitivity[player], 10, 500, mouse::kDefaultSensitivity);
    Slider("Vertical %", &mouse::g_sensitivity_y[player], 10, 500, mouse::kDefaultSensitivity);
    ImGui::Checkbox("Invert Y", &mouse::g_invert_y[player]);
}

// a player's keys
void DrawKeys(int player) {
    if (g_rebind >= 0 && g_rebind_player != player) g_rebind = -1;  // another player's tab: its rebind is done
    PollRebind();
    g_cancel_hovered = false;  // the waiting row sets it again as it draws
    ImGui::SeparatorText("Keyboard");
    ImGui::TextDisabled("two keys or mouse buttons each: click one, then press the key or button");
    ImGui::TextDisabled("Cancel stops a rebind, right click unbinds; a key in use swaps");
    // a frame per part of the controller; the frame names the part, so a row only names its action
    struct Row {
        int action;
        const char* label;
    };
    struct Group {
        const char* title;
        std::vector<Row> rows;
    };
    using namespace keyboard;
    static const Group kGroups[] = {
        {"Left stick", {{kMoveUp, "Up"}, {kMoveDown, "Down"}, {kMoveLeft, "Left"}, {kMoveRight, "Right"},
                        {kLS, "Click"}}},
        {"Right stick (camera)", {{kCamUp, "Up"}, {kCamDown, "Down"}, {kCamLeft, "Left"}, {kCamRight, "Right"},
                                  {kRS, "Click"}}},
        {"D-pad", {{kDpadUp, "Up"}, {kDpadDown, "Down"}, {kDpadLeft, "Left"}, {kDpadRight, "Right"}}},
        {"Face buttons", {{kA, "A"}, {kB, "B"}, {kX, "X"}, {kY, "Y"}}},
        {"Shoulders and triggers", {{kLB, "LB (white)"}, {kRB, "RB (black)"}, {kLT, "LT"}, {kRT, "RT"}}},
        {"Start / Back", {{kStart, "Start"}, {kBack, "Back"}}},
    };
    // two columns, the groups in pairs: the sticks, d-pad and face buttons, shoulders and start / back
    if (!ImGui::BeginTable("##groups", 2, ImGuiTableFlags_SizingStretchSame)) return;
    for (const Group& g : kGroups) {
        ImGui::TableNextColumn();
        ImGui::PushID(g.title);
        ImGui::BeginChild("##group", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        ImGui::SeparatorText(g.title);
        // label, the action's keys side by side, then its Reset; the keys line up down the frame
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float reset = std::max(ImGui::CalcTextSize("Reset").x, ImGui::CalcTextSize("Cancel").x) +
                            ImGui::GetStyle().FramePadding.x * 2.0f;  // the same button, either label
        const float avail = ImGui::GetContentRegionAvail().x;
        const float column = ImGui::GetCursorPosX() + avail * 0.3f;
        const float key_w = std::max((avail * 0.7f - reset - spacing * kSlots) / kSlots, 1.0f);
        for (const Row& r : g.rows) {
            ImGui::PushID(r.action);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.label);
            ImGui::SameLine(column);
            for (int s = 0; s < kSlots; s++) {
                if (s) ImGui::SameLine(0, spacing);
                ImGui::PushID(s);
                const bool waiting = g_rebind == r.action && g_rebind_slot == s;
                if (ImGui::Button(waiting ? "press..." : KeyName(g_keys[player][r.action][s]), ImVec2(key_w, 0)) &&
                    !g_rebind_settle) {
                    g_rebind = r.action;
                    g_rebind_player = player;
                    g_rebind_slot = s;
                    g_rebind_armed = false;
                }
                // right click unbinds, but not while a rebind waits or has just taken its key: then the click is
                // Mouse 2 being bound, which PollRebind takes before this button sees it
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && g_rebind < 0 && !g_rebind_settle)
                    g_keys[player][r.action][s] = 0;
                ImGui::PopID();
            }
            ImGui::SameLine(0, spacing);
            if (g_rebind == r.action) {
                // waiting: Reset becomes Cancel, which PollRebind honours when the left click lands on it
                ImGui::Button("Cancel", ImVec2(reset, 0));
                g_cancel_hovered = ImGui::IsItemHovered();
            } else {
                ImGui::BeginDisabled(IsDefault(player, r.action));
                if (ImGui::Button("Reset", ImVec2(reset, 0))) ResetKey(player, r.action);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("default: %s", ActionDefault(r.action).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ImGui::Button("Default keys")) {
        keyboard::ResetKeys(player);
        g_rebind = -1;
    }
}

// the Debug tab: warps to a checkpoint, a level or a cutscene (warp.cpp runs them on the main thread)
void DrawWarp() {
    static char filter[64] = "";
    static int checkpoint = -1;
    static int level = 0, spawn = 0, cut_level = 0, cut_index = 0;

    const int current = warp::CurrentLevel();
    const char* current_name = warp::LevelName(current);
    ImGui::Text("Level: %d %s", current, current_name ? current_name : "");
    const std::string status = warp::Status();
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    ImGui::TextDisabled("A checkpoint or level starts a new game: save after it only to a slot you can lose");

    ImGui::SeparatorText("Checkpoints");
    ImGui::InputTextWithHint("##filter", "filter by name or level", filter, sizeof filter);
    const std::vector<warp::Checkpoint>& list = warp::Checkpoints();
    auto matches = [](const std::string& s) {
        std::string a = s, b = filter;
        for (char& c : a) c = char(std::tolower(uint8_t(c)));
        for (char& c : b) c = char(std::tolower(uint8_t(c)));
        return a.find(b) != std::string::npos;
    };
    if (ImGui::BeginListBox("##checkpoints", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 10))) {
        for (int i = 0; i < int(list.size()); i++) {
            const warp::Checkpoint& c = list[size_t(i)];
            const char* ln = warp::LevelName(c.level);
            if (!ln) continue;  // the multiplayer ones, whose levels are another table's
            char row[192];
            std::snprintf(row, sizeof row, "%3d  %-20s spawn %-3d %s", i, ln, c.spawn, c.name.c_str());
            if (filter[0] && !matches(row)) continue;
            if (ImGui::Selectable(row, checkpoint == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                checkpoint = i;
                if (ImGui::IsMouseDoubleClicked(0)) warp::GoCheckpoint(i);
            }
        }
        ImGui::EndListBox();
    }
    ImGui::BeginDisabled(checkpoint < 0);
    if (ImGui::Button("Start at checkpoint")) warp::GoCheckpoint(checkpoint);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("or double-click; earlier checkpoints count as done");

    // the levels that exist, for the combos
    std::string items;
    std::vector<int> ids;
    for (int i = 0; i < warp::kLevels; i++)
        if (const char* n = warp::LevelName(i)) {
            items += std::to_string(i) + " " + n + '\0';
            ids.push_back(i);
        }
    items += '\0';
    auto level_combo = [&](const char* label, int* v) {
        int idx = int(std::find(ids.begin(), ids.end(), *v) - ids.begin());
        if (idx == int(ids.size())) idx = 0;
        ImGui::Combo(label, &idx, items.c_str());
        *v = ids[size_t(idx)];
    };

    ImGui::SeparatorText("Level");
    level_combo("Level##level", &level);
    ImGui::InputInt("Spawn", &spawn);
    spawn = std::clamp(spawn, 0, 255);
    if (ImGui::Button("Load level")) warp::GoLevel(level, spawn);
    ImGui::SameLine();
    ImGui::TextDisabled("a fresh game, nothing done");

    ImGui::SeparatorText("Cutscenes");
    int found_level = -1;
    const std::vector<int> found = warp::Cutscenes(&found_level);
    if (found_level >= 0) {
        const char* fn = warp::LevelName(found_level);
        ImGui::Text("In %s:", fn ? fn : "this level");
        if (found.empty()) ImGui::TextDisabled("none");
        for (size_t i = 0; i < found.size(); i++) {
            if (i % 10) ImGui::SameLine();
            if (ImGui::Button(std::to_string(found[i]).c_str())) warp::PlayCutscene(found_level, found[i]);
        }
    }
    level_combo("Level##cut", &cut_level);
    ImGui::InputInt("Cutscene", &cut_index);
    cut_index = std::clamp(cut_index, 0, 255);
    if (ImGui::Button("Play cutscene")) warp::PlayCutscene(cut_level, cut_index);
    ImGui::SameLine();
    ImGui::TextDisabled("loads its level first; one it lacks is refused there");
}

}  // namespace

void Draw(double fps) {
    // the menu's pending display settings follow the live ones each time it opens
    static bool was_open = false;
    static double last_fps = -1.0;
    static bool last_title = !g_fps_title;
    static int last_look = -1;
    const bool open = g_open.load();
    if (open && !was_open) {
        pending_mode = app::Get().display_mode;
        pending_w = app::Get().width;
        pending_h = app::Get().height;
    }
    if (!open) g_rebind = -1;  // closing the menu cancels a rebind

    // the menu saves itself: once its settings have held still for kSaveDelay (not every frame of a slider's drag),
    // and as it shuts
    static std::string seen;
    static double seen_at = 0.0;
    if (open) {
        std::string now = app::SettingsText();
        const double t = ImGui::GetTime();
        if (now != seen) seen = std::move(now), seen_at = t;
        else if (t - seen_at >= kSaveDelay) app::SaveChanged();
    } else if (was_open) {
        app::SaveChanged();
    }
    was_open = open;

    // not from here: setting the title from the render thread could deadlock on the window thread's UiLock; post
    if (fps != last_fps || g_fps_title != last_title) {
        last_fps = fps;
        last_title = g_fps_title;
        g_fps.store(fps);
        app::Post(kFpsTitle);
    }

    // the window thread takes or lets go of the cursor, as a player with a mouse turns mouse look on or off, or gets
    // or loses the mouse
    int look = 0;
    for (int p = 0; p < input::kPorts; p++)
        if (mouse::g_look[p] && input::Has(p, input::Kind::Mouse)) look |= 1 << p;
    if (look != last_look) {
        last_look = look;
        app::Post(mouse::kRefresh);
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
    // first opened (there is no imgui.ini, so each launch): about half the display, centred, never smaller than the
    // keyboard bindings need nor larger than the display
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetFontSize() / 13.0f;  // the minimums were set with ImGui's 13 px font
    const ImVec2 size(std::min(std::max(display.x * 0.45f, 720.0f * scale), display.x),
                      std::min(std::max(display.y * 0.65f, 640.0f * scale), display.y));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool keep_open = true;
    if (ImGui::Begin("Settings", &keep_open)) {
        bool keys_shown = false;
        if (ImGui::BeginTabBar("##tabs")) {
            // each tab scrolls on its own above the footer line
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
            auto tab = [&](const char* name) {
                if (!ImGui::BeginTabItem(name)) return false;
                ImGui::BeginChild("##page", ImVec2(0, -footer));
                return true;
            };
            auto end_tab = [] {
                ImGui::EndChild();
                ImGui::EndTabItem();
            };

            if (tab("Display")) {
                ImGui::SeparatorText("Display");
                int mode = int(pending_mode);
                if (ImGui::Combo("Mode", &mode, "Fullscreen\0Borderless\0Windowed\0"))
                    pending_mode = app::DisplayMode(mode);

                const std::vector<app::Mode>& modes = Modes();
                std::string res = "Desktop";
                int res_idx = 0;
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

                ImGui::SeparatorText("Aspect");
                // the camera follows the setting live; only the TV type the console reports waits for a restart
                const char* aspects[] = {"auto", "4:3", "16:10", "16:9", "21:9", "32:9"};
                constexpr int kAspects = 6;
                int aspect = -1;
                for (int i = 0; i < kAspects; i++)
                    if (s.aspect == aspects[i]) aspect = i;
                std::string items;
                for (int i = 0; i < kAspects; i++) items += std::string(aspects[i]) + '\0';
                if (aspect < 0) items += s.aspect + '\0', aspect = kAspects;  // a custom W:H from settings.toml
                items += '\0';
                if (ImGui::Combo("Aspect", &aspect, items.c_str()) && aspect < kAspects) s.aspect = aspects[aspect];
                ImGui::SameLine();
                ImGui::TextDisabled("(%.2f:1)", app::FrameAspect());

                ImGui::SeparatorText("FPS");
                ImGui::Checkbox("Overlay", &g_fps_overlay);
                ImGui::SameLine();
                ImGui::Checkbox("Window title", &g_fps_title);
                end_tab();
            }

            if (tab("Graphics")) {
                ImGui::SeparatorText("Rendering");
                Slider("Render %", &s.render_percent, 25, 400, app::Settings{}.render_percent);
                ImGui::Checkbox("V-Sync", &s.vsync);

                constexpr uint32_t kRates[] = {30, 60, 120, 180, 240};
                int rate = 0;
                for (int i = 0; i < 5; i++)
                    if (kRates[i] == display::g_frame_rate) rate = i;
                if (ImGui::Combo("Frame rate", &rate, "30 (original)\0 60\0 120\0 180\0 240\0"))
                    display::SetFrameRate(kRates[rate]);
                if (display::g_frame_rate > 60) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the game at 60, the frames between drawn again)");
                }

                ImGui::SeparatorText("Quality");
                constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
                int aniso = 0;
                for (int i = 0; i < 5; i++)
                    if (kAniso[i] == s.aniso) aniso = i;
                if (ImGui::Combo("Anisotropic", &aniso, "Game default\0 2x\0 4x\0 8x\0 16x\0"))
                    s.aniso = kAniso[aniso];

                constexpr uint32_t kMsaa[] = {1, 2, 4, 8};
                int msaa = 0;
                for (int i = 0; i < 4; i++)
                    if (kMsaa[i] == s.msaa) msaa = i;
                if (ImGui::Combo("MSAA", &msaa, "Off\0 2x\0 4x\0 8x\0")) s.msaa = kMsaa[msaa];

                ImGui::SeparatorText("Effects");
                ImGui::TextDisabled("over the 3D picture, not the menus and text drawn over it");
                ImGui::Checkbox("SMAA", &s.smaa);
                Slider("Film grain %", &s.film_grain, 0, 100, app::Settings{}.film_grain);
                Slider("Chromatic aberration %", &s.chromatic_aberration, 0, 100, app::Settings{}.chromatic_aberration);

                ImGui::Checkbox("Wind on distant grass", &grass::g_wind);
                ImGui::SameLine();
                ImGui::TextDisabled("the console keeps it still");
                ImGui::Checkbox("Fur shading", &fur::g_shading);
                ImGui::SameLine();
                ImGui::TextDisabled("soft strand edges, shadowed roots, sheen");
                ImGui::Checkbox("Dynamic water", &water::g_dynamic);
                ImGui::SameLine();
                ImGui::TextDisabled("ripples and wakes");
                ImGui::Checkbox("Metal shading", &metal::g_shading);
                ImGui::SameLine();
                ImGui::TextDisabled("armour catches highlights and reflections");
                ImGui::Checkbox("Glass shading", &glass::g_shading);
                ImGui::SameLine();
                ImGui::TextDisabled("bottles glow through, glint and reflect");
                end_tab();
            }

            if (tab("Gameplay")) {
                ImGui::Checkbox("Skippable cutscenes", &cutscenes::g_skippable);
                ImGui::SameLine();
                ImGui::TextDisabled("LT skips any cutscene, seen or not");
                int split = display::g_side_by_side;
                if (ImGui::Combo("Two player split", &split, "Top and bottom\0Side by side (experimental)\0"))
                    display::g_side_by_side = split != 0;
                ImGui::SameLine();
                ImGui::TextDisabled(split ? "from the next match; some HUD pieces stay where top and bottom puts them"
                                          : "from the next match");
                end_tab();
            }

            if (tab("Controls")) {
                if (ImGui::BeginTabBar("##player")) {
                    for (int p = 0; p < input::kPorts; p++) {
                        const std::string name = "Player " + std::to_string(p + 1);
                        if (!ImGui::BeginTabItem(name.c_str())) continue;
                        ImGui::PushID(p);
                        DrawDevices(p);
                        DrawMouse(p);
                        DrawKeys(p);
                        keys_shown = true;
                        ImGui::PopID();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                end_tab();
            }

            if (tab("Debug")) {
                DrawWarp();
                end_tab();
            }
            ImGui::EndTabBar();
        }
        if (!keys_shown) g_rebind = -1;  // leaving the Controls tab cancels a rebind

        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Esc toggles this menu; changes save as they are made");
        const char* quit = "Quit game";
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(quit).x -
                        ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::Button(quit)) ImGui::OpenPopup("Quit?");
        if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Quit the game? Progress since the last save is lost.");
            if (ImGui::Button("Quit")) app::Quit();  // only posts: the window thread closes
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    if (!keep_open) g_open.store(false);  // the window's X
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case app::kMsgKeyDown:
            // not on auto repeat; the menu window and the game both take the key, the menu only when open
            // Escape cancels a rebind waiting for its key instead (PollRebind)
            if ((wp == kEscape || wp == kInsert) && !(lp & (1 << 30)) && !(wp == kEscape && g_rebinding.load())) {
                g_open.store(!g_open.load());
                mouse::Refresh(hwnd);  // the cursor is free while the menu is up
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
            app::SetTitle(title);
            if (result) *result = 0;
            return true;
        }
        default:
            break;
    }
    mouse::Message(hwnd, msg, wp, lp);
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
