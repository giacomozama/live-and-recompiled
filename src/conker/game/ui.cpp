// ImGui settings menu and fps readout, for GameInfo::ui and GameInfo::message (see game_info.cpp).
//
// The render thread draws the frame with app::UiLock held (vulkan.cpp RenderUi), and the window thread runs
// Message for every message after ImGui's. Draw must never make a blocking cross-thread call: it only posts.
// So the window title is posted (kTitle), and the window thread sets it.
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
#include <cstring>
#include <ctime>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiItemFlags_NoNav

#include "app/app.h"
#include "audio/audio.h"
#include "common/achievements.h"
#include "common/medbeam.h"
#include "common/characters.h"
#include "common/chat.h"
#include "common/subtitles.h"
#include "common/cutscenes.h"
#include "common/game_fonts.h"
#include "common/display.h"
#include "common/fur.h"
#include "common/gamepad.h"
#include "common/grass.h"
#include "common/keyboard.h"
#include "common/lighting.h"
#include "common/loading.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "common/presets.h"
#include "common/prompts.h"
#include "common/reflection.h"
#include "common/rope.h"
#include "common/vignette.h"
#include "common/hud_art.h"
#include "common/warp.h"
#include "common/water.h"
#include "gpu/shader_record.h"
#include "core/diag.h"
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
bool g_menu_resizable = false;
bool g_decorations = true;

namespace {

// the window owns the title; Draw posts this once, Message (window thread) writes it
constexpr uint32_t kTitle = app::kMsgApp + 0x40;

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
// the display calibration's step in place of the menu's window (DrawCalibration), -1 the window; Esc and B go back to it
std::atomic<int> g_calib_step{-1};

// the main tab to show next frame (Show asks for one, the bumpers turn), -1 none; the tab drawn now; the player
// the Controls tab shows
// (Debug, the one before Quit, only in a build with the debug features: core/diag.h)
constexpr int kMainTabs = diag::kDebugFeatures ? 10 : 9, kControlsTab = 4, kMultiplayerTab = 5, kQuitTab = kMainTabs - 1;
// the Debug tab's option; its saved value is ignored by a build without it
bool MenuResizable() { return diag::kDebugFeatures && g_menu_resizable; }
// the Quit tab's color
constexpr ImVec4 kDanger(0.93f, 0.33f, 0.33f, 1.0f);
// a button for what can't be undone: the danger red, its edge line brighter so it shows against the fill (Pop after it)
void PushDangerButton() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kDanger.x * 0.75f, kDanger.y * 0.75f, kDanger.z * 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kDanger.x * 0.6f, kDanger.y * 0.6f, kDanger.z * 0.6f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 48.0f / 255.0f));
}
void PopDangerButton() { ImGui::PopStyleColor(4); }
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
bool g_tab_focused = false;  // the pad's cursor is on a tab button (last frame); render thread only
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
    static bool active_keys = false, active_tweak = false;
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
    // (only one that took the arrow keys in, a slider or a field: a button, checkbox or header held down and let go of
    // gave its sound with the press)
    const bool released = g.ActiveId == 0 && active != 0 && active_keys && active_tweak && now - active_at > 0.1;
    if (g.ActiveId != active) active_at = now, active_keys = g.ActiveIdSource != ImGuiInputSource_Mouse, active_tweak = false;
    if (g.ActiveId != 0 && g.ActiveIdUsingNavDirMask) active_tweak = true;
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

// a 1px black line around a rectangle, a 1px translucent one just inside it and a soft shadow outside, faded by `fade`
void DrawFrame(ImDrawList* dl, ImVec2 lo, ImVec2 hi, float round, float fade, int rings = 16, float strength = 90.0f, float line = 255.0f, float inner = 24.0f) {
    // AddRect draws its line inside the rectangle it is given: the black one's rectangle is the rectangle grown by
    // a pixel, the white one's its own
    // a soft shadow outside the black line: rings a pixel wide, each fainter than the one inside it
    const int kShadow = rings;
    for (int k = 0; k < kShadow; k++) {
        const float t = 1.0f - float(k) / float(kShadow), grow = 2.0f + float(k);
        dl->AddRect(ImVec2(lo.x - grow, lo.y - grow), ImVec2(hi.x + grow, hi.y + grow),
                    IM_COL32(0, 0, 0, int(strength * t * t * t * fade)), round + grow, 0, 1.0f);
    }
    dl->AddRect(ImVec2(lo.x - 1.0f, lo.y - 1.0f), ImVec2(hi.x + 1.0f, hi.y + 1.0f),
                IM_COL32(0, 0, 0, int(line * fade)), round + 1.0f, 0, 1.0f);
    dl->AddRect(lo, hi, IM_COL32(255, 255, 255, int(inner * fade)), round, 0, 1.0f);
}

// the lower edge of DrawFrame's two lines (no shadow), up to where the corners turn into the sides: a header's, under
// the card's own outline
void DrawFrameBottom(ImDrawList* dl, ImVec2 lo, ImVec2 hi, float round, float fade, float line) {
    auto edge = [&](ImVec2 a, ImVec2 b, float r, ImU32 col) {  // as AddRect's line, inside the rectangle given
        a.x += 0.5f, a.y += 0.5f, b.x -= 0.5f, b.y -= 0.5f;
        r = std::min(r, (b.x - a.x) * 0.5f);
        dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0.0f, IM_PI * 0.5f);
        dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, IM_PI * 0.5f, IM_PI);
        dl->PathStroke(col, 0, 1.0f);
    };
    edge(ImVec2(lo.x - 1.0f, lo.y - 1.0f), ImVec2(hi.x + 1.0f, hi.y + 1.0f), round + 1.0f, IM_COL32(0, 0, 0, int(line * fade)));
    edge(lo, hi, round, IM_COL32(255, 255, 255, int(24.0f * fade)));
}

// the current window's frame, on its own list under a full-screen clip box (the window's own would cut the shadow); not
// the foreground list, which draws over the tooltips
void DrawWindowFrame(float fade) {
    if (!g_decorations) return;
    const ImVec2 lo = ImGui::GetWindowPos(), hi(lo.x + ImGui::GetWindowWidth(), lo.y + ImGui::GetWindowHeight());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();
    DrawFrame(dl, lo, hi, ImGui::GetStyle().WindowRounding, fade);
    dl->PopClipRect();
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

// the text for an ID in the overlay's language, English where a line has none. The ID is also the ID of the widget
// that shows it, so a widget keeps its ID, and its focus, across languages. A string that is no ID (a number, a
// name) shows as it is; an ID-shaped one with no line shows as it is too, and is logged once
const char* T(const char* id) {
    static const std::unordered_map<std::string_view, const Line*> lines = [] {
        std::unordered_map<std::string_view, const Line*> map;
        for (const Line& line : kLines) map.emplace(line.id, &line);
        return map;
    }();
    const auto it = lines.find(id);
    if (it == lines.end()) {
        static std::unordered_set<std::string> reported;
        const bool id_shaped = id[0] >= 'a' && id[0] <= 'z' && std::strpbrk(id, " %") == nullptr;
        if (id_shaped && reported.insert(id).second) LOGR("ui: no text for '%s'", id);
        return id;
    }
    const Line& l = *it->second;
    const char* const text[] = {l.en, l.de, l.fr, l.es, l.it, l.ja};
    return text[Column()];
}

// a dropdown's items, each in the overlay's language, as ImGui wants them. Leading spaces (which line up numbers)
// stay in front of the ID's text
std::string Items(std::initializer_list<const char*> items) {
    std::string out;
    for (const char* item : items) {
        const char* id = item;
        while (*id == ' ') id++;
        out.append(id - item, ' ');
        out += T(id), out += '\0';
    }
    out += '\0';
    return out;
}

// a settings section: a header that folds its rows away, open to begin with
bool g_first_row = true;      // nothing drawn above in this section or tab yet: no separator before the next setting
bool g_section_open = false;  // the section before this one, open: its rows end where the next begins (or the tab ends)
// the page reaches the window's edges (its section headers and separators too), and the rows are inset by the window's
// padding, which sits around the tabs and inside the rows instead
bool g_inset_on = false;
float RowInset() { return std::max(ImGui::GetStyle().WindowPadding.x - 4.0f, 1.0f); }  // both sides
void InsetOn() {
    if (g_inset_on) return;
    const float inset = RowInset();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::Indent(inset);
    w->WorkRect.Max.x -= inset;
    w->ContentRegionRect.Max.x -= inset;  // what GetContentRegionAvail (the controls' widths) reads
    g_inset_on = true;
}
void InsetOff() {
    if (!g_inset_on) return;
    const float inset = RowInset();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::Unindent(inset);
    w->WorkRect.Max.x += inset;
    w->ContentRegionRect.Max.x += inset;
    g_inset_on = false;
}

// a section folds and unfolds: its rows are submitted while it is open or still closing, in a clip whose height follows
// the animation (the rows' own height, measured as they are laid out), and fade in after it has started to open and out
// before it closes
struct {
    bool active = false;      // a section's rows are being laid out: EndSection closes them
    bool clipped = false;
    bool faded = false;
    float y0 = 0.0f, t = 1.0f, height = 0.0f;
    ImGuiID height_id = 0;
    float max_y = 0.0f;       // the layout's extent before the rows, which a part-open section keeps to its drawn part
} g_sec;
float g_card_l = 0.0f, g_card_r = 0.0f;  // the open section's card, which the lines between its settings stay within
bool g_any_section = false;             // a section above in this tab: the next card keeps its distance from it
float g_clip_bottom = 3.4e38f;  // lines drawn between settings stay above this while a section is part open

void SettingGap();
void DropOpenRow();

void EndSection() {
    if (!g_sec.active) return;
    g_sec.active = false;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    // the card's room under its last setting: as much as a row has under it, from the line to the next one
    ImGui::Dummy(ImVec2(0.0f, std::max(ImGui::GetStyle().ItemSpacing.y * 0.1f, 0.0f)));
    g_card_l = g_card_r = 0.0f;
    const float measured = ImGui::GetCursorScreenPos().y - g_sec.y0;
    w->StateStorage.SetFloat(g_sec.height_id, measured);
    if (g_sec.faded) ImGui::PopStyleVar();
    if (g_sec.clipped) {
        ImGui::PopClipRect();
        g_clip_bottom = 3.4e38f;
        const float e = g_sec.t * g_sec.t * (3.0f - 2.0f * g_sec.t);
        // the layout goes on from the part that shows (a zero-height item adds the item spacing the rows' end has)
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, g_sec.y0 + measured * e - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        w->DC.CursorMaxPos.y = std::max(g_sec.max_y, ImGui::GetCursorScreenPos().y);
    }
}

// a section's card: its header folds the rows away, or with `collapsible` off there is no header and the rows are always shown;
// default_open off starts it folded (until the person opens it)
bool Section(const char* name, bool collapsible = true, bool default_open = true) {
    EndSection();
    DropOpenRow();  // the last setting's hint that never came: not for this section's first hint to take
    // the header is rounded and sits in from the window's edges, by less than the rows under it
    const float margin = std::round(RowInset() * 0.5f);
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    InsetOff();
    if (g_any_section) {  // between one card and the next, as far as the header is from the sides (the cursor is at an open card's bottom, an item spacing below a closed one's)
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + margin - (g_section_open ? 0.0f : ImGui::GetStyle().ItemSpacing.y)));
    }
    g_any_section = true;
    ImGui::Indent(margin);
    const float work_right = w->WorkRect.Max.x;
    w->WorkRect.Max.x = std::round(work_right - margin);
    {
        // a header a whole number of pixels tall, so its edges and the card's border meet
        const float fs = ImGui::GetFontSize();
        ImGui::PushStyleVarY(ImGuiStyleVar_FramePadding, (std::round(fs + ImGui::GetStyle().FramePadding.y * 2.8f) - fs) * 0.5f);
    }
    const ImGuiID id = ImGui::GetID(name);
    const ImGuiID t_id = ImHashStr("section_t", 0, id), h_id = ImHashStr("section_h", 0, id);
    float t = w->StateStorage.GetFloat(t_id, -1.0f);
    if (t < 0.0f && collapsible && !default_open) t = 0.0f;  // first sight, folded: no card below the header
    const float height = w->StateStorage.GetFloat(h_id, 0.0f);
    {
        // the section's card, framed like the window, round its header and rows (as tall as the rows were
        // laid out last frame, by how far open it is)
        // on whole pixels, the header's frame too, so the border and the header's fill meet exactly
        ImVec2 at = ImGui::GetCursorScreenPos();
        at = ImVec2(std::round(at.x), std::round(at.y));
        ImGui::SetCursorScreenPos(at);
        const float shown = t < 0.0f ? 1.0f : t * t * (3.0f - 2.0f * t);
        const float head = collapsible ? ImGui::GetFrameHeight() : 0.0f;
        const float bottom = std::round(at.y + head + (t != 0.0f ? (collapsible ? ImGui::GetStyle().ItemSpacing.y : 0.0f) + height * shown : 0.0f));
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];  // a tiny bit brighter than the backdrop
        bg.x += (1.0f - bg.x) * 0.03f, bg.y += (1.0f - bg.y) * 0.03f, bg.z += (1.0f - bg.z) * 0.03f, bg.w = 1.0f;
        w->DrawList->AddRectFilled(at, ImVec2(std::round(w->WorkRect.Max.x), bottom), ImGui::GetColorU32(bg), ImGui::GetStyle().FrameRounding);
        if (g_decorations) DrawFrame(w->DrawList, at, ImVec2(std::round(w->WorkRect.Max.x), bottom), ImGui::GetStyle().FrameRounding, ImGui::GetStyle().Alpha, 6, 30.0f, 120.0f);
        g_card_l = at.x;
        g_card_r = w->WorkRect.Max.x;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);  // its lines are the card's and its bottom edge's
    const bool open = collapsible ? ImGui::CollapsingHeader(T(name), default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0) : true;
    ImGui::PopStyleVar();
    // open, the header keeps the card's black and translucent white lines along its bottom edge and round its lower corners, setting it off from the
    // rows (the rest of its outline is the card's own)
    if (collapsible && g_decorations && (open || t > 0.0f)) {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        DrawFrameBottom(w->DrawList, lo, hi, ImGui::GetStyle().FrameRounding, ImGui::GetStyle().Alpha, 120.0f);
    }
    ImGui::PopStyleVar();
    ImGui::Unindent(margin);
    w->WorkRect.Max.x = work_right;

    const float target = open ? 1.0f : 0.0f;
    if (t < 0.0f || height <= 0.0f) {
        t = target;  // first sight, or never laid out open: no animation
    } else {
        const float step = ImGui::GetIO().DeltaTime / 0.13f;
        t = t < target ? std::min(t + step, target) : std::max(t - step, target);
    }
    w->StateStorage.SetFloat(t_id, t);

    g_sec = {};
    g_sec.t = t;
    g_sec.height_id = h_id;
    g_sec.height = height;
    g_section_open = t > 0.0f;
    g_first_row = true;
    if (t <= 0.0f) {
        g_card_l = g_card_r = 0.0f;
        InsetOff();
        g_sec.t = 0.0f;
        return false;
    }
    InsetOn();
    g_sec.active = true;
    g_sec.y0 = ImGui::GetCursorScreenPos().y;
    g_sec.max_y = w->DC.CursorMaxPos.y;
    if (t < 1.0f) {
        const float e = t * t * (3.0f - 2.0f * t);
        const float bottom = g_sec.y0 + height * e;
        g_sec.clipped = true;
        ImGui::PushClipRect(w->ClipRect.Min, ImVec2(w->ClipRect.Max.x, std::min(w->ClipRect.Max.y, bottom)), true);
        g_clip_bottom = bottom;
        const float a = std::clamp((t - 0.45f) / 0.55f, 0.0f, 1.0f);
        g_sec.faded = true;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
    }
    return true;
}

// PushTextWrapPos takes the window's local x; this wraps at a screen x
void WrapAtScreenX(float x) {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::PushTextWrapPos(x - w->Pos.x + w->Scroll.x);
}

// the setting row being laid out (Beside starts it), for a hint called right after it to join: the hint goes under the
// label, in the label's room, and the label with it and the control are centred on each other down the row. What a hint
// and a control measure is kept from the last frame under the row's id, so the row can be centred before they are drawn.
struct RowMeasure { float hint_h = 0.0f, ctrl_h = 0.0f; };
struct RowState {
    int frame = -1;
    bool open = false;  // a hint may still join it
    ImGuiID key = 0;
    float x = 0.0f, top = 0.0f, width = 0.0f;  // the label's column and the row's top
    float label_y = 0.0f, label_h = 0.0f;      // where its label was drawn, and how tall
    float ctrl_y = 0.0f;                       // where its control starts
} g_row;
std::unordered_map<ImGuiID, RowMeasure> g_row_measure;

