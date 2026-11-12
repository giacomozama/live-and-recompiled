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
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiItemFlags_NoNav

#include "app/app.h"
#include "audio/audio.h"
#include "common/characters.h"
#include "common/cutscenes.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/gamepad.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/lighting.h"
#include "common/loading.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "common/prompts.h"
#include "common/reflection.h"
#include "common/rope.h"
#include "common/vignette.h"
#include "common/warp.h"
#include "common/water.h"
#include "core/input.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "version.h"

namespace xnet {
// runtime xdk/xnet.cpp
std::string LocalAddress();
uint16_t Port();
int PortError();
}

namespace ui {

bool g_fps_overlay = false;
uint32_t g_accent = 4;  // orange
bool g_fps_title = true;
bool g_menu_resizable = false;

namespace {

// the window owns the title; Draw posts this, Message (window thread) writes it
constexpr uint32_t kFpsTitle = app::kMsgApp + 0x40;

// virtual-key codes the menu reads
constexpr int kMouse1 = 0x01, kEscape = 0x1B, kF1 = 0x70;

std::atomic<bool> g_open{false};
float g_fade = 1.0f;  // the menu's alpha as it fades in; render thread only
// the quit prompt alone, over the game (the frontend's Start)
std::atomic<bool> g_quit{false};
// the port of the controller that opened the menu, -1 a keyboard
std::atomic<int> g_owner{-1};
// Escape went to shut the menu or the prompt: it is not the game's Start until it is let go
std::atomic<bool> g_esc_swallow{false};

// the main tab to show next frame (Show asks for one, the bumpers turn), -1 none; the tab drawn now; the player
// the Controls tab shows
constexpr int kMainTabs = 9, kControlsTab = 4, kMultiplayerTab = 5, kQuitTab = 8;
// the Quit tab's color
constexpr ImVec4 kDanger(0.93f, 0.33f, 0.33f, 1.0f);
std::atomic<int> g_tab_target{-1};
int g_tab_now = 0;
int g_player_now = 0;

// ImGui picks a tab when it is clicked or activated, not when the cursor reaches it: a tab the cursor arrives on is
// picked, so the page under it is there to move down into. The tab the cursor was on, to tell arriving
bool Arrived(ImGuiID& was) {
    const bool focused = ImGui::IsItemFocused();
    const bool arrived = focused && was != ImGui::GetItemID();
    if (focused) was = ImGui::GetItemID();
    return arrived;
}
ImGuiID g_main_focus = 0;
bool g_tab_arrived = false;  // the cursor reached a tab this frame (its page follows); render thread only

// the sounds of the menu, for the keyboard and the pads alone: a tab or setting changing, and the cursor moving.
// A key or pad press counts for a moment, until the mouse is used
void Cues(bool opened_before, bool settings_changed) {
    static constexpr ImGuiKey kNav[] = {ImGuiKey_UpArrow,        ImGuiKey_DownArrow,      ImGuiKey_LeftArrow,
                                        ImGuiKey_RightArrow,     ImGuiKey_Enter,          ImGuiKey_KeypadEnter,
                                        ImGuiKey_Space,          ImGuiKey_Tab,            ImGuiKey_PageUp,
                                        ImGuiKey_PageDown,       ImGuiKey_GamepadDpadUp,  ImGuiKey_GamepadDpadDown,
                                        ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown,
                                        ImGuiKey_GamepadL1,      ImGuiKey_GamepadR1};
    static double key_at = -1.0, mouse_at = -1.0, select_at = -1.0, move_at = -1.0;
    static bool was_popup = false;
    static ImGuiID nav = 0, active = 0;
    static double active_at = 0.0;
    static bool active_keys = false;
    static const ImGuiWindow* nav_window = nullptr;
    static int tab = 0;
    const double now = ImGui::GetTime();
    for (ImGuiKey k : kNav)
        if (ImGui::IsKeyPressed(k)) key_at = now;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) mouse_at = now;
    const bool by_keys = key_at > mouse_at && now - key_at < 0.3;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID id = g.NavId;
    // an item taken up (a slider), or the cursor going into a list (the checkpoints) to pick from it
    const bool activated = g.ActiveId != 0 && g.ActiveId != active && g.ActiveIdSource != ImGuiInputSource_Mouse;
    // ... and let go of; a button's press is over at once and is not
    const bool released = g.ActiveId == 0 && active != 0 && active_keys && now - active_at > 0.1;
    if (g.ActiveId != active) active_at = now, active_keys = g.ActiveIdSource != ImGuiInputSource_Mouse;
    active = g.ActiveId;
    const ImGuiWindow* win = g.NavWindow;
    const bool entered = win != nav_window && win && (win->Flags & ImGuiWindowFlags_ChildWindow) &&
                         !(win->ChildFlags & ImGuiChildFlags_NavFlattened) && !(win->Flags & ImGuiWindowFlags_Popup);
    const bool left = win != nav_window && nav_window && !entered;
    nav_window = win;
    const bool moved = id != nav && opened_before;
    nav = id;
    const bool tab_changed = g_tab_now != tab && opened_before;
    tab = g_tab_now;
    // a dropdown opening or closing
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool popup_changed = popup != was_popup && opened_before;
    was_popup = popup;
    // the bumpers turn the tabs without a key, so a tab is by the mouse only while it was just used
    const bool by_mouse = now - mouse_at < 0.3;
    const bool select = ((tab_changed && !by_mouse) || ((settings_changed || popup_changed || activated || entered) && by_keys)) &&
                        now - select_at >= 0.05;
    if (!app::Get().menu_sounds) return;
    if (select) {
        audio::Play(audio::Cue::Select);
        select_at = move_at = now;
    } else if ((moved || released || left) && by_keys && !g_tab_arrived && !popup_changed && now - move_at >= 0.05) {
        audio::Play(audio::Cue::Move);
        move_at = now;
    }
}

// the game is held while the menu or the prompt is up (kernel::Hold), but for a multiplayer match, which runs on for
// the other players (Draw lets go or holds again as a match starts or ends under the menu)
void Hold() { kernel::Hold((g_open.load() || g_quit.load()) && !warp::Multiplayer()); }

// the menu shows or goes
void SetOpen(bool open) {
    // whatever is held as the menu shuts stays off the game's pads until it is let go, even if the game never polled
    // them while the menu was up (it is held then)
    if (!open && g_open.load())
        for (int port = 0; port < input::kPorts; port++) ui::Muzzle(port);
    g_open.store(open);
    if (!open) g_owner.store(-1);
    Hold();
}

// the prompt shows or goes
void SetQuit(bool quit) {
    g_quit.store(quit);
    Hold();
}

// the menu shut from the render thread (its X, a warp): the window thread takes the cursor back as Message does
void Close() {
    SetOpen(false);
    app::Post(mouse::kRefresh);
}

// what the pads did this frame for the menu to read (PadFrame sets it, render thread)
struct PadEdges {
    bool a = false, b = false, y = false;  // A, B and Y just pressed
    float scroll = 0.0f;        // pixels the right stick scrolls the page
} g_pad;

std::atomic<double> g_fps{0.0};

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
    if (app::KeyDown(kEscape) || g_pad.b) {
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

#include "ui_text.inc"

// the overlay's language: the settings' ui_language (the menu's UI combo changes it at once), apart from the game's
int Column() {
    static const char* const kKeys[] = {"english", "german", "french", "spanish", "italian", "japanese"};
    const std::string& language = app::Get().ui_language;
    for (int i = 0; i < 6; i++)
        if (language == kKeys[i]) return i;
    return 0;
}

// the text in the overlay's language; English where there is no translation. Its English text is also the ID of
// the widget that shows it, so a widget keeps its ID, and its focus, across languages
__attribute__((format_arg(1))) const char* T(const char* en) {
    static const std::unordered_map<std::string_view, const Line*> lines = [] {
        std::unordered_map<std::string_view, const Line*> map;
        for (const Line& line : kLines) map.emplace(line.en, &line);
        return map;
    }();
    const auto it = lines.find(en);
    if (it == lines.end()) return en;
    const Line& l = *it->second;
    const char* const text[] = {l.en, l.de, l.fr, l.es, l.it, l.ja};
    return text[Column()];
}

// a dropdown's items, each in the overlay's language, as ImGui wants them
std::string Items(std::initializer_list<const char*> items) {
    std::string out;
    for (const char* item : items) out += T(item), out += '\0';
    out += '\0';
    return out;
}

// a hint under the setting before it: smaller, dimmed and wrapped, lined up with a checkbox's label
void Hint(const char* text, bool checkbox = false) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float indent = checkbox ? ImGui::GetFrameHeight() + style.ItemInnerSpacing.x : 0.0f;
    ImGui::PushFont(nullptr, std::max(std::round(style.FontSizeBase * 0.8f), 11.0f));
    // close to its setting; a checkbox's label sits mid-frame, so under a dropdown or slider it keeps the full spacing
    if (checkbox) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y * 0.5f);
    if (indent > 0) ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", T(text));
    ImGui::PopStyleColor();
    if (indent > 0) ImGui::Unindent(indent);
    ImGui::PopFont();
}

// a hint as a dotted list, under the setting before it as Hint
void Hints(std::initializer_list<const char*> lines) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(nullptr, std::max(std::round(style.FontSizeBase * 0.8f), 11.0f));
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.25f));  // set apart from the setting above and what follows
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    for (const char* line : lines) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", T(line));  // wrapped lines line up under the first, past the bullet
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.75f));
}