// the row a setting began, its hint not come: closed, so a later hint can't join it
void DropOpenRow() {
    if (g_row.open && g_row.frame == ImGui::GetFrameCount()) g_row_measure.erase(g_row.key), g_row.open = false;
}

// helper text with **bold** runs, for the values a setting can take. There is no bold face: a bold run is drawn twice, a
// pixel apart (as SubHeading). Wrapped at wrap_w from the cursor; with draw off it only measures. Returns the height.
float RichText(const char* text, float wrap_w, bool draw) {
    struct Tok {
        std::string s;
        bool bold, brk, sp;  // brk: a line may break before it (after a space, or between CJK characters); sp: a space does
        float w;
    };
    std::vector<Tok> toks;
    std::string cur;
    bool bold = false, cur_bold = false, space = false, brk = true, sp = false;
    const auto flush = [&] {
        if (cur.empty()) return;
        const float w = ImGui::CalcTextSize(cur.c_str()).x + (cur_bold ? 1.0f : 0.0f);
        toks.push_back({cur, cur_bold, brk, sp, w});
        cur.clear();
        space = false;
    };
    for (const char* c = text; *c;) {
        if (c[0] == '*' && c[1] == '*') {
            flush();
            bold = !bold;
            c += 2;
        } else if (*c == ' ') {
            flush();
            space = true;
            c++;
        } else {
            unsigned int cp = 0;
            const int len = ImTextCharFromUtf8(&cp, c, nullptr);
            const bool cjk = cp >= 0x3000;
            if (cur.empty()) cur_bold = bold, sp = space, brk = space || cjk || toks.empty();
            else if (cur_bold != bold || cjk) {
                flush();
                cur_bold = bold, sp = false, brk = cjk;
            }
            cur.append(c, size_t(len > 0 ? len : 1));
            c += len > 0 ? len : 1;
            if (cjk) flush();
        }
    }
    flush();

    const float line_h = ImGui::GetTextLineHeight(), space_w = ImGui::CalcTextSize(" ").x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    float x = 0.0f, y = 0.0f;
    for (size_t i = 0; i < toks.size();) {
        size_t end = i + 1;  // a unit: the tokens a line can't break between
        float uw = toks[i].w;
        while (end < toks.size() && !toks[end].brk) uw += toks[end++].w;
        const float lead = x > 0.0f && toks[i].sp ? space_w : 0.0f;
        if (x > 0.0f && x + lead + uw > wrap_w) x = 0.0f, y += line_h;
        else x += lead;
        for (size_t k = i; k < end; k++) {
            if (draw) {
                const ImVec2 at(origin.x + x, origin.y + y);
                dl->AddText(at, col, toks[k].s.c_str());
                if (toks[k].bold) dl->AddText(ImVec2(at.x + 1.0f, at.y), col, toks[k].s.c_str());
            }
            x += toks[k].w;
        }
        i = end;
    }
    const float h = y + line_h;
    if (draw) ImGui::Dummy(ImVec2(wrap_w, h));
    return h;
}

// a hint under the setting before it: smaller, dimmed and wrapped. Right after a setting row it is the label's subtext,
// in the label's room; elsewhere it runs the width under what is above
// the D3D11 or OpenGL renderer draws (gpu::g_d3d11, gpu::g_opengl): a row only Vulkan's draws is greyed, its hint
// saying so
bool D3d11() { return gpu::g_d3d11.load(std::memory_order_relaxed) || gpu::g_opengl.load(std::memory_order_relaxed); }
const char* NoVulkan() {
    return gpu::g_opengl.load(std::memory_order_relaxed) ? "hint_not_available_with_opengl"
                                                         : "hint_not_available_with_direct3d";
}

void Hint(const char* text, bool checkbox = false) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool inline_hint = g_row.open && g_row.frame == ImGui::GetFrameCount();
    ImGui::PushFont(nullptr, std::max(std::round(style.FontSizeBase * 0.8f), 11.0f));
    if (inline_hint) {
        g_row.open = false;
        const float gap = std::round(style.ItemSpacing.y * 0.25f);
        const float hint_h = RichText(T(text), g_row.width, false);
        const float ctrl_h = std::max(ImGui::GetCursorScreenPos().y - style.ItemSpacing.y - g_row.ctrl_y, 0.0f);
        RowMeasure& m = g_row_measure[g_row.key];
        m.hint_h = hint_h, m.ctrl_h = ctrl_h;
        const float pair = g_row.label_h + gap + hint_h;
        const float height = std::max(pair, ctrl_h);
        ImGui::SetCursorScreenPos(ImVec2(g_row.x, g_row.label_y + g_row.label_h + gap));
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        RichText(T(text), g_row.width, true);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        // on past the row, where an item spacing and the next one's gap come
        ImGui::SetCursorScreenPos(ImVec2(g_row.x, g_row.top + height));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        return;
    }
    // close to its setting; a checkbox's label sits mid-frame, so under a dropdown or slider it keeps the full spacing
    if (checkbox) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    RichText(T(text), ImGui::GetContentRegionAvail().x, true);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// a notice card: a tinted fill, a round mark and the text at full size, framed like the cards. Warning is amber with a
// "!", info a calmer blue with an "i"; several lines are a dotted list. `separated`: set apart like a setting, the line
// above it and none under it (the setting after it follows closely)
void NoticeCard(std::initializer_list<const char*> lines, bool info, bool separated) {
    DropOpenRow();  // a setting's hint that never came: not for this card to take
    if (separated) SettingGap();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = w->DrawList;
    const ImVec4 tone = info ? ImVec4(0.52f, 0.68f, 0.90f, 1.0f) : ImVec4(0.96f, 0.69f, 0.20f, 1.0f);
    const float pad = std::round(style.FramePadding.y * 1.5f);
    const float vpad = std::round(style.FramePadding.y * 1.9f);
    const float icon = ImGui::GetFontSize() * 1.2f;
    const float side = std::round(style.ItemSpacing.y * 0.5f);  // the gap above the card
    if (!separated) ImGui::Dummy(ImVec2(0.0f, side));
    const float left = std::round(ImGui::GetCursorScreenPos().x);
    const float right = std::round(w->WorkRect.Max.x);
    const float top = std::round(ImGui::GetCursorScreenPos().y);
    const float round = style.FrameRounding;

    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const float text_x = left + pad + icon + pad * 1.6f;
    const bool list = lines.size() > 1;
    const float dot = list ? ImGui::GetFontSize() * 0.9f : 0.0f;  // a list's text sits past its bullets
    ImGui::SetCursorScreenPos(ImVec2(text_x, top + vpad + 2.0f));
    for (const char* line : lines) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (list)
            dl->AddCircleFilled(ImVec2(at.x + dot * 0.3f, at.y + ImGui::GetTextLineHeight() * 0.5f), ImGui::GetFontSize() * 0.11f,
                                ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SetCursorScreenPos(ImVec2(at.x + dot, at.y));
        RichText(T(line), right - pad - text_x - dot, true);
        ImGui::SetCursorScreenPos(ImVec2(at.x, ImGui::GetCursorScreenPos().y));
    }
    const float bottom = std::round(std::max(ImGui::GetCursorScreenPos().y - style.ItemSpacing.y, top + vpad + icon) + vpad + 2.0f);
    dl->ChannelsSetCurrent(0);
    const ImVec2 lo(left, top), hi(right, bottom);
    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImVec4(tone.x, tone.y, tone.z, info ? 0.10f : 0.12f)), round);
    if (g_decorations) DrawFrame(dl, lo, hi, round, style.Alpha, 6, 30.0f, 120.0f);
    // the mark: a filled disc with its letter
    const ImVec2 c(left + pad + icon * 0.5f, top + vpad + icon * 0.5f);
    dl->AddCircleFilled(c, icon * 0.5f, ImGui::GetColorU32(tone));
    const char* mark = info ? "i" : "!";
    const ImVec2 ts = ImGui::CalcTextSize(mark);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), info ? IM_COL32(10, 22, 40, 255) : IM_COL32(30, 20, 0, 255), mark);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(left, bottom));
    ImGui::Dummy(ImVec2(right - left, 0.0f));  // an item spacing below, as between any two rows
    if (separated) g_first_row = true;  // no line under it
}
void WarningCard(const char* text, bool separated = false) { NoticeCard({text}, false, separated); }
void InfoCard(std::initializer_list<const char*> lines, bool separated = false) { NoticeCard(lines, true, separated); }

// what comes before a setting: a line between it and the one above, or under a section's header the room such a line
// gives (a table's cells and a row's later widgets are no settings of their own)
void SettingGap() {
    if (!g_first_row && !ImGui::GetCurrentTable() && ImGui::GetCursorScreenPos().x <= ImGui::GetCurrentWindow()->Pos.x + ImGui::GetCurrentWindow()->DC.Indent.x + 1.0f)
    {
        // the whole width of the window, over its padding (the child's clip box takes it)
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const float air = ImGui::GetStyle().ItemSpacing.y * 0.6f;  // above and below the line, around each setting
        // the line halfway between the rows: the cursor is an item spacing under the row above, and a Dummy adds one more
        // after itself, so the room above the line was an item spacing more than the room below it; both are now half of what they added up to
        const float sy = ImGui::GetStyle().ItemSpacing.y;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + air + 0.5f * sy));
        const float y = std::round(ImGui::GetCursorScreenPos().y);
        const bool carded = g_card_r > g_card_l;  // inside a section's card: the line runs its width
        const float left = carded ? g_card_l : w->Pos.x;
        const float right = carded ? g_card_r : w->Pos.x + w->Size.x - w->ScrollbarSizes.x;
        // past the child's own clip box sideways (it keeps half the padding), not up or down: a line scrolled out of
        // the page stays out of it
        const float top = std::max(y - 1.0f, w->InnerClipRect.Min.y), bottom = std::min({y + 2.0f, w->InnerClipRect.Max.y, g_clip_bottom});
        if (top < bottom) {
            w->DrawList->PushClipRect(ImVec2(left, top), ImVec2(right, bottom));
            w->DrawList->AddRectFilled(ImVec2(left, y), ImVec2(right, y + 1.0f), ImGui::GetColorU32(ImGuiCol_Separator));  // one whole pixel row
            w->DrawList->PopClipRect();
        }
        ImGui::Dummy(ImVec2(0.0f, 1.0f + air + 0.5f * sy));  // the line's pixel, then as much room as above it
    }
    else if (g_first_row && g_section_open && !ImGui::GetCurrentTable())  // under its header, the room a separator would give
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.6f + 1.0f));
    g_first_row = false;
}

// a setting's label to the left of its widget, in a column as wide for every setting (wrapped if it is longer): draws
// it, sizes the widget that follows it (to the row's end, less `after` for a button beside it) and returns the widget's hidden label
float LabelWidth() { return std::min(ImGui::GetFontSize() * 28.0f, ImGui::GetContentRegionAvail().x * 0.65f); }  // less in a narrow column
// on to the value column on the same line, `start` the label's screen x (SameLine's offset would be off in a table cell)
void ToValueColumn(float start, float width) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(start + width + ImGui::GetStyle().ItemSpacing.x, ImGui::GetCursorScreenPos().y));
}
const char* Beside(const char* label, float after = 0.0f, float wide = 0.0f) {
    static std::string id;
    // a hint that never came for the last row: forget what it measured
    if (g_row.open && g_row.frame == ImGui::GetFrameCount()) g_row_measure.erase(g_row.key);
    SettingGap();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float x = ImGui::GetCursorScreenPos().x, width = std::max(LabelWidth(), wide);
    // text (and its hint) wraps short of the column's end, so a label keeps a wide gap from its control; the control's column stays put
    const float text_width = width - (wide > LabelWidth() ? 0.0f : ImGui::GetFontSize() * 1.5f);
    const float top = ImGui::GetCursorScreenPos().y;
    const ImGuiID key = ImGui::GetID(label);
    const auto known = g_row_measure.find(key);
    const char* text = T(label);
    float label_y = top;
    float ctrl_y = top;
    const bool hinted = known != g_row_measure.end() && known->second.hint_h > 0.0f;
    float label_h = ImGui::CalcTextSize(text, nullptr, false, text_width).y;
    if (hinted) {
        // a label with a hint under it: the pair and the control centred on each other
        const float gap = std::round(st.ItemSpacing.y * 0.25f);
        const float pair = label_h + gap + known->second.hint_h;
        const float height = std::max(pair, known->second.ctrl_h);
        label_y = top + std::round((height - pair) * 0.5f);
        ctrl_y = top + std::round((height - known->second.ctrl_h) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, label_y));
        WrapAtScreenX(x + text_width);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::AlignTextToFramePadding();
        label_h = ImGui::GetFrameHeight();
        WrapAtScreenX(x + text_width);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        label_h = std::max(ImGui::GetItemRectSize().y, label_h);
    }
    // not on the label's line when it is under a hint's: its height (two lines, say) would be the control's line's
    if (!hinted) ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(x + width + st.ItemSpacing.x, ctrl_y));
    g_row = {ImGui::GetFrameCount(), true, key, x, top, text_width, label_y, label_h, ctrl_y};
    const float room = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(room - after, ImGui::GetFontSize() * 4.0f));
    id = std::string("##") + label;
    return id.c_str();
}
// a setting with a few short choices: segments side by side in one rounded track, the chosen one in the accent. Where
// they would not fit the room beside the label it is a dropdown instead. Leading spaces in a name (which line up a
// dropdown's numbers) are dropped. A label that starts with ## has no label column: the segments take the whole row.
// off: a bit for each choice greyed out
bool Segmented(const char* label, int* v, std::initializer_list<const char*> items, uint32_t off = 0) {
    const char* id = label[0] == '#' && label[1] == '#' ? label : Beside(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    std::vector<std::string> names;
    std::vector<float> widths;
    const float pad = st.FramePadding.x * 1.2f;
    float total = 0.0f;
    for (const char* item : items) {
        const char* id = item;
        while (*id == ' ') id++;
        const char* name = T(id);
        names.push_back(name);
        widths.push_back(std::round(ImGui::CalcTextSize(name).x + pad * 2.0f));
        total += widths.back();
    }
    const float room = std::floor(ImGui::GetContentRegionAvail().x);
    // equal segments: each is as wide as the longest name needs, so they fit when that times the count does
    const float widest = *std::max_element(widths.begin(), widths.end());
    total = widest * float(widths.size());
    if (total > room) {
        if (id == label) ImGui::SetNextItemWidth(-FLT_MIN);  // no label column: the dropdown the row's width too
        if (!off) return ImGui::Combo(id, v, Items(items).c_str());
        bool picked = false;
        if (ImGui::BeginCombo(id, *v >= 0 && *v < int(names.size()) ? names[size_t(*v)].c_str() : "")) {
            for (int i = 0; i < int(names.size()); i++) {
                const ImGuiSelectableFlags flags = off >> i & 1 ? ImGuiSelectableFlags_Disabled : 0;
                if (ImGui::Selectable(names[size_t(i)].c_str(), *v == i, flags) && *v != i) *v = i, picked = true;
            }
            ImGui::EndCombo();
        }
        return picked;
    }
    // the row fills the room: every segment the same share of it, the last the rounding
    {
        const float each = std::floor(room / float(widths.size()));
        for (float& w : widths) w = each;
        widths.back() += room - each * float(widths.size());
        total = room;
    }

    bool changed = false;
    const float height = ImGui::GetFrameHeight(), round = st.FrameRounding;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // drawn on whole pixels (the widths are whole already), so a fill's edge and the lines on it meet exactly
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 lo(std::round(origin.x), std::round(origin.y)), hi(lo.x + total, lo.y + height);
    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_FrameBg), round);
    ImGui::PushID(id);
    float x = lo.x;
    for (int i = 0; i < int(names.size()); i++) {
        if (i) ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushID(i);
        const bool grey = off >> i & 1;
        ImGui::BeginDisabled(grey);
        if (ImGui::InvisibleButton("##segment", ImVec2(widths[size_t(i)], height))) *v = i, changed = true;
        ImGui::EndDisabled();
        const ImVec2 a(x, lo.y), b(x + widths[size_t(i)], hi.y);
        const ImDrawFlags corners = names.size() == 1 ? ImDrawFlags_RoundCornersAll
                                    : i == 0          ? ImDrawFlags_RoundCornersLeft
                                    : i + 1 == int(names.size()) ? ImDrawFlags_RoundCornersRight
                                                                 : ImDrawFlags_RoundCornersNone;
        if (*v == i) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_CheckMark, 0.80f), round, corners);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18), round, corners);
        const ImVec2 size = ImGui::CalcTextSize(names[size_t(i)].c_str());
        dl->AddText(ImVec2(std::round((a.x + b.x - size.x) * 0.5f), std::round((a.y + b.y - size.y) * 0.5f)),
                    ImGui::GetColorU32(grey ? ImGuiCol_TextDisabled : ImGuiCol_Text), names[size_t(i)].c_str());
        const bool beside_chosen = *v == i || *v == i - 1;  // the chosen one's sides have lines of their own
        if (i && g_decorations && !beside_chosen) {  // a line between this segment and the one before: dark, with a light one beside it
            dl->AddLine(ImVec2(a.x + 0.5f, a.y + 1.0f), ImVec2(a.x + 0.5f, b.y - 1.0f), IM_COL32(0, 0, 0, int(120.0f * st.Alpha)));
            dl->AddLine(ImVec2(a.x + 1.5f, a.y + 1.0f), ImVec2(a.x + 1.5f, b.y - 1.0f), IM_COL32(255, 255, 255, int(24.0f * st.Alpha)));
        } else if (i && !g_decorations) {
            dl->AddLine(ImVec2(a.x + 0.5f, a.y + 1.0f), ImVec2(a.x + 0.5f, b.y - 1.0f), IM_COL32(255, 255, 255, int(40.0f * st.Alpha)));
        }
        if (*v == i && g_decorations) {
            // its sides, as DrawFrame draws a rectangle: the dark line just outside the fill (on the neighbour's edge, so the
            // fill doesn't show through it), the light one on the fill's own edge
            const float y0 = a.y + 1.0f, y1 = b.y - 1.0f;
            const ImU32 dark = IM_COL32(0, 0, 0, int(200.0f * st.Alpha)), light = IM_COL32(255, 255, 255, int(55.0f * st.Alpha));
            if (i > 0) {
                dl->AddLine(ImVec2(a.x - 0.5f, y0), ImVec2(a.x - 0.5f, y1), dark);
                dl->AddLine(ImVec2(a.x + 0.5f, y0), ImVec2(a.x + 0.5f, y1), light);
            }
            if (i + 1 < int(names.size())) {
                dl->AddLine(ImVec2(b.x + 0.5f, y0), ImVec2(b.x + 0.5f, y1), dark);
                dl->AddLine(ImVec2(b.x - 0.5f, y0), ImVec2(b.x - 0.5f, y1), light);
            }
        }
        x = b.x;
        ImGui::PopID();
    }
    ImGui::PopID();
    if (g_decorations) {
        DrawFrame(dl, lo, hi, round, st.Alpha, 0, 0.0f, 120.0f);
        // over the chosen segment the lines need more to show, as on the tabs' selected pill: the same frame again, with
        // the tabs' stronger lines, kept to that segment's stretch of it
        float x = lo.x;
        for (int i = 0; i < int(widths.size()); i++) {
            const float x1 = x + widths[size_t(i)];
            if (*v == i) {
                dl->PushClipRect(ImVec2(i == 0 ? lo.x - 1.0f : x, lo.y - 1.0f),
                                 ImVec2(i + 1 == int(widths.size()) ? hi.x + 1.0f : x1, hi.y + 1.0f), true);
                DrawFrame(dl, lo, hi, round, st.Alpha, 0, 0.0f, 200.0f, 55.0f);
                dl->PopClipRect();
            }
            x = x1;
        }
    }
    return changed;
}

// a percent slider with a Reset beside it, greyed out while the value is its default
// div > 1 shows v / div with two decimals (a setting kept in hundredths)
void Slider(const char* label, uint32_t* v, uint32_t min, uint32_t max, uint32_t def, uint32_t div = 1) {
    ImGui::PushID(label);
    const float reset = ImGui::CalcTextSize(T("reset")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    Beside(label, reset + ImGui::GetStyle().ItemInnerSpacing.x);
    if (div > 1) {
        float f = float(*v) / float(div);
        if (ImGui::SliderFloat("##slider", &f, float(min) / float(div), float(max) / float(div), "%.2f"))
            *v = std::clamp<uint32_t>(uint32_t(std::lround(f * float(div))), min, max);
    } else {
        ImGui::SliderScalar("##slider", ImGuiDataType_U32, v, &min, &max, "%u");
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(*v == def);
    if (ImGui::Button(T("reset"))) *v = def;
    ImGui::EndDisabled();
    ImGui::PopID();
}

// a custom font (game_fonts): the font's name on a button that opens the system's file dialog, Reset back to the
// game's own; a file that isn't a font it can use says so under it
void FontRow(const char* label, game_fonts::Kind kind, const char* hint) {
    ImGui::PushID(label);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float reset = ImGui::CalcTextSize(T("reset")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    Beside(label, reset + spacing);
    const std::string path = game_fonts::Path(kind);
    std::string shown = game_fonts::Name(kind);
    if (shown.empty() && !path.empty()) shown = path.substr(path.find_last_of("/\\") + 1);  // npos + 1: all of it
    if (shown.empty()) shown = T("the_game_s_own");
    ImGui::BeginDisabled(D3d11());
    if (ImGui::Button((shown + "###pick").c_str(), ImVec2(ImGui::CalcItemWidth(), 0.0f)))
        app::PickFile(
            T("fonts"), "ttf;otf;ttc",
            [](void* k, const char* picked) {
                if (picked) game_fonts::Choose(game_fonts::Kind(reinterpret_cast<intptr_t>(k)), picked);
            },
            reinterpret_cast<void*>(intptr_t(kind)));
    ImGui::SameLine(0.0f, spacing);
    ImGui::BeginDisabled(path.empty());
    if (ImGui::Button((std::string(T("reset")) + "###reset_font").c_str())) game_fonts::Clear(kind);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Hint(D3d11() ? NoVulkan() : hint, true);
    if (!path.empty() && !game_fonts::Error(kind).empty()) WarningCard("hint_this_file_isn_t_a_font");
    ImGui::BeginDisabled(D3d11());
    // a collection's faces or a variable font's named instances
    const std::vector<std::string> styles = game_fonts::Styles(kind);
    if (styles.size() > 1) {
        const int current = std::clamp(game_fonts::StyleIndex(kind), 0, int(styles.size()) - 1);
        if (ImGui::BeginCombo(Beside("font_style"), styles[size_t(current)].c_str())) {
            for (int i = 0; i < int(styles.size()); i++) {
                ImGui::PushID(i);
                if (ImGui::Selectable(styles[size_t(i)].c_str(), i == current)) game_fonts::SetStyle(kind, i);
                if (i == current) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        Hint("hint_the_styles_this_font_file", true);
    }
    // a weight axis: any weight along it, Reset back to the style's own
    int min = 0, max = 0;
    if (game_fonts::WeightRange(kind, min, max)) {
        Beside("font_weight", reset + spacing);
        // a mouse drag shows its weight as it goes and sets it on release: each one set draws the font's glyphs again
        static int held[game_fonts::kKinds] = {};
        static bool dragging[game_fonts::kKinds] = {};
        int weight = dragging[kind] ? held[kind] : game_fonts::Weight(kind);
        const bool moved = ImGui::SliderInt("##weight", &weight, min, max, "%d");
        held[kind] = weight = std::clamp(weight, min, max);
        dragging[kind] = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if ((moved && !dragging[kind]) || ImGui::IsItemDeactivatedAfterEdit()) game_fonts::SetWeight(kind, weight);
        ImGui::SameLine(0.0f, spacing);
        ImGui::BeginDisabled(!game_fonts::WeightSet(kind));
        if (ImGui::Button((std::string(T("reset")) + "###reset_weight").c_str())) game_fonts::SetWeight(kind, 0);
        ImGui::EndDisabled();
        Hint("hint_how_bold_the_letters_are", true);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

// one whose default, 0, is a level the display reports: shown as `shown` until moved, Reset taking it back to 0
void DisplaySlider(const char* label, uint32_t* v, uint32_t min, uint32_t max, uint32_t shown) {
    ImGui::PushID(label);
    const float reset = ImGui::CalcTextSize(T("reset")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    Beside(label, reset + ImGui::GetStyle().ItemInnerSpacing.x);
    uint32_t value = *v ? *v : shown;
    if (ImGui::SliderScalar("##slider", ImGuiDataType_U32, &value, &min, &max, "%u")) *v = value;
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(*v == 0);
    if (ImGui::Button(T("reset"))) *v = 0;
    ImGui::EndDisabled();
    ImGui::PopID();
}

// a signed one, shown with its sign
void Slider(const char* label, int32_t* v, int32_t min, int32_t max, int32_t def) {
    ImGui::PushID(label);
    const float reset = ImGui::CalcTextSize(T("reset")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    Beside(label, reset + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SliderScalar("##slider", ImGuiDataType_S32, v, &min, &max, "%+d");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(*v == def);
    if (ImGui::Button(T("reset"))) *v = def;
    ImGui::EndDisabled();
    ImGui::PopID();
}

// Two percentages on one track, lo below hi and kept at least `gap` apart, with a Reset beside it. The handle nearest
// the click takes the drag.
void RangeSlider(const char* label, uint32_t* lo, uint32_t* hi, uint32_t min, uint32_t max, uint32_t gap,
                 uint32_t def_lo, uint32_t def_hi, float column = -1.0f, float track = 0.0f, float reset_w = 0.0f) {
    ImGui::PushID(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float reset = ImGui::CalcTextSize(T("reset")).x + st.FramePadding.x * 2.0f;
    float w;
    if (column >= 0.0f) {  // in a row of buttons (the keys): the label, the track as wide as the keys, a Reset as theirs
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(label));
        ImGui::SameLine(column);
        w = track;
    } else {
        Beside(label, reset + st.ItemInnerSpacing.x);
        w = ImGui::CalcItemWidth();
    }
    const float h = ImGui::GetFrameHeight();
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
    if (ImGui::Button(T("reset"), ImVec2(column >= 0.0f ? reset_w : 0.0f, 0))) *lo = def_lo, *hi = def_hi;
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() + 2.0f));  // room for the numbers under the track
    ImGui::PopID();
}

// a checkbox; right under a slider it gets a little more room
bool Check(const char* label, bool* v) {
    // the label at the left in the labels' column and the box at the right of it, where the other controls sit
    // a label wider than the column takes the room it needs, as far as the box still fits, and wraps past that
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;  // the least between a label's end and its box, besides the usual
    const float room = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x * 2.0f;
    return ImGui::Checkbox(Beside(label, 0.0f, std::min(ImGui::CalcTextSize(T(label)).x + gap, room)), v);
}

// the device in a player's keyboard, mouse or controller slot; picking one another player has moves it here
void DeviceCombo(int player, input::Kind kind) {
    using input::Kind;
    const std::vector<input::Device> devices = input::Devices(kind);
    const std::string held = input::Assigned(player, kind);
    const char* all = kind == Kind::Keyboard ? T("all_other_keyboards") : T("all_other_mice");
    // a device no longer connected keeps its name, without the #n
    auto label = [&](const std::string& key) -> std::string {
        if (key.empty()) return T("none");
        if (key == input::kAll) return all;
        for (const input::Device& d : devices)
            if (d.key == key) return d.label;
        char text[512];
        std::snprintf(text, sizeof text, T("s_not_connected"), key.substr(0, key.rfind('#')).c_str());
        return text;
    };
    // whose it is now, when another player's
    auto whose = [&](const std::string& key) -> std::string {
        const int p = input::Holder(kind, key);
        if (p < 0 || p == player) return "";
        char text[64];
        std::snprintf(text, sizeof text, T("player_d2"), p + 1);
        return text;
    };
    ImGui::PushID(int(kind));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##device", label(held).c_str())) {
        auto option = [&](const std::string& key, const std::string& text) {
            if (ImGui::Selectable((text + whose(key) + "##" + key).c_str(), held == key)) input::Assign(player, kind, key);
        };
        option("", T("none"));
        if (kind != Kind::Controller) option(input::kAll, all);
        for (const input::Device& d : devices) option(d.key, d.label);
        ImGui::EndCombo();
    }
    if (kind != Kind::Controller && held == input::kAll && ImGui::IsItemHovered()) {
        const ImGuiStyle& ts = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ts.WindowPadding.x * 0.5f, ts.WindowPadding.y * 0.4f));
        {
            ImVec4 dark = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
            dark.x *= 0.35f, dark.y *= 0.35f, dark.z *= 0.35f;
            ImGui::PushStyleColor(ImGuiCol_PopupBg, dark);
        }
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::SetTooltip("%s", kind == Kind::Keyboard ? T("hint_uses_every_keyboard_not_given")
                                                    : T("uses_every_mouse"));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

// a player's devices, and whether new controllers find a player on their own (shared by all, under each tab)
void DrawDevices(int player) {
    if (!Section("devices")) return;
    const float x0 = ImGui::GetCursorScreenPos().x, label_width = LabelWidth();
    for (input::Kind kind : {input::Kind::Keyboard, input::Kind::Mouse, input::Kind::Controller}) {
        SettingGap();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(kind == input::Kind::Keyboard ? "keyboard" : kind == input::Kind::Mouse ? "mouse" : "controller"));
        ToValueColumn(x0, label_width);  // the column of the other settings
        DeviceCombo(player, kind);
    }
    bool auto_assign = input::AutoAssign();
    if (Check("hint_new_controllers_go_to_the", &auto_assign))
        input::SetAutoAssign(auto_assign);
    Hint("hint_otherwise_they_stay_unassigned_until", true);
}

// a player's controller options, the game's own two (they replace its Options menu's)
void DrawController(int player) {
    if (!Section("controller")) return;
    // the sticks' curves and dead zones, in a match too
    using gamepad::kStickMin, gamepad::kStickMax, gamepad::kStickDefault;
    // movement, then the camera; the dead zone's hint, under the movement row, is for both
    using gamepad::kDeadZoneMax, gamepad::kDeadZoneDefault;
    Slider("movement_stick", &gamepad::g_move_stick_percent[player], kStickMin, kStickMax, kStickDefault);
    Hint("hint_stick_sensitivity");
    Slider("movement_dead_zone", &gamepad::g_move_stick_dead_zone[player], 0, kDeadZoneMax, kDeadZoneDefault);
    Hint("hint_stick_dead_zone");
    Slider("camera_stick_x", &gamepad::g_look_stick_x_percent[player], kStickMin, gamepad::kLookXMax, kStickDefault);
    Slider("camera_stick_y", &gamepad::g_look_stick_y_percent[player], kStickMin, gamepad::kLookYMax, kStickDefault);
    Hint("hint_camera_stick_speed");
    Slider("camera_dead_zone", &gamepad::g_look_stick_dead_zone[player], 0, kDeadZoneMax, kDeadZoneDefault);
    WarningCard("hint_single_player_only_invert_aim", true);
    Check("invert_aim", &gamepad::g_invert_aim[player]);
    Hint("hint_up_on_the_right_stick", true);
    WarningCard("hint_single_player_only_vibration", true);
    // one slider: 0 is off (the game's own vibration flag follows it)
    uint32_t level = gamepad::g_vibration[player] ? std::min(gamepad::g_vibration_percent[player], 100u) : 0;
    Slider("vibration", &level, 0, 100, 100);
    gamepad::g_vibration_percent[player] = level;
    gamepad::g_vibration[player] = level > 0;
    Hint("hint_how_hard_the_controller_shakes");
}

// a player's mouse look
void DrawMouse(int player) {
    if (!Section("mouse")) return;
    Check("mouse_look", &mouse::g_look[player]);
    Hint("hint_move_the_mouse_to_look", true);
    Slider("horizontal", &mouse::g_sensitivity[player], 10, 500, mouse::kDefaultSensitivity);
    Slider("vertical", &mouse::g_sensitivity_y[player], 10, 500, mouse::kDefaultSensitivity);
    WarningCard("hint_single_player_only_in_a", true);
    Check("invert_y", &mouse::g_invert_y[player]);
}

// a player's keys
void DrawKeys(int player) {
    // another player's tab: its rebind is done
    if (g_rebind >= 0 && g_rebind_player != player) g_rebind = -1;
    PollRebind();
    g_cancel_hovered = false;  // the waiting row sets it again as it draws
    if (!Section("keyboard")) {
        g_rebind = -1;  // folded away: no key waits
        return;
    }
    InfoCard({"hint_right_click_a_slot_y"});
    if (const int scheme = keyboard::Scheme(player))
        ImGui::Text(T("in_use_now"), keyboard::SchemeName(scheme));
    // a frame per kind of action; the frame names it, so a row only names its action
    struct Row {
        int action;
        const char* label;
    };
    struct Group {
        const char* title;
        int row, side;  // the table cell it stacks in
        std::vector<Row> rows;
    };
    using namespace keyboard;
    static const Group kGroups[] = {
        {"movement", 0, 0, {{kMoveUp, "forward"}, {kMoveDown, "back"}, {kMoveLeft, "left"}, {kMoveRight, "right"},
                      {kJump, "jump"}, {kCrouch, "crouch"}, {kWalk, "walk"}, {kCreep, "creep"}}},
        {"camera", 0, 1, {{kLookUp, "look_up"}, {kLookDown, "look_down"}, {kLookLeft, "look_left"},
                    {kLookRight, "look_right"}, {kZoom, "zoom_story"}}},
        {"weapons", 1, 0, {{kAttack, "attack_fire"}, {kAim, "aim_equip_weapon"}, {kWeaponMode, "first_person_mode"},
                     {kReload, "reload"}, {kUse, "use_story"}}},
        {"game", 0, 1, {{kPause, "pause"}, {kQuit, "quit_title_menu"}}},
        {"multiplayer", 1, 1, {{kAbility, "abilities"}, {kCycle, "cycle_equipped_hold"}, {kVote, "vote_kick"},
                         {kChat, "chat_shift_everyone"}}},
    };
    // two columns and two rows (Weapons and Multiplayer line up), each cell its groups stacked
    // a wide gap between the columns (the padding falls on both sides of it)
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetFontSize() * 1.2f, ImGui::GetStyle().CellPadding.y));
    const bool groups = ImGui::BeginTable("##groups", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
    ImGui::PopStyleVar();
    if (!groups) return;
    for (int cell = 0; cell < 4; cell++) {
        ImGui::TableNextColumn();
        for (const Group& g : kGroups) {
            if (g.row * 2 + g.side != cell) continue;
            ImGui::PushID(g.title);
            ImGui::BeginChild("##group", ImVec2(0, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
            ImGui::SeparatorText(T(g.title));
            // label, the action's keys side by side, then its Reset; the keys line up down the frame
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float reset = std::max(ImGui::CalcTextSize(T("reset")).x, ImGui::CalcTextSize(T("cancel")).x) +
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
                    if (ImGui::Button(waiting ? T("hint_press") : KeyName(g_keys[player][r.action][s]),
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
                    ImGui::Button(T("cancel"), ImVec2(reset, 0));
                    g_cancel_hovered = ImGui::IsItemHovered();
                } else {
                    ImGui::BeginDisabled(IsDefault(player, r.action));
                    if (ImGui::Button(T("reset"), ImVec2(reset, 0))) ResetKey(player, r.action);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        const ImGuiStyle& ts = ImGui::GetStyle();
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ts.WindowPadding.x * 0.5f, ts.WindowPadding.y * 0.4f));
        {
            ImVec4 dark = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
            dark.x *= 0.35f, dark.y *= 0.35f, dark.z *= 0.35f;
            ImGui::PushStyleColor(ImGuiCol_PopupBg, dark);
        }
        ImGui::SetNextWindowBgAlpha(0.85f);
                        ImGui::SetTooltip(T("default_s"), ActionDefault(r.action).c_str());
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::PopID();
            }
            if (g.rows[0].action == kMoveUp) {  // the Walk and Creep tilts, under the movement keys
                ImGui::Spacing();
                g_first_row = true;  // no separator over it: it belongs to the movement keys above
                RangeSlider("creep_walk", &g_creep_percent[player], &g_walk_percent[player], kTiltMin, kTiltMax, kTiltGap,
                            kDefaultCreep, kDefaultWalk, column, key_w * kSlots + spacing * (kSlots - 1), reset);
            }
            ImGui::Dummy(ImVec2(0.0f, 2.0f));  // the last row's buttons were clipped by the frame's bottom edge
            ImGui::EndChild();
            ImGui::PopID();
            // room before the next group stacked in the same cell
            if (&g != &kGroups[std::size(kGroups) - 1] && (&g + 1)->row == g.row && (&g + 1)->side == g.side)
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 1.5f));
        }
    }
    ImGui::EndTable();
    if (ImGui::Button(T("default_keys"))) {
        keyboard::ResetKeys(player);
        g_rebind = -1;
    }
}

// the Debug tab: warps to a checkpoint, a level or a cutscene (warp.cpp runs them on the main thread)
// a titled part inside a card: the full-width line the settings have between them, then its title
void SubHeading(const char* title) {
    SettingGap();
    // no bold face: the title drawn twice, a pixel apart
    const char* text = T(title);
    const ImVec2 at = ImGui::GetCursorScreenPos(), size = ImGui::CalcTextSize(text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(at, col, text);
    dl->AddText(ImVec2(at.x + 1.0f, at.y), col, text);
    ImGui::Dummy(ImVec2(size.x + 1.0f, size.y));
}

void DrawWarp() {
    static char filter[64] = "";
    static int checkpoint = -1;
    static int cut_level = 0, cut_index = 0;

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

    {
        SubHeading("current_level");
        const int current = warp::CurrentLevel();
        const char* current_name = warp::LevelName(current);
        ImGui::Text("%d %s", current, current_name ? current_name : "");
        const std::string status = warp::Status();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    }

    {
        SubHeading("checkpoints");
        WarningCard("hint_warning_these_start_a_new");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", T("filter_by_name"), filter, sizeof filter);
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
        if (ImGui::Button(T("start_at_checkpoint")) && warp::GoCheckpoint(checkpoint)) Close();
        ImGui::EndDisabled();
    }

    {
        SubHeading("soak_test");
        ImGui::TextWrapped("%s", T("hint_soak_test"));
        if (ImGui::Button(T("start_soak_test"))) {
            keyboard::HoldSoakTest();
            Close();
        }
    }

    {
        SubHeading("cutscenes");
        int found_level = -1;
        const std::vector<int> found = warp::Cutscenes(&found_level);
        if (found_level >= 0) {
            const char* fn = warp::LevelName(found_level);
            ImGui::Text(T("in_s"), fn ? fn : T("this_level"));
            if (found.empty()) ImGui::TextDisabled("%s", T("none2"));
            for (size_t i = 0; i < found.size(); i++) {
                if (i % 10) ImGui::SameLine();
                if (ImGui::Button(std::to_string(found[i]).c_str()) && warp::PlayCutscene(found_level, found[i])) Close();
            }
        }
        level_combo("level", "cut", &cut_level);
        ImGui::InputInt(T("cutscene"), &cut_index);
        cut_index = std::clamp(cut_index, 0, 255);
        if (ImGui::Button(T("play_cutscene")) && warp::PlayCutscene(cut_level, cut_index)) Close();
    }
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

void FeedNav(const Buttons& drive);

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
            if (g_calib_step.load() >= 0) g_calib_step.store(-1);
            else Close();
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
    FeedNav(drive);
}

// the pads' buttons as ImGui's pad keys: the left stick and d-pad move, A activates, B backs out
void FeedNav(const Buttons& drive) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // A on a tab button goes down into its page (the cursor on the first setting) instead of activating the tab, and A
    // is the page's own only once it has been let go
    static bool a_was = false, a_eaten = false;
    const bool on_tab = g_tab_focused;
    g_tab_focused = false;
    const bool into_page = drive.a && !a_was && on_tab;
    if (into_page) a_eaten = true;
    if (!drive.a) a_eaten = false;
    a_was = drive.a;
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, drive.a && !a_eaten);
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
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, drive.down || stick_down || into_page);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, drive.left || stick_left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, drive.right || stick_right);
}

// the quit prompt's body. Cancel is where the pad starts, and B is Cancel too (ImGui's own back out leaves a modal up)
bool QuitBody() {
    ImGui::TextUnformatted(T("hint_quit_the_game_progress_since"));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.8f));
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 2.0f, st.FramePadding.y * 1.5f));
    if (ImGui::Button(T("quit"))) app::Quit();  // only posts: the window thread closes
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("cancel"));
    ImGui::SetItemDefaultFocus();
    ImGui::PopStyleVar();
    return !cancel && !g_pad.b;
}