// a setting's label to the left of its widget, in a column as wide for every setting (wrapped if it is longer): draws
// it, sizes the widget that follows it (to the row's end, less `after` for a button beside it) and returns the widget's hidden label
float LabelWidth() { return std::min(ImGui::GetFontSize() * 13.0f, ImGui::GetContentRegionAvail().x * 0.4f); }  // less in a narrow column
// on to the value column on the same line, `start` the label's screen x (SameLine's offset would be off in a table cell)
void ToValueColumn(float start, float width) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(start + width + ImGui::GetStyle().ItemSpacing.x, ImGui::GetCursorScreenPos().y));
}
const char* Beside(const char* label, float after = 0.0f) {
    static std::string id;
    const float x = ImGui::GetCursorScreenPos().x, width = LabelWidth();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + width);
    ImGui::TextUnformatted(T(label));
    ImGui::PopTextWrapPos();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(x + width + ImGui::GetStyle().ItemSpacing.x, ImGui::GetCursorScreenPos().y));
    const float room = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(room - after, ImGui::GetFontSize() * 4.0f));
    id = std::string("##") + label;
    return id.c_str();
}
// where the last slider ended, to tell a checkbox that follows it
struct SliderEnd { int frame = -1; float y = 0.0f; } g_slider_end;

// a percent slider with a Reset beside it, greyed out while the value is its default
void Slider(const char* label, uint32_t* v, uint32_t min, uint32_t max, uint32_t def) {
    ImGui::PushID(label);
    const float reset = ImGui::CalcTextSize(T("Reset")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    Beside(label, reset + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SliderScalar("##slider", ImGuiDataType_U32, v, &min, &max, "%u");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(*v == def);
    if (ImGui::Button(T("Reset"))) *v = def;
    ImGui::EndDisabled();
    ImGui::PopID();
    g_slider_end = {ImGui::GetFrameCount(), ImGui::GetCursorPosY()};
}

// Two percentages on one track, lo below hi and kept at least `gap` apart, with a Reset beside it. The handle nearest
// the click takes the drag.
void RangeSlider(const char* label, uint32_t* lo, uint32_t* hi, uint32_t min, uint32_t max, uint32_t gap,
                 uint32_t def_lo, uint32_t def_hi) {
    ImGui::PushID(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float reset = ImGui::CalcTextSize(T("Reset")).x + st.FramePadding.x * 2.0f;
    Beside(label, reset + st.ItemInnerSpacing.x);
    const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##range", ImVec2(w, h));
    static int dragging = 0;  // 0 the low handle, 1 the high
    const float r = h * 0.35f, x0 = p.x + r, span = std::max(w - 2.0f * r, 1.0f);
    auto at = [&](uint32_t v) { return x0 + span * float(v - min) / float(max - min); };
    if (ImGui::IsItemActivated()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float dl = std::fabs(mx - at(*lo)), dh = std::fabs(mx - at(*hi));
        dragging = dl < dh || (dl == dh && mx < at(*lo)) ? 0 : 1;
    }
    if (ImGui::IsItemActive()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / span, 0.0f, 1.0f);
        const uint32_t v = min + uint32_t(std::lround(t * float(max - min)));
        if (dragging == 0) *lo = std::clamp(v, min, *hi - gap);
        else *hi = std::clamp(v, *lo + gap, max);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, cy - 2), ImVec2(p.x + w, cy + 2), ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
    dl->AddRectFilled(ImVec2(at(*lo), cy - 2), ImVec2(at(*hi), cy + 2), ImGui::GetColorU32(ImGuiCol_SliderGrab), 2.0f);
    for (int i = 0; i < 2; i++) {
        const bool on = ImGui::IsItemActive() && dragging == i;
        const uint32_t v = i ? *hi : *lo;
        dl->AddCircleFilled(ImVec2(at(v), cy), r,
                            ImGui::GetColorU32(on ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab));
        char text[8];
        std::snprintf(text, sizeof text, "%u", v);
        const ImVec2 size = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(at(v) - size.x * 0.5f, p.y + h + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), text);
    }
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::BeginDisabled(*lo == def_lo && *hi == def_hi);
    if (ImGui::Button(T("Reset"))) *lo = def_lo, *hi = def_hi;
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() + 2.0f));  // room for the numbers under the track
    ImGui::PopID();
    g_slider_end = {ImGui::GetFrameCount(), ImGui::GetCursorPosY()};
}

// a checkbox; right under a slider it gets a little more room
bool Check(const char* label, bool* v) {
    if (g_slider_end.frame == ImGui::GetFrameCount() && std::fabs(ImGui::GetCursorPosY() - g_slider_end.y) < 0.5f)
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));
    return ImGui::Checkbox((std::string(T(label)) + "##" + label).c_str(), v);
}

// the device in a player's keyboard, mouse or controller slot; picking one another player has moves it here
void DeviceCombo(int player, input::Kind kind) {
    using input::Kind;
    const std::vector<input::Device> devices = input::Devices(kind);
    const std::string held = input::Assigned(player, kind);
    const char* all = kind == Kind::Keyboard ? T("All other keyboards") : T("All other mice");
    // a device no longer connected keeps its name, without the #n
    auto label = [&](const std::string& key) -> std::string {
        if (key.empty()) return T("None");
        if (key == input::kAll) return all;
        for (const input::Device& d : devices)
            if (d.key == key) return d.label;
        char text[512];
        std::snprintf(text, sizeof text, T("%s (not connected)"), key.substr(0, key.rfind('#')).c_str());
        return text;
    };
    // whose it is now, when another player's
    auto whose = [&](const std::string& key) -> std::string {
        const int p = input::Holder(kind, key);
        if (p < 0 || p == player) return "";
        char text[64];
        std::snprintf(text, sizeof text, T("  (player %d)"), p + 1);
        return text;
    };
    ImGui::PushID(int(kind));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##device", label(held).c_str())) {
        auto option = [&](const std::string& key, const std::string& text) {
            if (ImGui::Selectable((text + whose(key) + "##" + key).c_str(), held == key)) input::Assign(player, kind, key);
        };
        option("", T("None"));
        if (kind != Kind::Controller) option(input::kAll, all);
        for (const input::Device& d : devices) option(d.key, d.label);
        ImGui::EndCombo();
    }
    if (kind != Kind::Controller && held == input::kAll && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", kind == Kind::Keyboard ? T("Uses every keyboard not given to another player")
                                                    : T("Uses every mouse not given to another player"));
    ImGui::PopID();
}

// a player's devices, and whether new controllers find a player on their own (shared by all, under each tab)
void DrawDevices(int player) {
    ImGui::SeparatorText(T("Devices"));
    const float x0 = ImGui::GetCursorScreenPos().x, label_width = LabelWidth();
    for (input::Kind kind : {input::Kind::Keyboard, input::Kind::Mouse, input::Kind::Controller}) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(kind == input::Kind::Keyboard ? "Keyboard" : kind == input::Kind::Mouse ? "Mouse" : "Controller"));
        ToValueColumn(x0, label_width);  // the column of the other settings
        DeviceCombo(player, kind);
    }
    bool auto_assign = input::AutoAssign();
    if (Check("New controllers go to the first player without one", &auto_assign))
        input::SetAutoAssign(auto_assign);
}

// a player's controller options, the game's own two (they replace its Options menu's)
void DrawController(int player) {
    ImGui::SeparatorText(T("Controller"));
    Check("Invert aim", &gamepad::g_invert_aim[player]);
    Hint("Up on the right stick aims down while aiming. The camera and the mouse aren't affected.", true);
    Check("Vibration", &gamepad::g_vibration[player]);
    ImGui::BeginDisabled(!gamepad::g_vibration[player]);
    Slider("Vibration %", &gamepad::g_vibration_percent[player], 0, 100, 100);
    ImGui::EndDisabled();
    Hint("How hard the controller shakes. NOTE: These Invert aim and Vibration settings are single player only: "
         "in multiplayer, the profile's own in-engine settings decide.");
}

// a player's mouse look
void DrawMouse(int player) {
    ImGui::SeparatorText(T("Mouse"));
    Check("Mouse look", &mouse::g_look[player]);
    Hint("Move the mouse to look around. Open this menu (F1) or press Alt+Tab to get the cursor back.", true);
    Slider("Horizontal %", &mouse::g_sensitivity[player], 10, 500, mouse::kDefaultSensitivity);
    Slider("Vertical %", &mouse::g_sensitivity_y[player], 10, 500, mouse::kDefaultSensitivity);
    Check("Invert Y", &mouse::g_invert_y[player]);
    Hint("Single player only: in a match the mouse follows the profile's Invert.", true);
}