// the accent, and the widgets' frames: the white line on the frame's edge and the dark one just outside it (the section
// cards' and the window's own lines, without the shadow)
void MenuStyle() {
    app::UiAccent(int(g_accent));
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameBorderSize = g_decorations ? 1.0f : 0.0f;
    st.PopupBorderSize = g_decorations ? 0.0f : 1.0f;  // the dropdowns' frames are drawn below, with their shadow
    st.Colors[ImGuiCol_Border] = ImVec4(1, 1, 1, 24.0f / 255.0f);
    st.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 120.0f / 255.0f);
}

// what an achievement shows at its row's end: the day it was earned, or how far along it is, or Locked
std::string AchievementStatus(int index, const achievements::State& st) {
    char text[64];
    if (st.unlocked) {
        const std::time_t when = std::time_t(st.when);
        const std::tm* tm = std::localtime(&when);
        if (tm && std::strftime(text, sizeof text, "%Y-%m-%d", tm)) return text;
        return "";
    }
    const achievements::Info& info = achievements::Get(index);
    if (!info.goal) return T("locked");
    if (info.goal == 1000) std::snprintf(text, sizeof text, "$%d / $%d", st.progress, info.goal);  // Cash Grabber
    else std::snprintf(text, sizeof text, "%d / %d", st.progress, info.goal);
    return text;
}

// a line of the tab's list: a name over its description, a status at the row's end (a bar under it toward a goal)
void AchievementLine(const char* name, const char* description, const std::string& status, bool unlocked, int goal,
                     int progress) {
    SettingGap();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetCurrentWindow()->WorkRect.Max.x - style.ItemSpacing.x;
    const ImVec2 status_size = ImGui::CalcTextSize(status.c_str());
    const float text_x = at.x;
    const float text_r = right - status_size.x - style.ItemSpacing.x * 2.0f;

    ImGui::SetCursorScreenPos(ImVec2(text_x, at.y));
    WrapAtScreenX(std::max(text_r, text_x + ImGui::GetFontSize() * 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, unlocked ? style.Colors[ImGuiCol_Text] : ImVec4(1, 1, 1, 0.62f));
    ImGui::TextWrapped("%s", name);
    ImGui::PopStyleColor();
    // under the name, at its x: a new line would start at the row's indent
    ImGui::SetCursorScreenPos(ImVec2(text_x, ImGui::GetCursorScreenPos().y - style.ItemSpacing.y * 0.6f));
    ImGui::PushFont(nullptr, std::max(std::round(style.FontSizeBase * 0.8f), 11.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", description);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    const float bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
    const float mid = (at.y + bottom) * 0.5f;

    const ImU32 status_color = unlocked ? ImGui::GetColorU32(ImGuiCol_CheckMark) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddText(ImVec2(right - status_size.x, mid - status_size.y * 0.5f), status_color, status.c_str());
    if (!unlocked && goal) {  // a thin bar under the count
        const float y = mid + status_size.y * 0.5f + 3.0f, w = status_size.x;
        dl->AddRectFilled(ImVec2(right - w, y), ImVec2(right, y + 3.0f), IM_COL32(255, 255, 255, 30), 1.5f);
        dl->AddRectFilled(ImVec2(right - w, y), ImVec2(right - w + w * float(progress) / float(goal), y + 3.0f),
                          ImGui::GetColorU32(ImGuiCol_CheckMark), 1.5f);
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, bottom));
    ImGui::Dummy(ImVec2(right - at.x, 0.0f));
}

// a secret achievement not yet earned (Info::secret) stays out of the list: the section ends with one line counting them
bool Hidden(int index) { return achievements::Get(index).secret && !achievements::StateOf(index).unlocked; }

void AchievementRow(int index) {
    const achievements::Info& info = achievements::Get(index);
    const achievements::State st = achievements::StateOf(index);
    AchievementLine(info.name, info.description[Column()], AchievementStatus(index, st), st.unlocked, info.goal, st.progress);
}

void SecretRow(int count) {
    char name[96];
    if (count == 1) std::snprintf(name, sizeof name, "%s", T("one_secret_achievement"));
    else std::snprintf(name, sizeof name, T("n_secret_achievements"), count);
    AchievementLine(name, T("hint_keep_playing_to_unlock"), T("locked"), false, 0, 0);
}

// the Achievements tab: how many are earned, then the story's and multiplayer's
void DrawAchievements() {
    if (Section("achievements", false)) {
        Check("earn_achievements", &achievements::g_enabled);
        Hint("hint_off_nothing_is_earned_or", true);
        SettingGap();
        const int got = achievements::UnlockedCount();
        char line[96];
        std::snprintf(line, sizeof line, T("unlocked_of"), got, achievements::kCount);
        ImGui::TextUnformatted(line);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::GetStyle().Colors[ImGuiCol_CheckMark]);
        ImGui::ProgressBar(float(got) / float(achievements::kCount), ImVec2(-FLT_MIN, ImGui::GetFontSize() * 0.45f), "");
        ImGui::PopStyleColor();
        Hint("hint_kept_on_this_computer_only");
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));  // set apart from the button, as on the Quit tab
        // Reset, red as Quit, behind a prompt: Cancel is where the pad starts, and B is Cancel too
        PushDangerButton();
        const bool ask = ImGui::Button(T("reset_achievements"));
        PopDangerButton();
        if (ask) ImGui::OpenPopup("##reset_achievements");
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        if (ImGui::BeginPopupModal("##reset_achievements", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(T("hint_reset_every_achievement_and"));
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.8f));
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 2.0f, st.FramePadding.y * 1.5f));
            PushDangerButton();
            const bool reset = ImGui::Button(T("reset"));
            PopDangerButton();
            ImGui::SameLine();
            const bool cancel = ImGui::Button(T("cancel"));
            if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            ImGui::PopStyleVar();
            if (reset) achievements::Reset();
            DrawWindowFrame(1.0f);
            if (reset || cancel || g_pad.b) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }
    for (const bool multi : {false, true}) {
        if (!Section(multi ? "multiplayer" : "single_player")) continue;
        int secret = 0;
        for (int i = 0; i < achievements::kCount; i++) {
            if (achievements::Get(i).multiplayer != multi) continue;
            if (Hidden(i)) secret++;
            else AchievementRow(i);
        }
        if (secret) SecretRow(secret);
    }
}

// the display calibration: a test pattern over the whole display (gpu::g_calibrate) and a panel at the bottom with
// the step's setting, what to look for and the way on. Under HDR paper white, black level, gamma and the peak; else
// black level and gamma
void DrawCalibration(app::Settings& s) {
    const bool hdr = s.hdr && gpu::HdrAvailable();
    static constexpr uint32_t kSdr[] = {1, 2}, kHdr[] = {3, 1, 2, 4};
    const uint32_t* steps = hdr ? kHdr : kSdr;
    const int count = hdr ? 4 : 2;
    int step = std::min(g_calib_step.load(), count - 1);
    const uint32_t mode = steps[step];
    gpu::g_calibrate.store(mode, std::memory_order_relaxed);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float font = ImGui::GetFontSize();
    const float margin = std::max(std::round(std::min(display.x, display.y) * 0.04f), font);
    const float width = std::min(display.x - margin * 2.0f, font * 46.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y - margin), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, display.y * 0.45f));
    static int shown = -1;
    if (shown != step) ImGui::SetNextWindowFocus();
    const bool arrived = shown != step;
    shown = step;
    if (ImGui::Begin("##calibrate", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 1.25f, st.FramePadding.y * 1.5f));
        ImGui::TextUnformatted(T("display_calibration"));
        char of[64];
        std::snprintf(of, sizeof of, T("step_of"), step + 1, count);
        ImGui::SameLine(width - st.WindowPadding.x - ImGui::CalcTextSize(of).x);
        ImGui::TextDisabled("%s", of);
        ImGui::Separator();
        g_first_row = true;
        const app::Settings def{};
        const char* hint = "";
        const char* label = mode == 1 ? "brightness" : mode == 2 ? "gamma" : mode == 3 ? "paper_white_nits" : "peak_brightness_nits";
        switch (mode) {
            case 1:
                Slider("brightness", &s.brightness, -20, 20, def.brightness);
                hint = "hint_calibrate_black";
                break;
            case 2:
                Slider("gamma", &s.gamma, 180, 260, def.gamma, 100);
                hint = "hint_calibrate_gamma";
                break;
            case 3:
                DisplaySlider("paper_white_nits", &s.hdr_paper_white, app::kPaperWhiteMin, app::kPaperWhiteMax,
                              app::HdrPaperWhite());
                hint = "hint_calibrate_paper_white";
                break;
            default:
                DisplaySlider("peak_brightness_nits", &s.hdr_peak, app::kPeakMin, app::kPeakMax, app::HdrPeak());
                hint = "hint_calibrate_peak";
                break;
        }
        g_first_row = false;
        // the pad's cursor starts on the slider (Slider's own ID under the label), not activated
        if (arrived) {
            ImGui::PushID(label);
            ImGui::SetFocusID(ImGui::GetID("##slider"), ImGui::GetCurrentWindow());
            ImGui::PopID();
        }
        RichText(T(hint), width - st.WindowPadding.x - ImGui::GetCursorPosX(), true);
        ImGui::Dummy(ImVec2(0.0f, font * 0.4f));
        // Back to the step before, or to the menu from the first; Next, Done on the last
        const float bw = std::max({ImGui::CalcTextSize(T("previous")).x, ImGui::CalcTextSize(T("next")).x,
                                   ImGui::CalcTextSize(T("done")).x}) + st.FramePadding.x * 4.0f;
        if (ImGui::Button(T("previous"), ImVec2(bw, 0.0f))) step--;
        ImGui::SameLine(width - st.WindowPadding.x - bw);
        if (ImGui::Button(T(step + 1 < count ? "next" : "done"), ImVec2(bw, 0.0f))) step = step + 1 < count ? step + 1 : -1;
        ImGui::PopStyleVar();
        DrawWindowFrame(1.0f);
    }
    ImGui::End();
    // unless Esc (the window thread) went back to the menu meanwhile
    if (g_calib_step.load() >= 0) g_calib_step.store(step);
    if (g_calib_step.load() < 0) shown = -1, gpu::g_calibrate.store(0, std::memory_order_relaxed);
}

// the toast for each achievement as it is earned: a card at the bottom's middle, framed and shadowed as the menu's
// window, that glides up and fades in, stays, then sinks a little as it fades out. It moves by fractions of a pixel
// (whole pixels stepped visibly at the slow end of the ease) and settles on whole ones
void DrawAchievementToast() {
    static int showing = -1;
    static double since = 0.0;
    constexpr double kIn = 0.5, kStay = 5.0, kOut = 0.5;
    const double now = ImGui::GetTime();
    if (showing >= 0 && now - since > kIn + kStay + kOut) showing = -1;
    if (showing < 0) {
        showing = achievements::NextToast();
        since = now;
        if (showing < 0) return;
        if (app::Get().achievement_sound) audio::Play(audio::Cue::Achievement);  // as it appears, whatever menu_sounds says: it is no menu sound
    }
    const double t = now - since;
    float alpha = 1.0f, offset = 0.0f;  // offset: how far below its place, in font heights
    if (t < kIn) {
        const float u = float(t / kIn), out = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);  // ease out (cubic)
        alpha = std::min(u * 1.6f, 1.0f);
        offset = (1.0f - out) * 1.6f;
    } else if (t > kIn + kStay) {
        const float u = std::min(float((t - kIn - kStay) / kOut), 1.0f), in = u * u * u;  // ease in (cubic)
        alpha = 1.0f - u * u * (3.0f - 2.0f * u);
        offset = in * 0.6f;
    }

    const achievements::Info& info = achievements::Get(showing);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float font = std::round(ImGui::GetFontSize() * 1.6f), small = std::round(ImGui::GetFontSize() * 1.1f);
    ImFont* f = ImGui::GetFont();
    const char* head = T("achievement_unlocked");
    const ImVec2 head_size = f->CalcTextSizeA(small, FLT_MAX, 0.0f, head);
    const ImVec2 name_size = f->CalcTextSizeA(font, FLT_MAX, 0.0f, info.name);
    const float pad = std::round(font * 0.9f), gap = std::round(font * 0.2f);
    // the description under the name, wrapped at the wider of the name and about 26 of its own lines' worth
    const float text_max = display.x - 32.0f - pad * 2.0f;
    const float wrap = std::min(std::max(name_size.x, small * 26.0f), text_max);
    const char* description = info.description[Column()];
    const ImVec2 desc_size = f->CalcTextSizeA(small, FLT_MAX, wrap, description);
    const float text_w = std::min(std::max({head_size.x, name_size.x, desc_size.x}), text_max);
    const float w = std::round(pad * 2.0f + text_w);
    const float h = std::round(head_size.y + gap + name_size.y + gap + desc_size.y + pad * 1.5f);
    const float x = std::round((display.x - w) * 0.5f);
    const float rest = std::round(display.y - h - font * 2.0f);
    const float y = offset > 0.0f ? rest + offset * font : rest;
    const ImVec2 lo(x, y), hi(x + w, y + h);
    const float round = style.WindowRounding;
    ImVec4 bg = style.Colors[ImGuiCol_WindowBg];  // the menu window's background
    bg.w = std::max(bg.w, 0.92f) * alpha;
    if (g_decorations) DrawFrame(dl, lo, hi, round, alpha);  // under the card: its shadow reaches outside it
    dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(bg), round);
    if (g_decorations) {  // the lines again over the fill
        dl->AddRect(ImVec2(lo.x - 1.0f, lo.y - 1.0f), ImVec2(hi.x + 1.0f, hi.y + 1.0f), IM_COL32(0, 0, 0, int(255 * alpha)),
                    round + 1.0f, 0, 1.0f);
        dl->AddRect(lo, hi, IM_COL32(255, 255, 255, int(24 * alpha)), round, 0, 1.0f);
    } else {
        dl->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_Border, alpha), round, 0, 1.0f);
    }
    const float tx = x + pad, ty = y + std::round((h - head_size.y - gap - name_size.y - gap - desc_size.y) * 0.5f);
    const ImVec4 accent = style.Colors[ImGuiCol_CheckMark];
    dl->PushClipRect(lo, ImVec2(hi.x - pad * 0.5f, hi.y), true);
    dl->AddText(f, small, ImVec2(tx, ty), ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, alpha)), head);
    dl->AddText(f, font, ImVec2(tx, ty + head_size.y + gap), IM_COL32(255, 255, 255, int(245 * alpha)), info.name);
    const ImVec4 dim = style.Colors[ImGuiCol_TextDisabled];
    dl->AddText(f, small, ImVec2(tx, ty + head_size.y + gap + name_size.y + gap),
                ImGui::ColorConvertFloat4ToU32(ImVec4(dim.x, dim.y, dim.z, dim.w * alpha)), description, nullptr, wrap);
    dl->PopClipRect();
}

}  // namespace

// for the overlay drawn elsewhere: subtitles.cpp's how-to panels take "press_key"
const char* Text(const char* id) { return T(id); }