// a player's keys
void DrawKeys(int player) {
    // another player's tab: its rebind is done
    if (g_rebind >= 0 && g_rebind_player != player) g_rebind = -1;
    PollRebind();
    g_cancel_hovered = false;  // the waiting row sets it again as it draws
    ImGui::SeparatorText(T("Keyboard"));
    Hint("Right-click a slot (Y on a controller) to clear it. Picking a key that is already used swaps the two.");
    if (const int scheme = keyboard::Scheme(player))
        ImGui::Text(T("In use now: %s's controls"), keyboard::SchemeName(scheme));
    // a frame per kind of action; the frame names it, so a row only names its action
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
        {"Movement", {{kMoveUp, "Forward"}, {kMoveDown, "Back"}, {kMoveLeft, "Left"}, {kMoveRight, "Right"},
                      {kJump, "Jump"}, {kCrouch, "Crouch"}, {kWalk, "Walk"}, {kCreep, "Creep"}}},
        {"Camera", {{kLookUp, "Look up"}, {kLookDown, "Look down"}, {kLookLeft, "Look left"},
                    {kLookRight, "Look right"}, {kZoom, "Zoom (story)"}}},
        {"Weapons", {{kAttack, "Attack / fire"}, {kAim, "Aim / equip weapon"}, {kWeaponMode, "First person / mode"},
                     {kReload, "Reload"}, {kUse, "Use (story)"}}},
        {"Multiplayer", {{kAbility, "Abilities"}, {kCycle, "Cycle equipped (hold for item wheel)"}, {kVote, "Vote kick"},
                         {kRadio, "Radio mode"}}},
        {"Game", {{kPause, "Pause"}, {kQuit, "Quit (title menu)"}}},
    };
    // two columns, the groups in pairs
    if (!ImGui::BeginTable("##groups", 2, ImGuiTableFlags_SizingStretchSame)) return;
    for (const Group& g : kGroups) {
        ImGui::TableNextColumn();
        ImGui::PushID(g.title);
        ImGui::BeginChild("##group", ImVec2(0, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
        ImGui::SeparatorText(T(g.title));
        // label, the action's keys side by side, then its Reset; the keys line up down the frame
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float reset = std::max(ImGui::CalcTextSize(T("Reset")).x, ImGui::CalcTextSize(T("Cancel")).x) +
                            ImGui::GetStyle().FramePadding.x * 2.0f;  // the same button, either label
        const float avail = ImGui::GetContentRegionAvail().x;
        const float column = ImGui::GetCursorPosX() + avail * 0.3f;
        const float key_w = std::max((avail * 0.7f - reset - spacing * kSlots) / kSlots, 1.0f);
        for (const Row& r : g.rows) {
            ImGui::PushID(r.action);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(r.label));
            ImGui::SameLine(column);
            for (int s = 0; s < kSlots; s++) {
                if (s) ImGui::SameLine(0, spacing);
                ImGui::PushID(s);
                const bool waiting = g_rebind == r.action && g_rebind_slot == s;
                if (ImGui::Button(waiting ? T("press...") : KeyName(g_keys[player][r.action][s]),
                                  ImVec2(key_w, 0)) &&
                    !g_rebind_settle) {
                    g_rebind = r.action;
                    g_rebind_player = player;
                    g_rebind_slot = s;
                    g_rebind_armed = false;
                }
                // right click unbinds, but not while a rebind waits or has just taken its key: then the click is
                // Mouse 2 being bound, which PollRebind takes before this button sees it
                // (Y on a slot the pad has focused does too)
                if ((ImGui::IsItemClicked(ImGuiMouseButton_Right) || (g_pad.y && ImGui::IsItemFocused())) &&
                    g_rebind < 0 && !g_rebind_settle)
                    g_keys[player][r.action][s] = 0;
                ImGui::PopID();
            }
            ImGui::SameLine(0, spacing);
            if (g_rebind == r.action) {
                // waiting: Reset becomes Cancel, which PollRebind honours when the left click lands on it
                ImGui::Button(T("Cancel"), ImVec2(reset, 0));
                g_cancel_hovered = ImGui::IsItemHovered();
            } else {
                ImGui::BeginDisabled(IsDefault(player, r.action));
                if (ImGui::Button(T("Reset"), ImVec2(reset, 0))) ResetKey(player, r.action);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(T("default: %s"), ActionDefault(r.action).c_str());
            }
            ImGui::PopID();
        }
        if (g.rows[0].action == kMoveUp) {  // the Walk and Creep tilts, under the movement keys
            ImGui::Spacing();
            RangeSlider("Creep / Walk %", &g_creep_percent[player], &g_walk_percent[player], kTiltMin, kTiltMax, kTiltGap,
                        kDefaultCreep, kDefaultWalk);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ImGui::Button(T("Default keys"))) {
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
    ImGui::SeparatorText(T("Current level"));
    ImGui::Text("%d %s", current, current_name ? current_name : "");
    const std::string status = warp::Status();
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

    ImGui::SeparatorText(T("Checkpoints"));
    Hint("Warning: these start a new game. Don't save afterwards over a save you want to keep.");
    ImGui::InputTextWithHint("##filter", T("filter by name or level"), filter, sizeof filter);
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
                // a second A on the same row soon after the first is a double click
                static int tapped = -1;
                static double tapped_at = 0.0;
                bool again = ImGui::IsMouseDoubleClicked(0);
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
                    const double now = ImGui::GetTime();
                    again = again || (tapped == i && now - tapped_at < 0.5);
                    tapped = i, tapped_at = now;
                }
                if (again && warp::GoCheckpoint(i)) Close();
            }
        }
        ImGui::EndListBox();
    }
    ImGui::BeginDisabled(checkpoint < 0);
    if (ImGui::Button(T("Start at checkpoint")) && warp::GoCheckpoint(checkpoint)) Close();
    ImGui::EndDisabled();
    Hint("Or double-click one. Earlier checkpoints count as completed.");

    // the levels that exist, for the combos
    std::string items;
    std::vector<int> ids;
    for (int i = 0; i < warp::kLevels; i++)
        if (const char* n = warp::LevelName(i)) {
            items += std::to_string(i) + " " + n + '\0';
            ids.push_back(i);
        }
    items += '\0';
    auto level_combo = [&](const char* label, const char* id, int* v) {
        int idx = int(std::find(ids.begin(), ids.end(), *v) - ids.begin());
        if (idx == int(ids.size())) idx = 0;
        ImGui::Combo((std::string(T(label)) + "##" + id).c_str(), &idx, items.c_str());
        *v = ids[size_t(idx)];
    };

    ImGui::SeparatorText(T("Level"));
    level_combo("Level", "level", &level);
    ImGui::InputInt(T("Spawn"), &spawn);
    spawn = std::clamp(spawn, 0, 255);
    if (ImGui::Button(T("Load level")) && warp::GoLevel(level, spawn)) Close();
    Hint("Starts with no progress.");

    ImGui::SeparatorText(T("Cutscenes"));
    int found_level = -1;
    const std::vector<int> found = warp::Cutscenes(&found_level);
    if (found_level >= 0) {
        const char* fn = warp::LevelName(found_level);
        ImGui::Text(T("In %s:"), fn ? fn : T("this level"));
        if (found.empty()) ImGui::TextDisabled("%s", T("none"));
        for (size_t i = 0; i < found.size(); i++) {
            if (i % 10) ImGui::SameLine();
            if (ImGui::Button(std::to_string(found[i]).c_str()) && warp::PlayCutscene(found_level, found[i])) Close();
        }
    }
    level_combo("Level", "cut", &cut_level);
    ImGui::InputInt(T("Cutscene"), &cut_index);
    cut_index = std::clamp(cut_index, 0, 255);
    if (ImGui::Button(T("Play cutscene")) && warp::PlayCutscene(cut_level, cut_index)) Close();
    Hint("Loads the level first. Nothing plays if that level doesn't have it.");
}

// the pads' buttons the menu reads, one port's or all the ones that drive it together
struct Buttons {
    bool a = false, b = false, y = false, start = false, back = false;
    bool lb = false, rb = false, lt = false, rt = false;
    bool up = false, down = false, left = false, right = false;
    float lx = 0.0f, ly = 0.0f, ry = 0.0f;  // sticks, -1 to 1, up positive, past a dead zone

    void Add(const Buttons& o) {
        a |= o.a, b |= o.b, y |= o.y, start |= o.start, back |= o.back;
        lb |= o.lb, rb |= o.rb, lt |= o.lt, rt |= o.rt;
        up |= o.up, down |= o.down, left |= o.left, right |= o.right;
        if (std::fabs(o.lx) > std::fabs(lx)) lx = o.lx;
        if (std::fabs(o.ly) > std::fabs(ly)) ly = o.ly;
        if (std::fabs(o.ry) > std::fabs(ry)) ry = o.ry;
    }
};

float Stick(int16_t v) {
    constexpr float kDead = 0.25f;
    const float a = (std::fabs(float(v)) / 32768.0f - kDead) / (1.0f - kDead);
    return a <= 0.0f ? 0.0f : std::copysign(std::min(a, 1.0f), float(v));
}

Buttons ReadPad(int port) {
    Buttons b;
    const input::Pad p = input::Raw(port);
    if (!p.connected) return b;
    b.up = p.buttons & (1 << 0), b.down = p.buttons & (1 << 1), b.left = p.buttons & (1 << 2);
    b.right = p.buttons & (1 << 3), b.start = p.buttons & (1 << 4), b.back = p.buttons & (1 << 5);
    b.a = p.analog[0] > 0x40, b.b = p.analog[1] > 0x40, b.y = p.analog[3] > 0x40;
    b.rb = p.analog[4] > 0x40, b.lb = p.analog[5] > 0x40;  // black and white are the right and left bumpers
    b.lt = p.analog[6] > 0x80, b.rt = p.analog[7] > 0x80;
    b.lx = Stick(p.lx), b.ly = Stick(p.ly), b.ry = Stick(p.ry);
    return b;
}