void Draw(double fps) {
    static bool was_open = false;
    static bool titled = false;
    static int last_look = -1;
    MenuStyle();
    chat::Draw(T("team"), T("all"));  // under the menu
    subtitles::Draw();  // the speech bubbles as subtitles, under the menu too
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
    if (!std::exchange(titled, true)) app::Post(kTitle);

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
        const char* label = T("frozen_f7_resumes");
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
        const char* label = T("hold_to_skip");
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
        // the ring runs just outside the pill, so its stroke never covers the content
        const float out = thick * 0.5f, rr = r + out, top = lo.y - out, bot = hi.y + out;
        const float s = w - h, pi = 3.14159265f, cap = pi * rr, total = 2.0f * s + 2.0f * cap;
        auto at = [&](float t) {
            if (t < s * 0.5f) return ImVec2(c.x + t, top);
            t -= s * 0.5f;
            if (t < cap) {
                const float a = -pi * 0.5f + t / rr;
                return ImVec2(c.x + s * 0.5f + rr * std::cos(a), c.y + rr * std::sin(a));
            }
            t -= cap;
            if (t < s) return ImVec2(c.x + s * 0.5f - t, bot);
            t -= s;
            if (t < cap) {
                const float a = pi * 0.5f + t / rr;
                return ImVec2(c.x - s * 0.5f + rr * std::cos(a), c.y + rr * std::sin(a));
            }
            t -= cap;
            return ImVec2(c.x - s * 0.5f + t, top);
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

    // the shader warm-up at work (gpu::shader_record::g_work): a pill at the bottom left, a turning arc and how far
    // it has come, so a first run's slower frames have a reason on screen. Compiling shows once it has run a second
    // (a run with everything cached is through before then, and needs no word); finding the disc's shaders, only
    // ever a first run's, at once
    const auto work = gpu::shader_record::g_work.load();
    static double work_since = -1.0;
    if (work == gpu::shader_record::Work::None) work_since = -1.0;
    else if (work_since < 0.0) work_since = ImGui::GetTime();
    const bool shown = work == gpu::shader_record::Work::Generating ||
                       (work != gpu::shader_record::Work::None && ImGui::GetTime() - work_since >= 1.0);
    if (shown && !quit) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float font = ImGui::GetFontSize() * 1.1f;
        char label[96];
        if (work == gpu::shader_record::Work::Generating) {
            std::snprintf(label, sizeof label, "%s", T("finding_shaders"));
        } else {
            const uint32_t total = std::max(gpu::shader_record::g_work_total.load(), 1u);
            const uint32_t done = std::min(gpu::shader_record::g_work_done.load(), total);
            std::snprintf(label, sizeof label, T("compiling_shaders_d"), int(uint64_t(done) * 100 / total));
        }
        const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, label);
        const float h = font * 2.0f, ring = font * 0.45f, gap = font * 0.6f, margin = font * 1.6f;
        const float w = h * 0.5f + ring * 2.0f + gap + text.x + h * 0.5f;
        const ImVec2 lo(std::round(margin), std::round(display.y - margin - h));
        const ImVec2 hi(std::round(margin + w), std::round(display.y - margin));
        // framed and shadowed as the menu's controls (the achievement toast's way), in the menu window's background
        const float round = h * 0.5f;
        ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        bg.w = std::max(bg.w, 0.92f);
        if (g_decorations) DrawFrame(dl, lo, hi, round, 1.0f);  // under the pill: its shadow reaches outside it
        dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(bg), round);
        if (g_decorations) {  // the lines again over the fill
            dl->AddRect(ImVec2(lo.x - 1.0f, lo.y - 1.0f), ImVec2(hi.x + 1.0f, hi.y + 1.0f), IM_COL32(0, 0, 0, 255),
                        round + 1.0f, 0, 1.0f);
            dl->AddRect(lo, hi, IM_COL32(255, 255, 255, 24), round, 0, 1.0f);
        } else {
            dl->AddRect(lo, hi, ImGui::GetColorU32(ImGuiCol_Border), round, 0, 1.0f);
        }
        const ImVec2 c(lo.x + h * 0.5f + ring, lo.y + h * 0.5f);
        const float thick = font * 0.14f, a0 = float(ImGui::GetTime()) * 5.0f;
        dl->PathArcTo(c, ring, 0.0f, 6.2831853f, 32);
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_Text, 0.25f), ImDrawFlags_None, thick);
        dl->PathArcTo(c, ring, a0, a0 + 4.2f, 24);
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark), ImDrawFlags_None, thick);
        dl->AddText(ImGui::GetFont(), font, ImVec2(c.x + ring + gap, c.y - text.y * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), label);
    }

    DrawAchievementToast();

    // the prompt alone: a modal over the game, which is held. It goes when Cancel, B or Esc shut it
    if (quit && !open) {
        if (!ImGui::IsPopupOpen("Quit?")) ImGui::OpenPopup("Quit?");
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        bool up = false;
        // the menu window's background, its translucency too (a popup's is more opaque)
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        if (ImGui::BeginPopupModal("Quit?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
            up = QuitBody();
            DrawWindowFrame(1.0f);
            if (!up) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        // the prompt sounds like the menu
        static bool was_quit = false;
        if (!open) Cues(was_quit, false);
        was_quit = up;
        if (!up) {
            SetQuit(false);
            app::Post(mouse::kRefresh);
        }
    }
    if (!open) {
        g_calib_step.store(-1);
        gpu::g_calibrate.store(0, std::memory_order_relaxed);
        return;
    }

    app::Settings& s = app::Get();
    static bool calibrated = false;  // the menu's window takes the focus back from the calibration
    if (g_calib_step.load() >= 0) {
        DrawCalibration(s);
        Cues(opened_before, settings_changed);
        calibrated = true;
        return;
    }
    gpu::g_calibrate.store(0, std::memory_order_relaxed);
    // the whole display but for a margin; the mouse can still reach every edge. Resizable (Debug): it starts there,
    // then stays where it is dragged or sized to (by its edges, or the empty space in it), for the session
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui::GetFontSize() / 13.0f;  // the minimums were set with ImGui's 13 px font
    const float margin = std::max(std::round(std::min(display.x, display.y) * 0.04f), 12.0f * scale);
    const ImGuiCond place = MenuResizable() ? ImGuiCond_Once : ImGuiCond_Always;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), place);
    ImGui::SetNextWindowSize(ImVec2(std::max(display.x - margin * 2.0f, 1.0f), std::max(display.y - margin * 2.0f, 1.0f)),
                             place);
    if (MenuResizable()) ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * scale, 200.0f * scale), display);
    if (!was_open || std::exchange(calibrated, false)) ImGui::SetNextWindowFocus();
    // the menu fades in with the blur behind it (vulkan.cpp Redisplay: the same time and curve)
    static double opened_at = 0.0;
    if (!opened_before) opened_at = ImGui::GetTime();
    float fade = std::min(float((ImGui::GetTime() - opened_at) / 0.11), 1.0f);
    fade = fade * fade * (3.0f - 2.0f * fade);
    g_fade = fade;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    bool keep_open = true;
    const ImGuiWindowFlags fixed = MenuResizable() ? 0 : ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
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
            // the footer: its separator right under the page (the page ends on it, its own padding at its bottom), then the line
            const float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + 2.0f;
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
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));  // its label is drawn with the pills, once (below)
                const std::string label = std::string(T(name)) + "##" + name;  // the English name is the ID
                // roomier tabs: ImGui sizes a tab from the frame padding as it is begun
                ImGui::PushStyleVarX(ImGuiStyleVar_FramePadding, ImGui::GetStyle().FramePadding.x * 1.4f);
                const bool selected =
                    ImGui::BeginTabItem(label.c_str(), nullptr, target == mine ? ImGuiTabItemFlags_SetSelected : 0);
                ImGui::PopStyleVar();
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
                if (ImGui::IsItemFocused()) g_tab_focused = true;
                const bool arrived = Arrived(g_main_focus) && !selected;
                if (arrived) g_tab_target.store(mine), g_tab_arrived = true;
                // drawn once the tabs are all in (below), when the one that is becoming selected is known
                pills[mine] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), T(name),
                               ImGui::IsItemHovered()};
                if (selected) shown = mine;
                if (!selected && (arrived || target == mine || ImGui::IsItemClicked())) pending = mine;
                if (!selected) return false;
                g_tab_now = mine;
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.6f));  // air under the tabs, above their separator
                {
                    // the separator under the tab row, the window's whole width
                    ImGuiWindow* w = ImGui::GetCurrentWindow();
                    const float y = std::round(ImGui::GetCursorScreenPos().y);
                    w->DrawList->PushClipRect(ImVec2(w->Pos.x, y - 1.0f), ImVec2(w->Pos.x + w->Size.x, y + 2.0f));
                    w->DrawList->AddLine(ImVec2(w->Pos.x, y + 0.5f), ImVec2(w->Pos.x + w->Size.x, y + 0.5f),
                                         ImGui::GetColorU32(ImGuiCol_Separator));
                    w->DrawList->PopClipRect();
                    ImGui::Dummy(ImVec2(0.0f, 1.0f));
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);  // the page starts on it
                }
                // a page fades in as its tab is picked (the one before is already gone: only the shown tab is submitted)
                static int faded_tab = -1;
                static double faded_at = 0.0;
                if (faded_tab != mine) faded_tab = mine, faded_at = ImGui::GetTime();
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(float((ImGui::GetTime() - faded_at) / 0.15), 1.0f) * g_fade);
                // from the window's left edge to its right, with no padding of its own (InsetOn gives the rows theirs)
                ImGui::SetCursorPosX(0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::BeginChild("##page", ImVec2(ImGui::GetWindowWidth(), -footer), ImGuiChildFlags_NavFlattened);
                ImGui::PopStyleVar();
                g_inset_on = false;
                InsetOn();
                ImGui::SetCursorPosY(std::round(RowInset() * 0.5f));  // the first card as far under the tab row's separator as it is from the sides
                if (g_pad.scroll != 0.0f) ImGui::SetScrollY(ImGui::GetScrollY() + g_pad.scroll);
                return true;
            };
            auto end_tab = [] {
                EndSection();
                g_any_section = false;
                g_section_open = false;
                g_first_row = true;
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 1.5f));  // space below the last row when scrolled to the end
                InsetOff();
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::EndTabItem();
            };

            if (tab("general")) {
                if (Section("language")) {
                    {
                        // the languages the game has text for (aid/text), in XC_LANGUAGE order; the menu has the same six
                        static const char* const kKeys[] = {"english", "japanese", "german", "french", "spanish", "italian"};
                        constexpr int kCount = int(std::size(kKeys));
                        auto pick = [&](const char* label, std::string& setting) {
                            int lang = 0;
                            for (int i = 0; i < kCount; i++)
                                if (setting == kKeys[i]) lang = i;
                            // segments, or a dropdown where the six names don't fit the room
                            if (Segmented(label, &lang, {"English", "日本語", "Deutsch", "Français", "Español", "Italiano"})) setting = kKeys[lang];
                        };
                        pick("ui", s.ui_language);
                        Hint("hint_the_language_of_this_menu");
                        pick("game_text", s.language);
                        Hint("hint_text_only_requires_restart_voices");
                        static const std::string start_language = s.language;
                        if (s.language != start_language) WarningCard("hint_restart_the_game_for_this");
                    }
                }
                if (Section("appearance")) {
                    {
                        Beside("ui_accent_color");  // its label (with the hint under it), the swatches in the value column
                        const float size = ImGui::GetFrameHeight() * 1.1f, gap = size * 0.4f;
                        for (uint32_t i = 0; i < 7; i++) {
                            if (i) ImGui::SameLine(0.0f, gap);
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
                        Hint("hint_the_color_of_the_selection");
                    }
                    Check("menu_decorations", &g_decorations);
                    Hint("hint_a_fine_light_and_dark", true);
                }
                end_tab();
            }

            if (tab("graphics")) {
                if (Section("display")) {
                    int mode = int(s.display_mode);
                    if (Segmented("mode", &mode, {"fullscreen", "borderless", "windowed"})) {
                        s.display_mode = app::DisplayMode(mode);
                        app::ApplyDisplay();
                    }
                    Hint("hint_how_the_game_window_is_placed");

                    const std::vector<app::Mode>& modes = Modes();
                    std::string res = T("desktop");
                    int res_idx = 0;
                    for (size_t i = 0; i < modes.size(); i++) {
                        res += '\0';
                        res += std::to_string(modes[i].width) + "x" + std::to_string(modes[i].height);
                        if (modes[i].width == s.width && modes[i].height == s.height) res_idx = int(i) + 1;
                    }
                    res += '\0';
                    if (ImGui::Combo(Beside("resolution"), &res_idx, res.c_str())) {
                        if (res_idx == 0) s.width = s.height = 0;
                        else s.width = modes[res_idx - 1].width, s.height = modes[res_idx - 1].height;
                        app::ApplyDisplay();
                    }
                    Hint("hint_the_size_of_the_picture_in");
                    // the camera follows the setting live; only the TV type the console reports waits for a restart
                    const char* aspects[] = {"auto", "4:3", "16:10", "16:9", "21:9", "32:9"};
                    constexpr int kAspects = 6;
                    int aspect = -1;
                    for (int i = 0; i < kAspects; i++)
                        if (s.aspect == aspects[i]) aspect = i;
                    std::string items;
                    for (int i = 0; i < kAspects; i++) items += std::string(i ? aspects[i] : T("monitor")) + '\0';
                    if (aspect < 0) items += s.aspect + '\0', aspect = kAspects;  // a custom W:H from settings.toml
                    items += '\0';
                    if (ImGui::Combo(Beside("aspect_ratio"), &aspect, items.c_str()) && aspect < kAspects) s.aspect = aspects[aspect];
                    Hint("hint_the_shape_of_the_picture");

                    {
                        const std::vector<std::string>& adapters = gpu::Adapters();
                        if (adapters.size() > 1 || !s.gpu.empty()) {
                            if (ImGui::BeginCombo(Beside("graphics_adapter"), s.gpu.empty() ? T("automatic") : s.gpu.c_str())) {
                                if (ImGui::Selectable(T("automatic"), s.gpu.empty())) s.gpu.clear();
                                for (const std::string& a : adapters)
                                    if (ImGui::Selectable(a.c_str(), a == s.gpu)) s.gpu = a;
                                ImGui::EndCombo();
                            }
                            Hint("hint_the_graphics_card_the_game");
                        }
                    }
                    {
                        // read once at launch (gpu::CreateBackend, the window made for it); D3D11 on Windows alone
#ifdef _WIN32
                        static const char* const kRenderers[] = {"vulkan", "d3d11", "opengl"};
                        constexpr const char* kRendererHint = "hint_vulkan_is_the_default_direct3d_opengl";
#else
                        static const char* const kRenderers[] = {"vulkan", "opengl"};
                        constexpr const char* kRendererHint = "hint_vulkan_is_the_default_opengl";
#endif
                        int renderer = 0;
                        for (int i = 0; i < int(std::size(kRenderers)); i++)
                            if (s.renderer == kRenderers[i]) renderer = i;
#ifdef _WIN32
                        if (Segmented("renderer", &renderer, {"Vulkan", "Direct3D 11", "OpenGL"}))
#else
                        if (Segmented("renderer", &renderer, {"Vulkan", "OpenGL"}))
#endif
                            s.renderer = kRenderers[renderer];
                        Hint(kRendererHint);
                        static const std::string start_renderer = s.renderer;
                        if (s.renderer != start_renderer) WarningCard("hint_restart_the_game_for_this");
                    }

                }

                if (Section("tone_and_calibration")) {
                    {
                        // offered once the swap chain found an HDR10 format; kept clickable while on, to turn it off
                        bool can = gpu::HdrAvailable() || s.hdr;
                        ImGui::BeginDisabled(!can);
                        Check("hdr", &s.hdr);
                        ImGui::EndDisabled();
                        Hint(can ? "hint_sends_the_picture_to_the"
                                 : "hint_the_display_or_desktop_offers", true);
                        if (s.hdr) {
                            DisplaySlider("paper_white_nits", &s.hdr_paper_white, app::kPaperWhiteMin,
                                          app::kPaperWhiteMax, app::HdrPaperWhite());
                            Hint("hint_how_bright_white_is_shown", true);
                            Slider("highlights", &s.hdr_highlights, 0, 100, app::Settings{}.hdr_highlights);
                            Hint("hint_makes_the_brightest_parts_of", true);
                            if (s.hdr_highlights) {
                                DisplaySlider("peak_brightness_nits", &s.hdr_peak, app::kPeakMin, app::kPeakMax,
                                              app::HdrPeak());
                                Hint("hint_how_bright_the_brightest_highlights", true);
                            }
                        }
                    }
                    Slider("gamma", &s.gamma, 180, 260, app::Settings{}.gamma, 100);
                    Hint("hint_higher_darkens_shadows_and_midtones", true);
                    Slider("brightness", &s.brightness, -20, 20, app::Settings{}.brightness);
                    Hint("hint_raises_or_lowers_the_black", true);
                    Beside("display_calibration");
                    ImGui::BeginDisabled(D3d11());
                    if (ImGui::Button(T("calibrate_display"))) g_calib_step.store(0);
                    ImGui::EndDisabled();
                    Hint(D3d11() ? NoVulkan() : "hint_test_patterns_for_brightness_and", true);
                }

                if (Section("frame_rate")) {
                    constexpr uint32_t kRates[] = {30, 60, 120, 180, 240};
                    int rate = 0;
                    // past the first rate to reach the monitor's refresh, greyed out (a setting there runs at it)
                    const uint32_t most = display::MostFrameRate();
                    uint32_t off = 0;
                    for (int i = 0; i < 5; i++) {
                        if (kRates[i] > most) off |= 1u << i;
                        if (kRates[i] == std::min(display::g_frame_rate, most)) rate = i;
                    }
                    // fixed while a system link session has another machine in it: every player's game must run at the same rate (link.cpp)
                    const bool locked = display::FrameRateLocked();
                    ImGui::BeginDisabled(locked);
                    if (Segmented("frame_rate", &rate, {"30_original", " 60", " 120", " 180", " 240"}, off))
                        display::SetFrameRate(kRates[rate]);
                    ImGui::EndDisabled();
                    Hint(locked ? "hint_can_t_be_changed_during" :
                                  "hint_rates_past_the_monitor_s", true);
                    // forced on past the monitor's refresh (app::VsyncForced): shown on and greyed, the setting kept
                    if (app::VsyncForced(app::Get())) {
                        bool on = true;
                        ImGui::BeginDisabled();
                        Check("v_sync", &on);
                        ImGui::EndDisabled();
                        Hint("hint_always_on_while_the_frame", true);
                    } else {
                        Check("v_sync", &s.vsync);
                        Hint("hint_waits_for_the_display_to", true);
                    }
                    // past the monitor's refresh rate with vsync the pictures go one a refresh (VulkanBackend::InStep)
                    // (vsync forced on there)
                    const app::Settings& now = app::Get();
                    if (now.frame_generation > 1 && app::VsyncForced(now)) {
                        int timing = s.picture_timing_exact ? 1 : 0;
                        ImGui::BeginDisabled(D3d11());
                        if (Segmented("picture_timing", &timing, {"lighter", "exact"}))
                            s.picture_timing_exact = timing == 1;
                        ImGui::EndDisabled();
                        Hint(D3d11() ? NoVulkan() : "hint_above_the_refresh_rate_lighter", true);
                    }
                    // the pictures between frames fewer while they keep the game from its rate (VulkanBackend::AutoInterp)
                    if (now.frame_generation > 1) {
                        ImGui::BeginDisabled(D3d11());
                        Check("adaptive_frame_interpolation", &s.adaptive_interp);
                        ImGui::EndDisabled();
                        Hint(D3d11() ? NoVulkan() : "hint_adaptive_frame_interpolation", true);
                    }
                    Check("fps_counter", &g_fps_overlay);
                    Hint("hint_shows_the_frame_rate_in", true);
                }

                // the picture's size and what rebuilds it; hoisted so the quality section sees what runs even folded
                const int dlss_state = gpu::g_dlss_state.load();
                const int fsr3_state = gpu::g_fsr3_state.load();
                int pick = s.upscaler == "dlss" ? 3 : s.upscaler == "fsr3" ? 2 : s.fsr ? 1 : 0;
                bool dlss = pick == 3;
                bool fsr3_runs = pick == 2 && fsr3_state != 0 && !D3d11();
                if (Section("upscaling")) {
                    // Upscaling, one setting: none, FSR 1.0 (app::Settings::fsr, over the render size), or FSR 3.1 or
                    // DLSS (app::Settings::upscaler), which set the render size themselves. A choice the GPU or
                    // renderer can't run is greyed out unless it is the one chosen (so it can be left); a renderer
                    // without either (D3D11, OpenGL) draws as if none were chosen, the setting kept, their rows left
                    // out. The hint says what the choice does, or why it can't
                    const uint32_t greyed = (pick != 3 && dlss_state != 1 ? 8u : 0u) |
                                            (pick != 2 && (fsr3_state != 1 || D3d11()) ? 4u : 0u);
                    if (Segmented("upscaling", &pick,
                                  {"upscaler_none", "upscaler_fsr1", "upscaler_fsr3", "upscaler_dlss"}, greyed)) {
                        s.upscaler = pick == 3 ? "dlss" : pick == 2 ? "fsr3" : "off";
                        s.fsr = pick == 1;
                    }
                    const char* why = nullptr;
                    if (pick == 3)
                        why = dlss_state == 4   ? NoVulkan()
                              : dlss_state == 3 ? "hint_the_dlss_files_aren_t"
                              : dlss_state == 2 ? "hint_dlss_failed_to_start_on"
                              : dlss_state == 1 ? "hint_dlss_renders_fewer_pixels_and"
                                                : "hint_dlss_needs_an_nvidia_rtx";
                    else if (pick == 2)
                        why = D3d11()             ? NoVulkan()
                              : fsr3_state == 2   ? "hint_fsr_3_failed_to_start"
                              : fsr3_state == 1   ? "hint_dlss_renders_fewer_pixels_and"
                                                  : "hint_fsr_3_needs_more_of_the_gpu";
                    else if (pick == 1)
                        why = "hint_amd_fidelityfx_super_resolution_edge";
                    else
                        why = dlss_state == 3   ? "hint_the_dlss_files_aren_t"
                              : dlss_state == 2 ? "hint_dlss_failed_to_start_on"
                                                : "hint_upscaling_none";
                    Hint(why, true);
                    dlss = pick == 3;
                    fsr3_runs = pick == 2 && fsr3_state != 0 && !D3d11();
                    if (fsr3_runs) {
                        int quality = int(std::min<uint32_t>(s.upscaler_quality, 5));
                        if (Segmented("fsr3_quality", &quality,
                                      {"native_aa", "quality", "balanced", "performance", "ultra_performance", "custom"}))
                            s.upscaler_quality = uint32_t(quality);
                        Hint("hint_how_many_pixels_fsr_3_renders", quality != 5);
                        if (quality == 5) {  // Custom: the render size's share, past the modes' too
                            Slider("render", &s.upscaler_percent, 10, 100, app::Settings{}.upscaler_percent);
                            Hint("hint_the_size_the_game_is", true);
                        }
                        Slider("dlss_sharpness", &s.dlss_sharpness, 0, 100, app::Settings{}.dlss_sharpness);
                        Hint("hint_rcas_sharpening_over_fsr_3", true);
                        if (s.dlss_sharpness > 0) {
                            Slider("fur_sharpness", &s.fur_sharpness, 0, 100, app::Settings{}.fur_sharpness);
                            Hint("hint_how_much_of_fsr_s", true);
                        }
                    } else if (dlss && dlss_state != 4) {
                        int quality = int(std::min<uint32_t>(s.upscaler_quality, 5));
                        if (Segmented("dlss_quality", &quality,
                                      {"dlaa", "quality", "balanced", "performance", "ultra_performance", "custom"}))
                            s.upscaler_quality = uint32_t(quality);
                        Hint("hint_how_many_pixels_dlss_renders", quality != 5);
                        if (quality == 5) {  // Custom: the render size's share, past the modes' too
                            Slider("render", &s.upscaler_percent, 10, 100, app::Settings{}.upscaler_percent);
                            Hint("hint_the_size_the_game_is", true);
                        }
                        int model = int(std::min<uint32_t>(s.dlss_model, 3));
                        if (Segmented("dlss_model", &model, {"K", "M", "L", "by_quality"})) s.dlss_model = uint32_t(model);
                        Hint("hint_k_is_the_lightest_m", true);
                        Slider("dlss_sharpness", &s.dlss_sharpness, 0, 100, app::Settings{}.dlss_sharpness);
                        Hint("hint_rcas_sharpening_over_dlss_s", true);
                        if (s.dlss_sharpness > 0) {
                            Slider("fur_sharpness", &s.fur_sharpness, 0, 100, app::Settings{}.fur_sharpness);
                            Hint("hint_how_much_of_fsr_s", true);
                        }
                    } else {
                        Slider("render", &s.render_percent, 25, 200, app::Settings{}.render_percent);
                        Hint("hint_the_size_the_game_is", pick != 1);
                        if (pick == 1) {
                            Slider("fsr_sharpness", &s.fsr_sharpness, 0, 100, app::Settings{}.fsr_sharpness);
                            Hint("hint_how_strongly_the_upscaled_picture");
                            Slider("fur_sharpness", &s.fur_sharpness, 0, 100, app::Settings{}.fur_sharpness);
                            Hint("hint_how_much_of_fsr_s", true);
                        }
                    }
                }

                if (Section("quality")) {
                    constexpr uint32_t kAniso[] = {0, 2, 4, 8, 16};
                    int aniso = 0;
                    for (int i = 0; i < 5; i++)
                        if (kAniso[i] == s.aniso) aniso = i;
                    if (Segmented("anisotropic", &aniso, {"game_default", " 2x", " 4x", " 8x", " 16x"}))
                        s.aniso = kAniso[aniso];
                    Hint("hint_keeps_textures_sharp_at_a");

                    // one choice: no anti-aliasing, SMAA, or MSAA at a sample count (the two never run together)
                    constexpr uint32_t kAa[] = {0, 1, 2, 4, 8};  // 0 off, 1 SMAA, else MSAA samples
                    // DLSS anti-aliases the picture itself: the choice is kept for when it is off
                    int aa = s.smaa ? 1 : s.msaa >= 8 ? 4 : s.msaa >= 4 ? 3 : s.msaa >= 2 ? 2 : 0;
                    ImGui::BeginDisabled(dlss || fsr3_runs);
                    if (Segmented("anti_aliasing", &aa, {"off", "SMAA", "MSAA 2x", "MSAA 4x", "MSAA 8x"})) {
                        s.smaa = aa == 1;
                        s.msaa = aa >= 2 ? kAa[aa] : 1;
                    }
                    ImGui::EndDisabled();
                    Hint(fsr3_runs ? "hint_fsr_3_anti_aliases_the_picture"
                         : dlss    ? "hint_dlss_anti_aliases_the_picture"
                                   : "hint_smooths_jagged_edges_smaa_is",
                         true);
                }

                if (Section("post_processing")) {
                    Check("vignette", &vignette::g_on);
                    Hint("hint_the_game_s_darkened_screen", true);
                    Slider("film_grain", &s.film_grain, 0, 100, app::Settings{}.film_grain);
                    Hint("hint_adds_a_fine_noise_over");
                    Slider("chromatic_aberration", &s.chromatic_aberration, 0, 100, app::Settings{}.chromatic_aberration);
                    Hint("hint_splits_colors_apart_toward_the");
                    Slider("ambient_occlusion", &s.ambient_occlusion, 0, 100, app::Settings{}.ambient_occlusion);
                    Hint("hint_darkens_creases_and_corners_where");
                    if (s.ambient_occlusion) {
                        Slider("ambient_occlusion_reach", &s.ambient_occlusion_radius, 5, 200,
                               app::Settings{}.ambient_occlusion_radius);
                        Hint("hint_how_far_from_a_surface");
                    }
                    Slider("motion_blur", &s.motion_blur, 0, 100, app::Settings{}.motion_blur);
                    Hint("hint_blurs_the_picture_as_the");
                }

                if (Section("lighting")) {
                    Check("per_pixel_lighting", &lighting::g_per_pixel);
                    Hint("hint_smoother_light_and_shadows_on", true);
                    const bool smoother = gpu::g_lightmap_smoothing.load(std::memory_order_relaxed);
                    ImGui::BeginDisabled(!smoother);
                    Check("smooth_baked_shadows", &s.smooth_lightmaps);
                    ImGui::EndDisabled();
                    Hint(D3d11() ? NoVulkan() : "hint_straightens_the_jagged_edges_of", true);
                    if (s.smooth_lightmaps && smoother) {
                        Slider("baked_shadow_smoothing", &s.lightmap_smoothing, 1, 10, app::Settings{}.lightmap_smoothing);
                        Hint("hint_how_far_along_an_edge", true);
                        constexpr uint32_t kSizes[] = {256, 512, 1024, 2048};
                        int size = 1;
                        for (int i = 0; i < 4; i++)
                            if (kSizes[i] == s.lightmap_size) size = i;
                        if (Segmented("baked_shadow_size", &size, {"256", " 512", " 1024", " 2048"}))
                            s.lightmap_size = kSizes[size];
                        Hint("hint_the_largest_a_smoothed_shadow", true);
                    }
                    int fog = int(s.fog);
                    if (Segmented("fog", &fog, {"enabled", "disabled", "tint_only"})) s.fog = app::Fog(fog);
                    Hint("hint_disabled_clears_the_haze_over", true);
                }

                if (Section("fur")) {
                    // kept in settings.toml as fur_strands (Super) and fur_shading (the legacy shading, Vanilla only): super
                    // fur lights its strands its own way (and takes the draws first anyway)
                    int fur_tech = fur::g_strands ? 1 : 0;
                    if (Segmented("fur_tech", &fur_tech, {"vanilla", "super"})) {
                        fur::g_strands = fur_tech == 1;
                        if (fur::g_strands) fur::g_shading = false;
                    }
                    if (fur::g_strands) fur::g_shading = false;
                    Hint("hint_vanilla_keeps_the_game_s");
                    if (!fur::g_strands) {
                        Check("improved_fur_shading", &fur::g_shading);
                        Hint("hint_makes_fur_look_more_voluminous", true);
                    }
                    // the per-model presets stay out of sight until asked for (not saved: closed on each start)
                    static bool customize_fur = false;
                    if (fur::g_strands) {
                        Check("customize_fur", &customize_fur);
                        Hint("hint_shows_the_settings_for_each", true);
                    }
                    if (fur::g_strands && customize_fur) {
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
                            Hint("hint_the_game_s_models_show");
                        } else {
                            std::string current;
                            for (const auto& [name, type] : named)
                                if (type == model) current = label(name, type);
                            if (ImGui::BeginCombo(Beside("model"), current.c_str())) {
                                for (const auto& [name, type] : named)
                                    if (ImGui::Selectable(label(name, type).c_str(), type == model)) model = type;
                                ImGui::EndCombo();
                            }
                            Hint("hint_each_model_has_its_own", true);
                        }
                        if (fur::ModelLook* own = fur::ModelLookOf(model)) {
                            const fur::Look& d = fur::DefaultLook(model);
                            fur::Look& l = own->look;
                            ImGui::PushID(int(model));  // each model's sliders their own widgets, so a drag doesn't carry over
                            Check("vanilla_fur", &own->game);
                            if (fur::IsHair(model))
                                Hint("hint_the_hair_keeps_the_game", true);
                            else
                                Hint("hint_this_model_keeps_the_game", true);
                            if (fur::IsHair(model)) {  // its motion, whichever fur it has
                                Slider("sway", &l.sway, 0u, 200u, d.sway);
                                Hint("hint_how_much_the_hair_moves", true);
                                Slider("follows_motion", &l.motion, 0u, 200u, d.motion);
                                Hint("hint_how_far_the_hair_trails", true);
                            }
                            // the look in two columns, a pair to a row: each slider in the next cell
                            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                                                ImVec2(ImGui::GetFontSize() * 1.2f, ImGui::GetStyle().CellPadding.y));
                            const bool look = !own->game && ImGui::BeginTable("##look", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
                            ImGui::PopStyleVar();
                            if (look) {
                                const auto cell = [](auto&&... args) {
                                    ImGui::TableNextColumn();
                                    Slider(args...);
                                };
                                cell("extra_shells", &own->shells, 0u, fur::kMaxExtraShells, fur::DefaultShells(model));
                                cell("length", &l.length, 20u, 100u, d.length);
                                cell("strands", &l.strands, 8u, 64u, d.strands);
                                cell("tufts", &l.tufts, 1u, 16u, d.tufts);
                                cell("tufting", &l.tufting, 0u, 100u, d.tufting);
                                cell("droop", &l.droop, 0u, 200u, d.droop);
                                cell("thickness", &l.thickness, 30u, 100u, d.thickness);
                                cell("taper", &l.taper, 20u, 150u, d.taper);
                                cell("root_shade", &l.root_shade, 0u, fur::kMaxShade, d.root_shade);
                                cell("tuft_shade", &l.gap_shade, 0u, fur::kMaxShade, d.gap_shade);
                                cell("sheen", &l.sheen, 0u, 200u, d.sheen);
                                cell("back_glow", &l.back_glow, 0u, 200u, d.back_glow);
                                cell("bare_skin_fill", &l.fill, 0u, 100u, d.fill);
                                cell("fur_brightness", &l.tone, 50u, 200u, d.tone);
                                cell("strays", &l.stray, 0u, 30u, d.stray);
                                cell("stray_length", &l.stray_length, 100u, 250u, d.stray_length);
                                ImGui::EndTable();
                            }
                            ImGui::PopID();
                        }
                    }
                    // with DLSS or without: under DLSS it rebuilds the strands from the jitter, without it the
                    // coarser shading shows only while the frame rate needs it
                    if (fur::g_strands) {
                        ImGui::BeginDisabled(D3d11());
                        Check("adaptive_fur_shading", &s.dlss_fur_auto);
                        ImGui::EndDisabled();
                        Hint(D3d11() ? NoVulkan() : "hint_shades_super_fur_coarser_when", true);
                    }
                    Check("full_distant_fur", &fur::g_distant);
                    Hint("hint_keeps_fur_on_far_away", true);
                }

                if (Section("detail")) {
                    // one choice for the two settings (each kept in settings.toml as its own; full models with
                    // the console's animation, settable there, shows as the last)
                    int distant = characters::g_max_lod ? 2 : characters::g_smooth_animation ? 1 : 0;
                    if (Segmented("far_characters", &distant, {"original", "smooth_animation", "animation_and_model"}))
                        characters::g_smooth_animation = distant >= 1, characters::g_max_lod = distant == 2;
                    Hint("hint_far_characters", true);
                    Check("detailed_multiplayer_characters", &characters::g_menu_models);
                    Hint("hint_multiplayer_classes_use_their_more", true);
                    // one choice for the three grass settings (each still kept in settings.toml as its own)
                    bool high = grass::g_wind && grass::g_max_lod && !grass::g_fade;
                    if (Check("high_quality_grass", &high)) grass::g_wind = grass::g_max_lod = high, grass::g_fade = !high;
                    Hint("hint_faraway_grass_sways_stays_thick", true);
                    Check("dynamic_water", &water::g_dynamic);
                    Hint("hint_water_ripples_and_makes_waves", true);
                    Check("floor_depth_of", &reflection::g_floor_depth);
                    Hint("hint_fixes_the_blur_on_shiny", true);
                    // the redrawn pictures are bound by the Vulkan renderer alone (gpu::Replacement)
                    ImGui::BeginDisabled(D3d11());
                    Check("redrawn_hud", &hud_art::g_on);
                    ImGui::EndDisabled();
                    Hint(D3d11() ? NoVulkan() : "hint_sights_and_other_hud_pictures", true);
                }
                end_tab();
            }

            if (tab("tweaks")) {
                if (Section("camera")) {
                    Slider("field_of_view", &display::g_fov, display::kFovMin, display::kFovMax, display::kFov);
                    Hint("hint_how_wide_the_view_is");
                    Slider("extra_look_up", &display::g_look_up, 0, display::kLookUpMax, 0);
                    Hint("hint_lets_the_third_person_camera");
                }

                if (Section("videos")) {
                    Check("skip_intro_fmvs", &menus::g_skip_intro);
                    Hint("hint_the_movies_before_the", true);
                    bool no_attract = !menus::g_attract;
                    if (Check("disable_attract_video", &no_attract)) menus::g_attract = !no_attract;
                    Hint("hint_the_main_menu_no_longer", true);
                }

                if (Section("keyboard_and_mouse")) {
                    Check("keyboard_button_prompts", &prompts::g_on);
                    Hint("hint_while_you_play_with_the", true);
                }

                if (Section("fonts")) {
                    FontRow("title_font", game_fonts::kTitle, "hint_the_big_titles_of_the");
                    FontRow("text_font", game_fonts::kText, "hint_the_text_of_the_menus");
                }

                if (Section("gameplay")) {
                    Check("skippable_cutscenes", &cutscenes::g_skippable);
                    Hint("hint_hold_any_button_but_pause", true);
                    Check("speech_bubbles_in", &cutscenes::g_english_bubbles);
                    Hint("hint_conker_s_monologues_on_the", true);
                    // one of three: the game's bubbles, the runtime's (redrawn_bubbles) or subtitles (speech_subtitles,
                    // which comes first where an older settings file has both)
                    int speech = subtitles::g_on ? 2 : subtitles::g_redraw ? 1 : 0;
                    if (Segmented("speech", &speech, {"game_bubbles", "redrawn_bubbles", "subtitles"})) {
                        subtitles::g_on = speech == 2;
                        subtitles::g_redraw = speech == 1;
                    }
                    Hint(speech == 2   ? "hint_what_characters_say_and_think"
                         : speech == 1 ? "hint_redrawn_bubbles"
                                       : "hint_game_bubbles",
                         true);
                    Check("fade_instead_of", &loading::g_fade);
                    Hint("hint_loads_fade_to_black_and", true);
                    Check("rope_physics", &rope::g_physics);
                    Hint("hint_ropes_swing_realistically_when_conker", true);
                }
                end_tab();
            }

            if (tab("audio")) {
                if (Section("general")) {
                    Slider("volume", &s.volume, 0, 100, app::Settings{}.volume);
                    {
                        static std::vector<audio::Device> devices;
                        if (devices.empty() || ImGui::IsWindowAppearing()) devices = audio::Devices();
                        const std::string system_default = T("system_default");
                        std::string current = system_default;
                        for (const audio::Device& d : devices)
                            if (d.id == s.audio_device) current = d.name;
                        if (!s.audio_device.empty() && current == system_default)
                            current += std::string(" ") + T("selected_device_missing");
                        if (ImGui::BeginCombo(Beside("output_device"), current.c_str())) {
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
                        if (Segmented("speakers", &speakers, {"stereo", "5_1_surround"})) s.surround = speakers == 1;
                        Hint("hint_stereo_mixes_for_two_speakers");
                        static const bool start_surround = s.surround;
                        if (s.surround != start_surround) WarningCard("hint_restart_the_game_for_this");
                    }
                    Check("mute_in_background", &s.mute_in_background);
                    Hint("hint_no_sound_while_you_are", true);
                    Check("menu_sounds", &s.menu_sounds);
                    Hint("hint_the_sounds_of_changing_tabs", true);
                    Check("achievement_sound", &s.achievement_sound);
                    Hint("hint_the_sound_that_plays_as", true);
                }
                // the mixer's choices, where how the console sounds isn't settled
                if (Section("mix", true, false)) {
                    {
                        const app::Settings def{};
                        // a warning card, not a dimmed hint: it is the first thing to read in this section
                        WarningCard("hint_if_the_game_already_sounds");
                        ImGui::BeginDisabled(s.surround);
                        int backs = s.stereo_full_backs ? 1 : 0;
                        if (Segmented("back_speakers_in", &backs, {"3_db", "full_level"}))
                            s.stereo_full_backs = backs == 1;
                        Hint("hint_how_loud_the_back_speakers");
                        Check("subwoofer_in_stereo", &s.stereo_sub);
                        Hint("hint_folds_the_subwoofer_channel_into", true);
                        ImGui::EndDisabled();
                        Slider("reverb", &s.reverb, 0, 200, def.reverb);
                        Hint("hint_the_room_echo_in_caves");
                        int resampler = int(std::min<uint32_t>(s.resampler, 2));
                        if (Segmented("resampling", &resampler, {"linear", "cubic", "sinc"}))
                            s.resampler = uint32_t(resampler);
                        Hint("hint_how_sounds_recorded_at_lower");
                        Check("crosstalk_cancellation", &s.crosstalk);
                        Hint("hint_widens_positioned_sounds_over_two", true);
                        Check("doppler_effect", &s.doppler);
                        Hint("hint_moving_sounds_rise_and_fall", true);
                        Check("distance_muffling", &s.distance_filters);
                        Hint("hint_far_and_blocked_sounds_lose", true);
                        Check("voice_headroom", &s.voice_headroom);
                        Hint("hint_lowers_some_sounds_to_leave", true);
                        Check("sound_envelopes", &s.envelopes);
                        Hint("hint_sounds_fade_in_swell_and", true);
                        Check("sound_wobble_lfo", &s.lfo);
                        Hint("hint_vibrato_and_tremolo_on_the", true);
                        Check("sound_filters", &s.voice_filters);
                        Hint("hint_muffled_or_resonant_sounds_keep", true);
                        Check("soft_clipping", &s.soft_clip);
                        Hint("hint_when_the_mix_gets_too", true);
                        // every Mix setting back to its default; greyed out while they already are
                        const bool same = s.stereo_full_backs == def.stereo_full_backs && s.stereo_sub == def.stereo_sub &&
                                          s.reverb == def.reverb && s.crosstalk == def.crosstalk &&
                                          s.doppler == def.doppler && s.distance_filters == def.distance_filters &&
                                          s.resampler == def.resampler && s.voice_headroom == def.voice_headroom &&
                                          s.soft_clip == def.soft_clip && s.envelopes == def.envelopes &&
                                          s.lfo == def.lfo && s.voice_filters == def.voice_filters;
                        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));
                        ImGui::BeginDisabled(same);
                        if (ImGui::Button(T("reset_mix_to"))) {
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
                }
                end_tab();
            }

            if (tab("controls")) {
                // whose controls: a card of its own above the sections, not a second row of tabs
                if (Section("player", false)) {
                    {
                        // Segmented takes names it can keep for the frame
                        char names[input::kPorts][64];
                        for (int p = 0; p < input::kPorts; p++) std::snprintf(names[p], sizeof names[p], T("player_d"), p + 1);
                        static_assert(input::kPorts == 4, "the segments below are one per port");
                        Segmented("player", &g_player_now, {names[0], names[1], names[2], names[3]});
                    }
                }
                const int p = g_player_now;
                ImGui::PushID(p);
                DrawDevices(p);
                DrawController(p);
                DrawMouse(p);
                DrawKeys(p);
                keys_shown = true;
                ImGui::PopID();
                end_tab();
            }

            if (tab("multiplayer")) {
                static std::string lan;
                static uint16_t in_use = 0;
                static int error = 0;
                if (lan.empty() || ImGui::IsWindowAppearing())
                    lan = xnet::LocalAddress(), in_use = xnet::Port(), error = xnet::PortError();
                if (Section("online_play")) {
                    // edited in a copy and set as the field is left or Enter pressed: system link resolves the address
                    // each time it changes (xnet.cpp JoinAddress), and a half typed name is a DNS lookup
                    static char host[256];
                    if (ImGui::IsWindowAppearing()) std::snprintf(host, sizeof host, "%s", s.join_ip.c_str());
                    static char lan_line[160];
                    std::snprintf(lan_line, sizeof lan_line, T("hint_your_lan_address_is"), lan.c_str());
                    InfoCard({"hint_leave_empty_to_host_or",
                              "hint_for_lan_hosts_outside_your",
                              "hint_for_hosts_over_the_internet",
                              "hint_for_hosts_on_a_port",
                              lan_line}, true);
                    ImGui::InputTextWithHint(Beside("host_address"), T("leave_empty_to"), host, sizeof host);
                    if (ImGui::IsItemDeactivatedAfterEdit()) s.join_ip = host;
                    if (!app::JoinArg().empty()) ImGui::Text(T("this_copy_joins"), app::JoinArg().c_str());
                    InfoCard({"hint_udp_3074_as_on_the",
                              "hint_hosting_for_players_past_the",
                              "hint_each_extra_copy_on_this",
                              "hint_a_change_starts_from_the"}, true);
                    int port = int(s.system_link_port);
                    ImGui::BeginDisabled(app::PortArg() != 0);
                    if (ImGui::InputInt(Beside("online_play_port"), &port, 0, 0)) s.system_link_port = uint32_t(std::clamp(port, 1, 65535));
                    ImGui::EndDisabled();
                    if (app::PortArg()) ImGui::Text(T("this_copy_uses"), app::PortArg());
                    if (in_use && error) {
                        static char port_line[200];
                        std::snprintf(port_line, sizeof port_line, T("udp_u_couldn"), in_use,
                                      error == 10048 ? T("another_program_or") : T("blocked"));
                        WarningCard(port_line);
                    }
                    static const uint32_t start_port = s.system_link_port;
                    if (s.system_link_port != start_port) WarningCard("hint_restart_the_game_for_this");
                }

                if (Section("ui")) {
                    Check("unstretched_hud", &display::g_hud_split);
                    Hint("hint_on_a_widescreen_display_the", true);
                    int tags = int(std::min<uint32_t>(display::g_player_tags, display::kTagOff));
                    if (Segmented("player_names", &tags, {"names", "markers", "off"})) display::g_player_tags = uint32_t(tags);
                    Hint("hint_over_other_players_in_a", true);
                    Check("player_ranks", &display::g_player_ranks);
                    Hint("hint_the_rank_emblem_beside_other", true);
                    int bars = int(std::min<uint32_t>(display::g_health_bars, display::kBarsOff));
                    if (Segmented("health_bars", &bars, {"beside", "instead_of_name_marker", "off"}))
                        display::g_health_bars = uint32_t(bars);
                    Hint("hint_other_players_health_bars", true);
                    Check("player_outlines", &display::g_player_outlines);
                    Hint("hint_in_a_match_other_players", true);
                    Check("medgun_beam", &medbeam::g_enabled);
                    Hint("hint_a_beam_joins_a_medgun", true);
                    int team = int(std::min<uint32_t>(display::g_team_colors, display::kTeamBlueOrange));
                    if (Segmented("team_colors", &team, {"green_red", "blue_orange"}))
                        display::g_team_colors = uint32_t(team);
                    Hint("hint_the_colors_a_match_tells", true);
                    Check("readable_menu_hints", &menus::g_bright_hints);
                    Hint("hint_the_hints_above_and_below", true);
                }
                end_tab();
            }

            if (tab("mods")) {
                // the folders are looked at again each time the tab opens, and on Rescan
                if (ImGui::IsWindowAppearing()) app::ScanMods();
                if (Section("mods")) {
                    InfoCard({"hint_ticked_mods_are_in_use",
                              "hint_a_change_starts_from_the"}, true);
                    std::vector<app::ModEntry>& mods = s.mods;
                    if (mods.empty()) ImGui::TextDisabled("%s", T("hint_no_mods_installed"));
                    int move = -1, to = -1;
                    const float arrow = ImGui::GetFrameHeight();
                    for (int i = 0; i < int(mods.size()); i++) {
                        ImGui::PushID(mods[i].name.c_str());  // by name: the cursor goes with a mod as it moves
                        SettingGap();
                        const float x0 = ImGui::GetCursorScreenPos().x;
                        ImGui::BeginDisabled(i == 0);
                        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move = i, to = i - 1;
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::BeginDisabled(i + 1 == int(mods.size()));
                        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move = i, to = i + 1;
                        ImGui::EndDisabled();
                        ImGui::SameLine(0.0f, arrow * 0.5f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(mods[i].name.c_str());
                        // the box at the value column of the other settings, or past a name that runs beyond the labels
                        const float end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x * 3.0f;
                        const float column = x0 + LabelWidth() + ImGui::GetStyle().ItemSpacing.x;
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::SetCursorScreenPos(ImVec2(std::max(column, end), ImGui::GetCursorScreenPos().y));
                        ImGui::Checkbox("##on", &mods[i].enabled);
                        ImGui::PopID();
                    }
                    if (move >= 0) std::swap(mods[move], mods[to]);
                    if (app::ModsChanged())
                        WarningCard("hint_restart_the_game_to_use");
                }
                if (Section("installing")) {
                    static std::string dir;
                    if (dir.empty()) dir = app::ModsDir().string();
                    InfoCard({"hint_put_each_mod_in_its",
                              "hint_the_game_s_own_files"}, true);
                    {
                        // set in from the card's sides and down from its header
                        const float side = std::round(ImGui::GetStyle().ItemSpacing.x * 1.5f);
                        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
                        ImGui::Indent(side);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - side);
                        ImGui::TextWrapped("%s", dir.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent(side);
                    }
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
                    if (ImGui::Button(T("rescan"))) app::ScanMods();
                    ImGui::SameLine();
                    if (ImGui::Button(T("open_folder"))) app::OpenFolder(app::ModsDir());
                }
                end_tab();
            }

            if (tab("achievements")) {
                DrawAchievements();
                end_tab();
            }

            if (diag::kDebugFeatures && tab("debug")) {
                if (Section("menu")) {
                    Check("resizable_menu", &g_menu_resizable);
                    Hint("hint_drag_the_menu_s_edges",
                         true);
                }
                if (Section("fast_forward")) {
                    Check("fast_forward_enabled", &display::g_fast_forward_enabled);
                    ImGui::BeginDisabled(!display::FastForwardEnabled());
                    uint32_t speed = display::g_fast_forward_speed.load(std::memory_order_relaxed);
                    Slider("fast_forward_speed", &speed, display::kFastForwardMin, display::kFastForwardMax, 4);
                    display::g_fast_forward_speed.store(speed, std::memory_order_relaxed);
                    ImGui::EndDisabled();
                    Hint("hint_fast_forward");
                }
                if (Section("color_vision")) {
                    int cvd = int(std::min<uint32_t>(gpu::g_cvd_sim.load(std::memory_order_relaxed), 4));
                    ImGui::BeginDisabled(D3d11());
                    if (Segmented("simulate", &cvd, {"off", "protan", "deutan", "tritan", "Mono"}))
                        gpu::g_cvd_sim.store(uint32_t(cvd), std::memory_order_relaxed);
                    ImGui::EndDisabled();
                    Hint(D3d11() ? NoVulkan() : "hint_shows_the_screen_as_seen", true);
                }
                if (Section("warp")) DrawWarp();
                end_tab();
            }
            if (tab("quit")) {
                if (Section("quit", false)) {
                    SettingGap();  // the room under the card's top, as before a settings row
                    ImGui::TextUnformatted(T("hint_quit_the_game_progress_since"));
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.75f));  // set apart from the button
                    PushDangerButton();
                    if (ImGui::Button(T("quit_game"))) app::Quit();  // the tab is the prompt: no second one
                    PopDangerButton();
                }
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
                    // every tab's label is drawn here alone (ImGui's is clear): drawn twice, ImGui's at its own spot and
                    // this centred one, a hovered tab's looked bold and a pixel wider
                    if (i == shown || (p.hot && !hush)) {
                        const bool danger = i == kQuitTab;  // Quit is red, not the accent
                        ImVec4 fill = danger ? kDanger : st.Colors[ImGuiCol_CheckMark];
                        fill.w = i == shown ? 0.80f : 0.0f;
                        if (i != shown) fill = danger ? ImVec4(kDanger.x, kDanger.y, kDanger.z, 0.16f) : ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
                        dl->AddRectFilled(p.lo, p.hi, ImGui::GetColorU32(fill), (p.hi.y - p.lo.y) * 0.5f);  // a pill
                    }
                    // every tab is outlined like the widgets, selected or not (the Close button's is its frame's own)
                    if (g_decorations)
                        // over the selected tab's bright fill the lines need more to show
                        DrawFrame(dl, p.lo, p.hi, (p.hi.y - p.lo.y) * 0.5f, st.Alpha, 0, 0.0f, i == shown ? 200.0f : 120.0f,
                                  i == shown ? 55.0f : 24.0f);
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
            const float pad_x = st.FramePadding.x * 1.4f;  // the tabs' roomier padding (tab, above)
            const float width = ImGui::CalcTextSize(T("close")).x + pad_x * 2.0f;
            ImGui::SetCursorPos(ImVec2(after.x + ImGui::GetContentRegionAvail().x - width, row_y));
            ImGui::PushStyleVarX(ImGuiStyleVar_FramePadding, pad_x);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);  // a pill like the tabs
            if (ImGui::Button(T("close"))) Close();
            ImGui::PopStyleVar(2);
            ImGui::PopItemFlag();
            ImGui::PopStyleColor(3);
            ImGui::SetCursorPos(after);
        }
        if (!keys_shown) g_rebind = -1;  // leaving the Controls tab cancels a rebind

        {
            // the footer's separator, the window's whole width, right where the page ends
            ImGuiWindow* w = ImGui::GetCurrentWindow();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
            const float y = std::round(ImGui::GetCursorScreenPos().y);
            w->DrawList->PushClipRect(ImVec2(w->Pos.x, y - 1.0f), ImVec2(w->Pos.x + w->Size.x, y + 2.0f));
            w->DrawList->AddLine(ImVec2(w->Pos.x, y), ImVec2(w->Pos.x + w->Size.x, y), ImGui::GetColorU32(ImGuiCol_Separator));
            w->DrawList->PopClipRect();
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
        }
        ImGui::AlignTextToFramePadding();
        // the build's version (git describe, cmake/version.cmake) at the footer line's far right; on a narrow window the
        // hint is cut short of it
        const float version_x = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(XBE_VERSION).x;
        const ImVec2 line = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(line, ImVec2(ImGui::GetWindowPos().x + version_x - ImGui::GetStyle().ItemSpacing.x * 2.0f,
                                         line.y + ImGui::GetFrameHeight()), true);
        ImGui::TextDisabled("%s", T("hint_stick_or_d_pad_move"));
        ImGui::PopClipRect();
        ImGui::SameLine(version_x);
        ImGui::TextDisabled("%s", XBE_VERSION);
        ImGui::PopStyleVar(2);
        DrawWindowFrame(g_fade);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (g_decorations) {
        // the dropdown lists and the tooltips: framed like the window, on the foreground list (their own clip box would cut the shadow)
        for (ImGuiWindow* pw : ImGui::GetCurrentContext()->Windows)
            if (pw->Active && !pw->Hidden &&
                ((pw->Flags & ImGuiWindowFlags_Tooltip) ||
                 ((pw->Flags & ImGuiWindowFlags_Popup) && std::strncmp(pw->Name, "##Combo_", 8) == 0))) {
                const ImVec2 lo = pw->Pos, hi(pw->Pos.x + pw->Size.x, pw->Pos.y + pw->Size.y);
                const float round = ImGui::GetStyle().PopupRounding;
                if (pw->Flags & ImGuiWindowFlags_Tooltip) {
                    // the whole frame a pixel in from the edge, the black and light lines still touching
                    DrawFrame(ImGui::GetForegroundDrawList(), ImVec2(lo.x + 1.0f, lo.y + 1.0f), ImVec2(hi.x - 1.0f, hi.y - 1.0f),
                              std::max(round - 1.0f, 0.0f), g_fade);
                } else {
                    DrawFrame(ImGui::GetForegroundDrawList(), lo, hi, round, g_fade);
                }
            }
    }
    Cues(opened_before, settings_changed);
    g_tab_arrived = false;
    if (!keep_open) Close();  // the window's X
}

bool Welcome() {
    MenuStyle();
    {
        Buttons drive;
        for (int port = 0; port < input::kPorts; port++) drive.Add(ReadPad(port));
        FeedNav(drive);
    }
    app::Settings& s = app::Get();
    static int preset = presets::kModern;
    // in as the window opens, out after Start; done once out
    static double shown_at = -1.0, start_at = -1.0;
    const double now = ImGui::GetTime();
    const bool first = shown_at < 0.0;
    if (first) shown_at = now;
    float fade = std::min(float((now - shown_at) / 0.25), 1.0f);
    if (start_at >= 0.0) {
        if (now - start_at >= 0.2) {
            gpu::g_welcome_fade.store(0.0f);  // black from here until the game's first picture
            return true;
        }
        fade = std::min(fade, 1.0f - float((now - start_at) / 0.2));
    }
    fade = fade * fade * (3.0f - 2.0f * fade);
    g_fade = fade;
    gpu::g_welcome_fade.store(fade);  // the backdrop (vulkan.cpp kWelcomeHlsl) with the window

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImGuiStyle& base = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float margin = std::max(std::round(std::min(display.x, display.y) * 0.04f), font);
    const float width = std::min(std::max(display.x * 0.42f, font * 34.0f), display.x - margin * 2.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, display.y - margin * 2.0f));
    if (first) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(base.WindowPadding.x * 2.5f, base.WindowPadding.y * 2.5f));
    if (ImGui::Begin("##welcome", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        // the menu's roomier rows
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(base.FramePadding.x * 1.25f, base.FramePadding.y * 1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(base.ItemSpacing.x, base.ItemSpacing.y * 1.25f));
        auto gap = [&](float lines) { ImGui::Dummy(ImVec2(0.0f, std::round(font * lines))); };
        // a part's title: no bold face, so drawn twice a pixel apart (as SubHeading)
        auto heading = [&](const char* title) {
            const char* text = T(title);
            const ImVec2 at = ImGui::GetCursorScreenPos(), size = ImGui::CalcTextSize(text);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddText(at, col, text);
            dl->AddText(ImVec2(at.x + 1.0f, at.y), col, text);
            ImGui::Dummy(ImVec2(size.x + 1.0f, size.y));
        };

        ImGui::PushFont(nullptr, std::round(base.FontSizeBase * 1.7f));
        ImGui::TextUnformatted(T("welcome_to_live"));
        ImGui::PopFont();
        Hint("hint_pick_a_language_and_a");
        gap(1.0f);

        heading("language");
        {
            // the languages the game has text for, as on the General tab; the menu's and the game's text both
            static const char* const kKeys[] = {"english", "japanese", "german", "french", "spanish", "italian"};
            int lang = 0;
            for (int i = 0; i < int(std::size(kKeys)); i++)
                if (s.language == kKeys[i]) lang = i;
            if (Segmented("##language", &lang, {"English", "日本語", "Deutsch", "Français", "Español", "Italiano"}))
                s.language = s.ui_language = kKeys[lang];
            Hint("hint_text_only_voices_stay_in");
        }
        gap(0.8f);

        heading("graphics");
        Segmented("##preset", &preset,
                  {presets::Name(presets::kNostalgic), presets::Name(presets::kModern), presets::Name(presets::kEnhanced)});
        Hint(presets::Description(preset));
        gap(1.2f);

        // Start, the row's width, in the accent
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x, accent.y, accent.z, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);  // its lines are drawn below, as the segments' track has
        if (ImGui::Button(T("start"), ImVec2(-FLT_MIN, std::round(ImGui::GetFrameHeight() * 1.3f))) && start_at < 0.0) {
            presets::Apply(preset);
            start_at = now;
        }
        ImGui::PopStyleVar();
        if (g_decorations)
            DrawFrame(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                      ImGui::GetStyle().FrameRounding, fade, 0, 0.0f, 200.0f, 55.0f);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        DrawWindowFrame(fade);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return false;
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (app::Welcoming()) return false;  // before boot: no menu to open (the welcome screen is ImGui's alone)
    if (chat::Message(msg, wp, lp)) {  // system link chat: Y opens its line, which takes the typing until it closes
        if (result) *result = 0;
        return true;
    }
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
                } else if (g_open.load() && g_calib_step.load() >= 0) {
                    g_esc_swallow.store(true);
                    g_calib_step.store(-1);
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
        case kTitle: {
            wchar_t title[256];
            const int instance = app::Instance();
            if (instance) {
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
bool g_menu_resizable = false;
bool g_decorations = true;

void Draw(double) {}

bool Welcome() { return true; }

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