// the menu or prompt is up and an item holds the keyboard or the pad: B shuts nothing then
bool Busy() {
    // the cursor is in a child window of its own (a list box), or the menu layer: B steps out of it first
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* nav = g.NavWindow;
    const bool inside = nav && ((nav != nav->RootWindow && !(nav->RootWindowForNav->Flags & ImGuiWindowFlags_Popup) &&
                                 nav->RootWindowForNav->ParentWindow) ||
                                g.NavLayer != ImGuiNavLayer_Main);
    return g_rebind >= 0 || inside || ImGui::IsAnyItemActive() ||
           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// the pads, once a frame, before the menu draws. Start with Back opens or shuts the menu from any pad, and while the
// menu or the prompt is up the pads drive ImGui's navigation: the left stick and d-pad move, A activates, B backs
// out, LB and RB turn the tabs, the right stick scrolls the page. (ImGui's backends read
// no pads: the game is held, and X, which opens ImGui's window switcher, and the shoulders, which tweak, stay ours.)
// In a match, which runs on, only the pad that opened the menu drives it.
void PadFrame(bool open, bool quit) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const bool shown = open || quit;
    const bool match = warp::Multiplayer();
    const int owner = g_owner.load();

    Buttons drive;
    bool combo = false;
    int combo_port = -1;
    for (int port = 0; port < input::kPorts; port++) {
        const Buttons b = ReadPad(port);
        if (b.start && b.back) combo = true, combo_port = port;
        if (shown && (!match || port == owner)) drive.Add(b);
    }

    static Buttons last;
    static bool was_shown = false, was_combo = false, fed = false;
    static double toggled = -1.0;
    const double now = ImGui::GetTime();
    g_pad = PadEdges{};
    if (shown && !was_shown) last = drive;  // the buttons held as it opened are not presses
    was_shown = shown;
    auto pressed = [&](bool Buttons::*button) { return drive.*button && !(last.*button); };

    // Start with Back; a second after Start shut it alone does not open it again
    const bool combo_pressed = combo && !was_combo;
    was_combo = combo;
    if (combo_pressed && now - toggled > 0.3) {
        toggled = now;
        if (open) Close();
        else ui::Show(ui::Tab::Last, combo_port);
        last = drive;
        return;
    }
    // Start alone shuts the menu or the prompt
    if (shown && pressed(&Buttons::start)) {
        toggled = now;
        if (open) Close();
        else SetQuit(false);
        last = drive;
        return;
    }

    g_pad.b = pressed(&Buttons::b);
    g_pad.y = pressed(&Buttons::y);
    if (open) {
        if (g_pad.b && !Busy()) {  // B with nothing to back out of
            Close();
            last = drive;
            return;
        }
        const int step = (pressed(&Buttons::rb) ? 1 : 0) - (pressed(&Buttons::lb) ? 1 : 0);
        if (step)
            g_tab_target.store((g_tab_now + kMainTabs + step) % kMainTabs);
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            g_pad.scroll = -drive.ry * std::fabs(drive.ry) * 1100.0f * (ImGui::GetFontSize() / 13.0f) * io.DeltaTime;
    }
    last = drive;

    if (!shown && !fed) return;
    fed = shown;
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, drive.a);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, drive.b);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, drive.y);
    // the left stick presses the d-pad's keys (ImGui moves the cursor with those alone, and repeats them as they are
    // held): past half way along the axis it leans on more, and let go again at a third
    static bool stick_up = false, stick_down = false, stick_left = false, stick_right = false;
    auto lean = [](float v, bool& on, bool along) {
        on = along && v > (on ? 0.33f : 0.5f);
        return on;
    };
    const bool horizontal = std::fabs(drive.lx) > std::fabs(drive.ly);
    lean(-drive.lx, stick_left, horizontal), lean(drive.lx, stick_right, horizontal);
    lean(drive.ly, stick_up, !horizontal), lean(-drive.ly, stick_down, !horizontal);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, drive.up || stick_up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, drive.down || stick_down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, drive.left || stick_left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, drive.right || stick_right);
}

// the quit prompt's body. Cancel is where the pad starts, and B is Cancel too (ImGui's own back out leaves a modal up)
bool QuitBody() {
    ImGui::TextUnformatted(T("Quit the game? Progress since the last save is lost."));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.8f));
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 2.0f, st.FramePadding.y * 1.5f));
    if (ImGui::Button(T("Quit"))) app::Quit();  // only posts: the window thread closes
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel"));
    ImGui::SetItemDefaultFocus();
    ImGui::PopStyleVar();
    return !cancel && !g_pad.b;
}

}  // namespace

void Draw(double fps) {
    static bool was_open = false;
    static double last_fps = -1.0;
    static bool last_title = !g_fps_title;
    static int last_look = -1;
    app::UiAccent(int(g_accent));
    PadFrame(g_open.load(), g_quit.load());
    const bool open = g_open.load();
    const bool quit = g_quit.load();
    if (!open) g_rebind = -1;  // closing the menu cancels a rebind
    if ((open || quit) && kernel::Held() == warp::Multiplayer()) kernel::Hold(!warp::Multiplayer());

    // the menu saves itself: once its settings have held still for kSaveDelay (not every frame of a slider's drag),
    // and as it shuts
    static std::string seen;
    static double seen_at = 0.0;
    bool settings_changed = false;
    if (open) {
        std::string now = app::SettingsText();
        const double t = ImGui::GetTime();
        if (now != seen) settings_changed = was_open, seen = std::move(now), seen_at = t;
        else if (t - seen_at >= kSaveDelay) app::SaveChanged();
    } else if (was_open) {
        app::SaveChanged();
    }
    const bool opened_before = was_open;
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
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x * 0.45f,
                                                                ImGui::GetStyle().WindowPadding.y * 0.35f));
        if (ImGui::Begin("##fps", nullptr, flags))
            ImGui::Text("%.0f FPS (%.2f ms)", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // F7's freeze (gpu::g_freeze): a note at the top's middle while the game waits on its last frame
    if (gpu::g_freeze.load()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const char* label = "Frozen: F7 resumes, F8 steps a frame";
        const float font = ImGui::GetFontSize();
        const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, label);
        const float pad = font * 0.5f, x = (ImGui::GetIO().DisplaySize.x - text.x) * 0.5f, y = font * 0.6f;
        dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.5f), ImVec2(x + text.x + pad, y + text.y + pad * 0.5f),
                          IM_COL32(0, 0, 0, 150), font * 0.4f);
        dl->AddText(ImGui::GetFont(), font, ImVec2(x, y), IM_COL32(255, 255, 255, 235), label);
    }

    // a cutscene's hold to skip: a pill at the bottom right whose border fills as the button is held
    if (const float hold = cutscenes::Hold(); hold > 0.0f && !open && !quit) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float unit = ImGui::GetFontSize() * 1.35f;  // the widget and its text, larger than the menu's
        const char* label = T("Hold to skip");
        const float font = unit;
        const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, label);
        const float h = unit * 2.4f, w = text.x + h * 1.1f, r = h * 0.5f, margin = unit * 1.6f, thick = unit * 0.2f;
        const ImVec2 c(display.x - margin - w * 0.5f, display.y - margin - h * 0.5f);
        const ImVec2 lo(c.x - w * 0.5f, c.y - h * 0.5f), hi(c.x + w * 0.5f, c.y + h * 0.5f);
        dl->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 150), r);
        dl->AddText(ImGui::GetFont(), font, ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f),
                    IM_COL32(255, 255, 255, 235), label);
        // along the outline from the top's middle, clockwise: half the top, the right cap, the bottom, the left cap,
        // half the top
        const float s = w - h, pi = 3.14159265f, cap = pi * r, total = 2.0f * s + 2.0f * cap;
        auto at = [&](float t) {
            if (t < s * 0.5f) return ImVec2(c.x + t, lo.y);
            t -= s * 0.5f;
            if (t < cap) {
                const float a = -pi * 0.5f + t / r;
                return ImVec2(c.x + s * 0.5f + r * std::cos(a), c.y + r * std::sin(a));
            }
            t -= cap;
            if (t < s) return ImVec2(c.x + s * 0.5f - t, hi.y);
            t -= s;
            if (t < cap) {
                const float a = pi * 0.5f + t / r;
                return ImVec2(c.x - s * 0.5f + r * std::cos(a), c.y + r * std::sin(a));
            }
            t -= cap;
            return ImVec2(c.x - s * 0.5f + t, lo.y);
        };
        // the track and the fill walk the same outline, so they overlap exactly
        auto stroke = [&](float length, ImU32 color) {
            const int steps = std::max(2, int(length / 2.0f));
            std::vector<ImVec2> pts;
            pts.reserve(steps + 1);
            for (int i = 0; i <= steps; i++) pts.push_back(at(length * float(i) / float(steps)));
            dl->AddPolyline(pts.data(), int(pts.size()), color, ImDrawFlags_None, thick);
        };
        stroke(total, IM_COL32(255, 255, 255, 60));
        stroke(total * std::min(hold, 1.0f), ImGui::GetColorU32(ImGuiCol_CheckMark));
    }

    // the prompt alone: a modal over the game, which is held. It goes when Cancel, B or Esc shut it
    if (quit && !open) {
        if (!ImGui::IsPopupOpen("Quit?")) ImGui::OpenPopup("Quit?");
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        bool up = false;
        if (ImGui::BeginPopupModal("Quit?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            up = QuitBody();
            if (!up) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // the prompt sounds like the menu
        static bool was_quit = false;
        if (!open) Cues(was_quit, false);
        was_quit = up;
        if (!up) {
            SetQuit(false);
            app::Post(mouse::kRefresh);
        }
    }
    if (!open) return;

    app::Settings& s = app::Get();
    // the whole display but for a margin; the mouse can still reach every edge. Resizable (Debug): it starts there,
    // then stays where it is dragged or sized to (by its edges, or the empty space in it), for the session
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetFontSize() / 13.0f;  // the minimums were set with ImGui's 13 px font
    const float margin = std::max(std::round(std::min(display.x, display.y) * 0.04f), 12.0f * scale);
    const ImGuiCond place = g_menu_resizable ? ImGuiCond_Once : ImGuiCond_Always;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), place);
    ImGui::SetNextWindowSize(ImVec2(std::max(display.x - margin * 2.0f, 1.0f), std::max(display.y - margin * 2.0f, 1.0f)),
                             place);
    if (g_menu_resizable) ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * scale, 200.0f * scale), display);
    if (!was_open) ImGui::SetNextWindowFocus();
    // the menu fades in with the blur behind it (vulkan.cpp Redisplay: the same time and curve)
    static double opened_at = 0.0;
    if (!opened_before) opened_at = ImGui::GetTime();
    float fade = std::min(float((ImGui::GetTime() - opened_at) / 0.11), 1.0f);
    fade = fade * fade * (3.0f - 2.0f * fade);
    g_fade = fade;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    bool keep_open = true;
    const ImGuiWindowFlags fixed = g_menu_resizable ? 0 : ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("Settings", &keep_open,
                     fixed | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoSavedSettings)) {
        // roomier rows, so the pad's cursor is easy to follow and a thumb-sized target is easy to hit
        const ImGuiStyle& base = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(base.FramePadding.x * 1.25f, base.FramePadding.y * 1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(base.ItemSpacing.x, base.ItemSpacing.y * 1.25f));
        bool keys_shown = false;
        const float row_y = ImGui::GetCursorPosY();
        if (ImGui::BeginTabBar("##tabs")) {
            // each tab scrolls on its own above the footer line
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
            const int target = g_tab_target.exchange(-1);
            int index = 0;
            // the tabs' pills: the selected one's, and the hovered or pad-focused one's, drawn together so that a tab that
            // is being picked has the accent the frame it is picked and the one before it lets go (ImGui moves the
            // selection a frame later)
            struct Pill {
                ImVec2 lo, hi;
                const char* name;
                bool hot;
            } pills[kMainTabs] = {};
            int shown = 0, pending = -1;
            auto tab = [&](const char* name) {
                const int mine = index++;
                // the pad's cursor is the tab's highlight alone, no border (the tab draws its cursor as it begins)
                ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, mine == kQuitTab ? kDanger : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                const std::string label = std::string(T(name)) + "##" + name;  // the English name is the ID
                const bool selected =
                    ImGui::BeginTabItem(label.c_str(), nullptr, target == mine ? ImGuiTabItemFlags_SetSelected : 0);
                ImGui::PopStyleColor(2);
                // ImGui picks the first item to hold the cursor from the first press of a direction, which then moves
                // nothing: the open tab holds it from the start. Only while this window has the focus: with none (a
                // click on no window leaves ImGui focusing nothing) SetNavID writes through a null NavWindow
                ImGuiContext& g = *ImGui::GetCurrentContext();
                if (selected && g.NavId == 0 && g.NavWindow && g.NavWindow->RootWindow == g.CurrentWindow->RootWindow) {
                    const ImVec2 origin = g.CurrentWindow->Pos;
                    ImGui::SetNavID(ImGui::GetItemID(), g.NavLayer, g.CurrentFocusScopeId,
                                    ImRect(ImGui::GetItemRectMin().x - origin.x, ImGui::GetItemRectMin().y - origin.y,
                                           ImGui::GetItemRectMax().x - origin.x, ImGui::GetItemRectMax().y - origin.y));
                }
                const bool arrived = Arrived(g_main_focus) && !selected;
                if (arrived) g_tab_target.store(mine), g_tab_arrived = true;
                // drawn once the tabs are all in (below), when the one that is becoming selected is known
                pills[mine] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), T(name),
                               ImGui::IsItemHovered()};
                if (selected) shown = mine;
                if (!selected && (arrived || target == mine || ImGui::IsItemClicked())) pending = mine;
                if (!selected) return false;
                g_tab_now = mine;
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.6f));  // air between the tabs and the page
                // a page fades in as its tab is picked (the one before is already gone: only the shown tab is submitted)
                static int faded_tab = -1;
                static double faded_at = 0.0;
                if (faded_tab != mine) faded_tab = mine, faded_at = ImGui::GetTime();
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(float((ImGui::GetTime() - faded_at) / 0.15), 1.0f) * g_fade);
                ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
                if (g_pad.scroll != 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() + g_pad.scroll);
                return true;
            };
            auto end_tab = [] {
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.5f));  // space below the last row when scrolled to the end
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::EndTabItem();
            };

            if (tab("General")) {
                ImGui::SeparatorText(T("Language"));
                {
                    // the languages the game has text for (aid/text), in XC_LANGUAGE order; the menu has the same six
                    static const char* const kKeys[] = {"english", "japanese", "german", "french", "spanish", "italian"};
                    constexpr int kCount = int(std::size(kKeys));
                    constexpr const char* kNames = "English\0日本語\0Deutsch\0Français\0Español\0Italiano\0";
                    auto pick = [&](const char* label, std::string& setting) {
                        int lang = 0;
                        for (int i = 0; i < kCount; i++)
                            if (setting == kKeys[i]) lang = i;
                        if (ImGui::Combo(Beside(label), &lang, kNames)) setting = kKeys[lang];
                    };
                    pick("UI", s.ui_language);
                    pick("Game text", s.language);
                    Hint("Text only, requires restart. Voices stay in English.");
                }
                ImGui::SeparatorText(T("Appearance"));
                {
                    ImGui::AlignTextToFramePadding();
                    const float x0 = ImGui::GetCursorScreenPos().x, label_width = LabelWidth();
                    ImGui::TextUnformatted(T("UI accent color"));
                    const float size = ImGui::GetFrameHeight() * 1.1f, gap = size * 0.4f;
                    for (uint32_t i = 0; i < 7; i++) {
                        if (i) ImGui::SameLine(0.0f, gap);
                        else ToValueColumn(x0, label_width);
                        ImGui::PushID(int(i));
                        float rgb[3];
                        app::UiAccentColor(int(i), rgb);
                        const ImVec4 color(rgb[0], rgb[1], rgb[2], 1.0f);
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        if (ImGui::InvisibleButton("##swatch", ImVec2(size, size))) g_accent = i;
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(color), size * 0.25f);
                        if (g_accent == i)
                            dl->AddRect(ImVec2(p.x - 2.5f, p.y - 2.5f), ImVec2(p.x + size + 2.5f, p.y + size + 2.5f),
                                        IM_COL32(255, 255, 255, 235), size * 0.25f + 2.5f, 0, 3.0f);
                        else if (ImGui::IsItemHovered())
                            dl->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(255, 255, 255, 120), size * 0.25f, 0, 1.5f);
                        ImGui::PopID();
                    }
                    Hint("The color of the selection and the sliders in this menu.");
                }
                end_tab();
            }

            if (tab("Graphics")) {
                ImGui::SeparatorText(T("Display"));
                int mode = int(s.display_mode);
                if (ImGui::Combo(Beside("Mode"), &mode, Items({"Fullscreen", "Borderless", "Windowed"}).c_str())) {
                    s.display_mode = app::DisplayMode(mode);
                    app::ApplyDisplay();
                }

                const std::vector<app::Mode>& modes = Modes();
                std::string res = T("Desktop");
                int res_idx = 0;
                for (size_t i = 0; i < modes.size(); i++) {
                    res += '\0';
                    res += std::to_string(modes[i].width) + "x" + std::to_string(modes[i].height);
                    if (modes[i].width == s.width && modes[i].height == s.height) res_idx = int(i) + 1;
                }
                res += '\0';
                if (ImGui::Combo(Beside("Resolution"), &res_idx, res.c_str())) {
                    if (res_idx == 0) s.width = s.height = 0;
                    else s.width = modes[res_idx - 1].width, s.height = modes[res_idx - 1].height;
                    app::ApplyDisplay();
                }
                // the camera follows the setting live; only the TV type the console reports waits for a restart
                const char* aspects[] = {"auto", "4:3", "16:10", "16:9", "21:9", "32:9"};
                constexpr int kAspects = 6;
                int aspect = -1;
                for (int i = 0; i < kAspects; i++)
                    if (s.aspect == aspects[i]) aspect = i;
                std::string items;
                for (int i = 0; i < kAspects; i++) items += std::string(i ? aspects[i] : T("auto")) + '\0';
                if (aspect < 0) items += s.aspect + '\0', aspect = kAspects;  // a custom W:H from settings.toml
                items += '\0';
                if (ImGui::Combo(Beside("Aspect ratio"), &aspect, items.c_str()) && aspect < kAspects) s.aspect = aspects[aspect];

                {
                    const std::vector<std::string>& adapters = gpu::Adapters();
                    if (adapters.size() > 1 || !s.gpu.empty()) {
                        if (ImGui::BeginCombo(Beside("Graphics adapter"), s.gpu.empty() ? T("Automatic") : s.gpu.c_str())) {
                            if (ImGui::Selectable(T("Automatic"), s.gpu.empty())) s.gpu.clear();
                            for (const std::string& a : adapters)
                                if (ImGui::Selectable(a.c_str(), a == s.gpu)) s.gpu = a;
                            ImGui::EndCombo();
                        }
                    }
                }

                ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));  // set apart from the dropdown above
                Check("V-Sync", &s.vsync);
                Hint("Waits for the display to refresh before showing each frame. Stops screen tearing; turn it off for the lowest latency.", true);

                ImGui::SeparatorText(T("Quality"));
                Slider("Render %", &s.render_percent, 25, 200, app::Settings{}.render_percent);
                Check("AMD FSR 1.0", &s.fsr);
                Hint("AMD FidelityFX Super Resolution: edge-adaptive spatial upscaling & contrast-adaptive sharpening.", true);
                if (s.fsr) {
                    Slider("FSR sharpness %", &s.fsr_sharpness, 0, 100, app::Settings{}.fsr_sharpness);
                    Slider("Fur sharpness %", &s.fur_sharpness, 0, 100, app::Settings{}.fur_sharpness);
                    Hint("How much of FSR's sharpening fur takes. 0 keeps it soft.", true);
                }
                constexpr uint32_t kRates[] = {30, 60, 120, 180, 240};
                int rate = 0;
                for (int i = 0; i < 5; i++)
                    if (kRates[i] == display::g_frame_rate) rate = i;
                if (ImGui::Combo(Beside("Frame rate"), &rate, Items({"30 (original)", " 60", " 120", " 180", " 240"}).c_str()))
                    display::SetFrameRate(kRates[rate]);

                constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
                int aniso = 0;
                for (int i = 0; i < 5; i++)
                    if (kAniso[i] == s.aniso) aniso = i;
                if (ImGui::Combo(Beside("Anisotropic"), &aniso, Items({"Game default", " 2x", " 4x", " 8x", " 16x"}).c_str()))
                    s.aniso = kAniso[aniso];

                // one choice: no anti-aliasing, SMAA, or MSAA at a sample count (the two never run together)
                constexpr uint32_t kAa[] = {0, 1, 2, 4, 8};  // 0 off, 1 SMAA, else MSAA samples
                int aa = s.smaa ? 1 : s.msaa >= 8 ? 4 : s.msaa >= 4 ? 3 : s.msaa >= 2 ? 2 : 0;
                if (ImGui::Combo(Beside("Anti-aliasing"), &aa, Items({"Off", "SMAA", "MSAA 2x", "MSAA 4x", "MSAA 8x"}).c_str())) {
                    s.smaa = aa == 1;
                    s.msaa = aa >= 2 ? kAa[aa] : 1;
                }

                ImGui::SeparatorText(T("Post-processing"));
                Check("Vignette", &vignette::g_on);
                Hint("The game's darkened screen edges.", true);
                Slider("Film grain %", &s.film_grain, 0, 100, app::Settings{}.film_grain);
                Slider("Chromatic aberration %", &s.chromatic_aberration, 0, 100, app::Settings{}.chromatic_aberration);
                Slider("Ambient occlusion %", &s.ambient_occlusion, 0, 100, app::Settings{}.ambient_occlusion);
                if (s.ambient_occlusion)
                    Slider("Ambient occlusion reach", &s.ambient_occlusion_radius, 5, 200,
                           app::Settings{}.ambient_occlusion_radius);
                end_tab();
            }

            if (tab("Enhancements")) {
                ImGui::SeparatorText(T("Camera"));
                Slider("Field of view", &display::g_fov, display::kFovMin, display::kFovMax, display::kFov);

                ImGui::SeparatorText(T("Lighting"));
                Check("Per-pixel lighting", &lighting::g_per_pixel);
                Hint("Smoother light and shadows on characters and objects.", true);
                bool no_fog = !s.fog;
                if (Check("Disable fog", &no_fog)) s.fog = !no_fog;
                Hint("Clears the haze over faraway scenery. Enabling this distorts colors pretty much everywhere "
                     "in the game, leaving it off is recommended.", true);

                ImGui::SeparatorText(T("Fur"));
                // one of the two at most, kept in settings.toml as fur_shading and fur_strands: super fur lights its
                // strands its own way (and takes the draws first anyway)
                int fur_look = fur::g_strands ? 2 : fur::g_shading ? 1 : 0;
                if (ImGui::Combo(Beside("Fur shading"), &fur_look, Items({"None", "Simple", "Super"}).c_str())) {
                    fur::g_shading = fur_look == 1;
                    fur::g_strands = fur_look == 2;
                }
                if (fur::g_strands) fur::g_shading = false;
                Hint("Simple makes fur look more voluminous and improves its shading. Super draws it as fine, tapered "
                     "strands in tufts, lit as hair.");
                if (fur::g_strands) {
                    // the model whose look the sliders change: the furry models the game's table names, those
                    // super fur has drawn, and those with a look saved
                    static uint32_t model = fur::kNoModel;
                    std::vector<uint32_t> models = fur::KnownModels();
                    const auto add = [&](uint32_t type) {
                        if (type != fur::kNoModel && !fur::Furless(type) &&
                            std::find(models.begin(), models.end(), type) == models.end())
                            models.push_back(type);
                    };
                    for (uint32_t type : fur::SeenModels()) add(fur::Canonical(type));  // a multiplayer class once
                    for (const fur::ModelLook& m : fur::g_model_looks) add(m.type);
                    std::vector<std::pair<std::string, uint32_t>> named;
                    for (uint32_t type : models) named.emplace_back(fur::ModelName(type), type);
                    std::sort(named.begin(), named.end());
                    if (std::none_of(named.begin(), named.end(), [](const auto& n) { return n.second == model; }))
                        model = named.empty() ? fur::kNoModel : named.front().second;
                    const auto changed = [](uint32_t type) {
                        const fur::ModelLook* m = fur::FindModelLook(type);
                        return m && (m->look != fur::DefaultLook(type) || m->game != fur::GameFurOf(type) ||
                                     m->shells != fur::DefaultShells(type));
                    };
                    const auto label = [&](const std::string& name, uint32_t type) {
                        return name + (changed(type) ? " *" : "") + "##" + std::to_string(type);
                    };
                    if (named.empty()) {
                        Hint("The game's models show up here once it has started.");
                    } else {
                        std::string current;
                        for (const auto& [name, type] : named)
                            if (type == model) current = label(name, type);
                        if (ImGui::BeginCombo(Beside("Model"), current.c_str())) {
                            for (const auto& [name, type] : named)
                                if (ImGui::Selectable(label(name, type).c_str(), type == model)) model = type;
                            ImGui::EndCombo();
                        }
                        Hint("Each model has its own settings; those changed are marked *. Multiplayer models are "
                             "listed once the game has loaded them.", true);
                    }
                    if (fur::ModelLook* own = fur::ModelLookOf(model)) {
                        const fur::Look& d = fur::DefaultLook(model);
                        fur::Look& l = own->look;
                        ImGui::PushID(int(model));  // each model's sliders their own widgets, so a drag doesn't carry over
                        Check("Game's own fur", &own->game);
                        if (fur::IsHair(model))
                            Hint("The hair keeps the game's fur, with super fur on; otherwise it has a look of its own.", true);
                        else
                            Hint("This model keeps the game's fur, with super fur on.", true);
                        if (fur::IsHair(model)) {  // its motion, whichever fur it has
                            Slider("Sway %", &l.sway, 0u, 200u, d.sway);
                            Hint("How much the hair moves in the air.", true);
                            Slider("Follows motion %", &l.motion, 0u, 200u, d.motion);
                            Hint("How far the hair trails behind when the character moves or turns, and swings back "
                                 "as it stops.", true);
                        }
                        // the look in two columns, a pair to a row: each slider in the next cell
                        if (!own->game && ImGui::BeginTable("##look", 2, ImGuiTableFlags_SizingStretchSame)) {
                            const auto cell = [](auto&&... args) {
                                ImGui::TableNextColumn();
                                Slider(args...);
                            };
                            cell("Extra shells", &own->shells, 0u, fur::kMaxExtraShells, fur::DefaultShells(model));
                            cell("Length %", &l.length, 20u, 100u, d.length);
                            cell("Strands", &l.strands, 8u, 64u, d.strands);
                            cell("Tufts", &l.tufts, 1u, 16u, d.tufts);
                            cell("Tufting %", &l.tufting, 0u, 100u, d.tufting);
                            cell("Droop %", &l.droop, 0u, 200u, d.droop);
                            cell("Thickness %", &l.thickness, 30u, 100u, d.thickness);
                            cell("Taper %", &l.taper, 20u, 150u, d.taper);
                            cell("Root shade %", &l.root_shade, 0u, fur::kMaxShade, d.root_shade);
                            cell("Tuft shade %", &l.gap_shade, 0u, fur::kMaxShade, d.gap_shade);
                            cell("Sheen %", &l.sheen, 0u, 200u, d.sheen);
                            cell("Back glow %", &l.back_glow, 0u, 200u, d.back_glow);
                            cell("Bare skin fill %", &l.fill, 0u, 100u, d.fill);
                            cell("Fur brightness %", &l.tone, 50u, 200u, d.tone);
                            cell("Strays %", &l.stray, 0u, 30u, d.stray);
                            cell("Stray length %", &l.stray_length, 100u, 250u, d.stray_length);
                            ImGui::EndTable();
                        }
                        ImGui::PopID();
                    }
                }
                Check("Full distant fur", &fur::g_distant);
                Hint("Keeps fur on far-away characters.", true);

                ImGui::SeparatorText(T("Characters"));
                Check("Force max character LoD", &characters::g_max_lod);
                Hint("Characters keep their full model and smooth animation however far away.", true);
                Check("Detailed multiplayer characters", &characters::g_menu_models);
                Hint("Multiplayer classes use their more detailed character select models in matches; squirrels get "
                     "fur on the body. Takes effect from the next match.", true);

                ImGui::SeparatorText(T("Grass"));
                Check("Wind on distant grass", &grass::g_wind);
                Hint("Faraway grass sways in the wind too.", true);
                Check("Force max grass LoD", &grass::g_max_lod);
                Hint("Faraway grass stays as thick as nearby grass instead of thinning out.", true);
                bool solid = !grass::g_fade;  // the setting is the console's fade, which this checkbox turns off
                if (Check("Don't fade distant grass", &solid)) grass::g_fade = !solid;
                Hint("Faraway grass stays solid, however far, instead of fading away as on the console.", true);

                ImGui::SeparatorText(T("Water and reflections"));
                Check("Dynamic water", &water::g_dynamic);
                Hint("Water ripples and makes waves as things move through it.", true);
                Check("Floor depth of field", &reflection::g_floor_depth);
                Hint("Fixes the blur on shiny, reflective floors.", true);

                ImGui::SeparatorText(T("HUD"));
                Check("Unstretched multiplayer HUD", &display::g_hud_split);
                Hint("On a widescreen display, the HUD in a multiplayer match keeps its shape, each part at the left, "
                     "the middle or the right of the screen. Off: it stretches across the screen.", true);

                ImGui::SeparatorText(T("Menus"));
                Check("Readable menu hints", &menus::g_bright_hints);
                Hint("The hints above and below the multiplayer menus (R Configure) show as clearly as the menu's "
                     "title. Off: faint and flickering, as on the console.", true);
                bool no_attract = !menus::g_attract;
                if (Check("Disable attract video", &no_attract)) menus::g_attract = !no_attract;
                Hint("The main menu no longer plays its video after two minutes without input.", true);

                ImGui::SeparatorText(T("Keyboard and mouse"));
                Check("Keyboard button prompts", &prompts::g_on);
                Hint("While you play with the keyboard and mouse, the game's button icons show the keys they are "
                     "bound to. Off: the controller's buttons, as on the console.", true);

                ImGui::SeparatorText(T("Gameplay"));
                Check("Skippable cutscenes", &cutscenes::g_skippable);
                Hint("Hold any button, but pause, for a second to skip any cutscene, even ones you haven't seen. Off: "
                     "LT skips the ones you have.", true);
                Check("Speech bubbles in Intro and Ending cutscenes", &cutscenes::g_english_bubbles);
                Hint("Conker's monologues on the throne show speech bubbles in English too, as they do in every other "
                     "language. Off: no bubbles there in English, as on the console.", true);
                Check("Fade instead of loading screens", &loading::g_fade);
                Hint("Loads fade to black and back instead of showing the loading screen, which also makes them much "
                     "faster.", true);
                Check("Rope physics", &rope::g_physics);
                Hint("Ropes swing realistically when Conker hangs on them.", true);
                end_tab();
            }

            if (tab("Audio")) {
                Slider("Volume %", &s.volume, 0, 100, app::Settings{}.volume);
                {
                    static std::vector<audio::Device> devices;
                    if (devices.empty() || ImGui::IsWindowAppearing()) devices = audio::Devices();
                    const std::string system_default = T("System default");
                    std::string current = system_default;
                    for (const audio::Device& d : devices)
                        if (d.id == s.audio_device) current = d.name;
                    if (!s.audio_device.empty() && current == system_default)
                        current += std::string(" ") + T("(selected device missing)");
                    if (ImGui::BeginCombo(Beside("Output device"), current.c_str())) {
                        if (ImGui::Selectable(system_default.c_str(), s.audio_device.empty())) {
                            s.audio_device.clear();
                            audio::SelectDevice(s.audio_device);
                        }
                        for (const audio::Device& d : devices)
                            if (ImGui::Selectable(d.name.c_str(), d.id == s.audio_device)) {
                                s.audio_device = d.id;
                                audio::SelectDevice(s.audio_device);
                            }
                        ImGui::EndCombo();
                    }
                }
                {
                    int speakers = s.surround ? 1 : 0;
                    if (ImGui::Combo(Beside("Speakers"), &speakers, Items({"Stereo", "5.1 surround"}).c_str())) s.surround = speakers == 1;
                    Hint("Stereo mixes for two speakers or headphones. 5.1 surround mixes for six speakers and "
                         "plays on as many as the output device has. Starts from the next launch.");
                }
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));  // set apart from the hint above
                Check("Mute in background", &s.mute_in_background);
                Hint("No sound while you are in another window.", true);
                Check("Menu sounds", &s.menu_sounds);
                Hint("The sounds of changing tabs and settings and of moving around this menu with the keyboard or a pad.", true);
                // the mixer's choices, where how the console sounds isn't settled
                ImGui::SeparatorText(T("Mix"));
                {
                    const app::Settings def{};
                    // at full size, not a dimmed hint: it is the first thing to read in this section
                    ImGui::TextWrapped("%s", T("If the game already sounds good to you, leave these as they are."));
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));
                    ImGui::BeginDisabled(s.surround);
                    int backs = s.stereo_full_backs ? 1 : 0;
                    if (ImGui::Combo(Beside("Back speakers in stereo"), &backs, Items({"-3 dB", "Full level"}).c_str()))
                        s.stereo_full_backs = backs == 1;
                    Hint("How loud the back speakers' sounds are when folded into the left and right ones.");
                    Check("Subwoofer in stereo", &s.stereo_sub);
                    Hint("Folds the subwoofer channel into both speakers.", true);
                    ImGui::EndDisabled();
                    Slider("Reverb %", &s.reverb, 0, 200, def.reverb);
                    Hint("The room echo in caves, halls and the like.");
                    int resampler = int(std::min<uint32_t>(s.resampler, 2));
                    if (ImGui::Combo(Beside("Resampling"), &resampler, Items({"Linear", "Cubic", "Sinc"}).c_str()))
                        s.resampler = uint32_t(resampler);
                    Hint("How sounds recorded at lower rates are played at 48 kHz. Cubic and Sinc are sharper than "
                         "Linear.");
                    Check("Crosstalk cancellation", &s.crosstalk);
                    Hint("Widens positioned sounds over two speakers.", true);
                    Check("Doppler effect", &s.doppler);
                    Hint("Moving sounds rise and fall in pitch as they pass.", true);
                    Check("Distance muffling", &s.distance_filters);
                    Hint("Far and blocked sounds lose their highs.", true);
                    Check("Voice headroom", &s.voice_headroom);
                    Hint("Lowers some sounds to leave room in the mix. Off plays every sound at full level, which "
                         "changes the balance between music, voices and effects.", true);
                    Check("Sound envelopes", &s.envelopes);
                    Hint("Sounds fade in, swell and fade out, and looping sounds play out their endings when they "
                         "stop.", true);
                    Check("Sound wobble (LFO)", &s.lfo);
                    Hint("Vibrato and tremolo on the sounds that ask for them.", true);
                    Check("Sound filters", &s.voice_filters);
                    Hint("Muffled or resonant sounds keep their filtering.", true);
                    Check("Soft clipping", &s.soft_clip);
                    Hint("When the mix gets too loud it is rounded off instead of cut flat, which crackles less.", true);
                    // every Mix setting back to its default; greyed out while they already are
                    const bool same = s.stereo_full_backs == def.stereo_full_backs && s.stereo_sub == def.stereo_sub &&
                                      s.reverb == def.reverb && s.crosstalk == def.crosstalk &&
                                      s.doppler == def.doppler && s.distance_filters == def.distance_filters &&
                                      s.resampler == def.resampler && s.voice_headroom == def.voice_headroom &&
                                      s.soft_clip == def.soft_clip && s.envelopes == def.envelopes &&
                                      s.lfo == def.lfo && s.voice_filters == def.voice_filters;
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));
                    ImGui::BeginDisabled(same);
                    if (ImGui::Button(T("Reset mix to defaults"))) {
                        s.stereo_full_backs = def.stereo_full_backs;
                        s.stereo_sub = def.stereo_sub;
                        s.reverb = def.reverb;
                        s.crosstalk = def.crosstalk;
                        s.doppler = def.doppler;
                        s.distance_filters = def.distance_filters;
                        s.resampler = def.resampler;
                        s.voice_headroom = def.voice_headroom;
                        s.soft_clip = def.soft_clip;
                        s.envelopes = def.envelopes;
                        s.lfo = def.lfo;
                        s.voice_filters = def.voice_filters;
                    }
                    ImGui::EndDisabled();
                }
                end_tab();
            }

            if (tab("Controls")) {
                // whose controls: a list, not a second row of tabs, so the pad's cursor goes straight down the page
                std::string players;
                for (int p = 0; p < input::kPorts; p++) {
                    char name[64];
                    std::snprintf(name, sizeof name, T("Player %d"), p + 1);
                    players += name;
                    players += '\0';
                }
                ImGui::Combo(Beside("Player"), &g_player_now, players.c_str());
                const int p = g_player_now;
                ImGui::PushID(p);
                DrawDevices(p);
                // side by side
                if (ImGui::BeginTable("##pads", 2, ImGuiTableFlags_SizingStretchSame)) {
                    ImGui::TableNextColumn();
                    DrawController(p);
                    ImGui::TableNextColumn();
                    DrawMouse(p);
                    ImGui::EndTable();
                }
                DrawKeys(p);
                keys_shown = true;
                ImGui::PopID();
                end_tab();
            }

            if (tab("Multiplayer")) {
                ImGui::SeparatorText(T("Online play"));
                // edited in a copy and set as the field is left or Enter pressed: system link resolves the address
                // each time it changes (xnet.cpp JoinAddress), and a half typed name is a DNS lookup
                static char host[256];
                if (ImGui::IsWindowAppearing()) std::snprintf(host, sizeof host, "%s", s.join_ip.c_str());
                ImGui::InputTextWithHint(Beside("Host address"), T("leave empty to host or find local games"), host, sizeof host);
                if (ImGui::IsItemDeactivatedAfterEdit()) s.join_ip = host;
                if (!app::JoinArg().empty()) ImGui::Text(T("This copy joins %s (--join)"), app::JoinArg().c_str());
                Hints({"Leave empty to host, or to find games on your home network.",
                       "For LAN hosts outside your subnet, enter their local IP.",
                       "For hosts over the internet, enter their public IP.",
                       "For hosts on a port other than 3074, append the port after the IP (1.2.3.4:3075)."});
                int port = int(s.system_link_port);
                ImGui::BeginDisabled(app::PortArg() != 0);
                if (ImGui::InputInt(Beside("Online play port"), &port, 0, 0)) s.system_link_port = uint32_t(std::clamp(port, 1, 65535));
                ImGui::EndDisabled();
                if (app::PortArg()) ImGui::Text(T("This copy uses %d (--port)"), app::PortArg());
                Hints({"UDP, 3074 as on the console.",
                       "Hosting for players past the LAN: forward it on your router.",
                       "Each extra copy on this PC needs its own: launch it with --port N.",
                       "A change starts from the next launch."});
                static std::string lan;
                static uint16_t in_use = 0;
                static int error = 0;
                if (lan.empty() || ImGui::IsWindowAppearing())
                    lan = xnet::LocalAddress(), in_use = xnet::Port(), error = xnet::PortError();
                ImGui::SeparatorText(T("This PC"));
                ImGui::Text(T("LAN IP: %s"), lan.c_str());
                if (!in_use) ImGui::Text("%s", T("Port: not open yet (it opens with Online)"));
                else if (!error) ImGui::Text(T("Port: UDP %u"), in_use);
                else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("Port: UDP %u couldn't be opened (%s)"), in_use,
                                        error == 10048 ? T("another program or copy has it") : T("blocked"));
                end_tab();
            }

            if (tab("Mods")) {
                // the folders are looked at again each time the tab opens, and on Rescan
                if (ImGui::IsWindowAppearing()) app::ScanMods();
                ImGui::SeparatorText(T("Mods"));
                std::vector<app::ModEntry>& mods = s.mods;
                if (mods.empty()) ImGui::TextDisabled("%s", T("No mods installed."));
                int move = -1, to = -1;
                const float arrow = ImGui::GetFrameHeight();
                for (int i = 0; i < int(mods.size()); i++) {
                    ImGui::PushID(mods[i].name.c_str());  // by name: the cursor goes with a mod as it moves
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move = i, to = i - 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(i + 1 == int(mods.size()));
                    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move = i, to = i + 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine(0.0f, arrow * 0.5f);
                    Check(mods[i].name.c_str(), &mods[i].enabled);
                    ImGui::PopID();
                }
                if (move >= 0) std::swap(mods[move], mods[to]);
                // the lists sit apart from the rows around them, not tucked under one setting
                const ImVec2 gap(0.0f, ImGui::GetStyle().ItemSpacing.y);
                ImGui::Dummy(gap);
                Hints({"Ticked mods are in use. One higher in the list wins where two replace the same file.",
                       "A change starts from the next launch."});
                ImGui::Dummy(gap);
                if (app::ModsChanged())
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s", T("Restart the game to use these mods."));
                ImGui::SeparatorText(T("Installing"));
                static std::string dir;
                if (dir.empty()) dir = app::ModsDir().string();
                ImGui::TextWrapped("%s", dir.c_str());
                ImGui::Dummy(gap);
                Hints({"Put each mod in its own folder here, laid out like the game's disc (dvddata\\...).",
                       "The game's own files are left as they are."});
                ImGui::Dummy(gap);
                if (ImGui::Button(T("Rescan"))) app::ScanMods();
                end_tab();
            }

            if (tab("Debug")) {
                ImGui::SeparatorText(T("FPS"));
                Check("Overlay", &g_fps_overlay);
                ImGui::SameLine();
                Check("Window title", &g_fps_title);
                ImGui::SeparatorText(T("Menu"));
                Check("Resizable menu", &g_menu_resizable);
                Hint("Drag the menu's edges to size it, or an empty part of it to move it, to see the game beside it. "
                     "F7 freezes the game on its frame, drawn again as settings change; F8 steps a frame.",
                     true);
                DrawWarp();
                end_tab();
            }
            if (tab("Quit")) {
                ImGui::TextUnformatted(T("Quit the game? Progress since the last save is lost."));
                const auto shade = [](float f) { return ImVec4(kDanger.x * f, kDanger.y * f, kDanger.z * f, 1.0f); };
                ImGui::PushStyleColor(ImGuiCol_Button, shade(0.75f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDanger);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(0.6f));
                if (ImGui::Button(T("Quit game"))) app::Quit();  // the tab is the prompt: no second one
                ImGui::PopStyleColor(3);
                end_tab();
            }
            {
                // ImGui rounds a tab's top corners alone, so its backgrounds are clear and the pills are ours, over the
                // tabs: their labels again
                if (pending >= 0) shown = pending;
                // a hover that was there as the selection changed (a bumper, a click) is not shown until the mouse moves:
                // it would sit beside the new selection as a second highlight
                static int last_shown = -1;
                static ImVec2 anchor;
                static bool hush = true;
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                if (shown != last_shown) last_shown = shown, anchor = mouse, hush = true;
                else if (hush && (mouse.x != anchor.x || mouse.y != anchor.y)) hush = false;
                const ImGuiStyle& st = ImGui::GetStyle();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < index; i++) {
                    const Pill& p = pills[i];
                    if (i != shown && !(p.hot && !hush)) continue;
                    const bool danger = i == kQuitTab;  // Quit is red, not the accent
                    ImVec4 fill = danger ? kDanger : st.Colors[ImGuiCol_CheckMark];
                    fill.w = i == shown ? 0.80f : 0.0f;
                    if (i != shown) fill = danger ? ImVec4(kDanger.x, kDanger.y, kDanger.z, 0.16f) : ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
                    dl->AddRectFilled(p.lo, p.hi, ImGui::GetColorU32(fill), (p.hi.y - p.lo.y) * 0.5f);  // a pill
                    const ImVec2 size = ImGui::CalcTextSize(p.name);
                    dl->AddText(ImVec2(std::round((p.lo.x + p.hi.x - size.x) * 0.5f), std::round((p.lo.y + p.hi.y - size.y) * 0.5f)),
                                ImGui::GetColorU32(i == kQuitTab && i != shown ? kDanger : st.Colors[ImGuiCol_Text]), p.name);
                }
            }
            ImGui::EndTabBar();

            // Close at the tab row's far right, flat like the tabs and rounded all round. B is the pad's way out: the
            // pad's cursor never lands on it
            const ImVec2 after = ImGui::GetCursorPos();
            const ImGuiStyle& st = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(T("Close")).x + st.FramePadding.x * 2.0f;
            ImGui::SetCursorPos(ImVec2(after.x + ImGui::GetContentRegionAvail().x - width, row_y));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);  // a pill like the tabs
            if (ImGui::Button(T("Close"))) Close();
            ImGui::PopStyleVar();
            ImGui::PopItemFlag();
            ImGui::PopStyleColor(3);
            ImGui::SetCursorPos(after);
        }
        if (!keys_shown) g_rebind = -1;  // leaving the Controls tab cancels a rebind

        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        // the build's version (git describe, cmake/version.cmake) at the footer line's far right; on a narrow window the
        // hint is cut short of it
        const float version_x = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(XBE_VERSION).x;
        const ImVec2 line = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(line, ImVec2(ImGui::GetWindowPos().x + version_x - ImGui::GetStyle().ItemSpacing.x * 2.0f,
                                         line.y + ImGui::GetFrameHeight()), true);
        ImGui::TextDisabled("%s", T("Stick or d-pad move, LB / RB turn the tabs, A select, B back. F1 or Start + Back open and close this menu. "
                                    "Changes are saved automatically."));
        ImGui::PopClipRect();
        ImGui::SameLine(version_x);
        ImGui::TextDisabled("%s", XBE_VERSION);
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    Cues(opened_before, settings_changed);
    g_tab_arrived = false;
    if (!keep_open) Close();  // the window's X
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case app::kMsgKeyDown:
            // not on auto repeat; the menu window and the game both take the key, the menu only when open
            // Escape cancels a rebind waiting for its key instead (PollRebind)
            // F1 opens and shuts the menu; Esc shuts it or the prompt, and otherwise is the game's Start (EscStart)
            if ((wp == kEscape || wp == kF1) && !(lp & (1 << 30)) && !(wp == kEscape && g_rebinding.load())) {
                if (wp == kF1) {
                    g_quit.store(false);
                    g_owner.store(-1);
                    SetOpen(!g_open.load());
                } else if (g_open.load() || g_quit.load()) {
                    g_esc_swallow.store(true);
                    g_quit.store(false);
                    SetOpen(false);
                } else {
                    break;
                }
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
    if (!g_open.load() && !g_quit.load()) menus::Message(msg, wp, lp);  // the on-screen keyboard's typing
    mouse::Message(hwnd, msg, wp, lp);
    return false;
}

bool Open() { return g_open.load() || g_quit.load(); }

void Show(Tab tab, int port) {
    if (tab == Tab::Controls) g_tab_target.store(kControlsTab);
    else if (tab == Tab::Multiplayer) g_tab_target.store(kMultiplayerTab);
    g_quit.store(false);
    g_owner.store(port);
    SetOpen(true);
    app::Post(mouse::kRefresh);  // the window thread lets the cursor go, as Message does
}

int Owner() { return g_owner.load(); }

void ShowQuit() {
    if (g_open.load()) return;
    SetQuit(true);
    app::Post(mouse::kRefresh);
}

// the game's pad, per port: the menu held it neutral since the game last polled; the buttons (bits 0-15) and analog
// buttons (16-23) that were held as it shut, until each is let go
bool g_muzzled[input::kPorts] = {};
uint32_t g_stuck[input::kPorts] = {};

uint32_t Pressed(const input::Pad& pad) {
    uint32_t bits = pad.buttons;
    for (int i = 0; i < 8; i++)
        if (pad.analog[i] > 0x40) bits |= 1u << (16 + i);
    return bits;
}

void Muzzle(int port) {
    if (port >= 0 && port < input::kPorts) g_muzzled[port] = true;
}

void Unmuzzle(int port, input::Pad& pad) {
    if (port < 0 || port >= input::kPorts) return;
    const uint32_t held = Pressed(pad);
    if (g_muzzled[port]) g_muzzled[port] = false, g_stuck[port] = held;
    g_stuck[port] &= held;
    if (!g_stuck[port]) return;
    pad.buttons &= uint16_t(~g_stuck[port]);
    for (int i = 0; i < 8; i++)
        if (g_stuck[port] & (1u << (16 + i))) pad.analog[i] = 0;
}

bool EscStart() {
    if (!app::KeyDown(kEscape)) {
        g_esc_swallow.store(false);
        return false;
    }
    return !g_esc_swallow.load() && !Open();
}

void SwallowEsc() { g_esc_swallow.store(true); }

}  // namespace ui

#else  // no ImGui here (the headless harness): the menu and the hook do nothing

namespace ui {

bool g_fps_overlay = false;
uint32_t g_accent = 4;  // orange
bool g_fps_title = true;
bool g_menu_resizable = false;

void Draw(double) {}

bool Message(void*, uint32_t, uintptr_t, intptr_t, intptr_t*) { return false; }

bool Open() { return false; }

void Show(Tab, int) {}

int Owner() { return -1; }

void ShowQuit() {}

bool EscStart() { return false; }
void SwallowEsc() {}

void Muzzle(int) {}

void Unmuzzle(int, input::Pad&) {}

}  // namespace ui

#endif
