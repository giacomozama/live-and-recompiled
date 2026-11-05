// The multiplayer menus without Xbox Live: the frontend's multiplayer menu drops its Xbox Live item and calls System
// Link Online, and the pause menu of a match drops Xbox Live and its Controller item becomes Options, which opens
// the settings menu. The mouse works the game's menu lists.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

#include "common/gamepad.h"
#include "common/keyboard.h"
#include "common/menus.h"
#include "common/mouse.h"
#include "common/prompts.h"
#include "common/ui.h"
#include "common/warp.h"

namespace {

using kernel::R32;
using kernel::W32;

// a UTF-16 string in guest memory from UTF-8 (the basic plane), written once
uint32_t GuestText(const char* s) {
    std::u16string text;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
        char16_t c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) c = char16_t((c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F)), p += 2;
        else if (c >= 0xC0 && p[0]) c = char16_t((c & 0x1F) << 6 | (p[0] & 0x3F)), p += 1;
        text += c;
    }
    const uint32_t va = mem::AllocSystem(uint32_t(text.size() + 1) * 2);
    for (size_t i = 0; i <= text.size(); i++) kernel::G<uint16_t>(va + uint32_t(i) * 2) = text.c_str()[i];
    return va;
}

// The language the game picked at boot (0x2A7840: XC_LANGUAGE 2 to 6, else English as 1), the folder of
// aid/text it reads; the labels this file gives the menus come in it, the game's own label kept for Japanese
enum : uint32_t { kEnglish = 1, kJapanese, kGerman, kFrench, kSpanish, kItalian };
uint32_t GameLanguage() { return R32(0x7FA1DC); }
// en, de, fr, es, it; null for Japanese
const char* InLanguage(const char* en, const char* de, const char* fr, const char* es, const char* it) {
    switch (GameLanguage()) {
        case kJapanese: return nullptr;
        case kGerman: return de;
        case kFrench: return fr;
        case kSpanish: return es;
        case kItalian: return it;
        default: return en;
    }
}

}  // namespace

// The frontend's multiplayer menu (0x236030, title "Welcome") runs the item list 0x2401F0 takes: 12 bytes an item, the
// slot of its label in the frontend's strings ([0x849800], a pointer each, resolved from the key names at 0x6BDAB0),
// its handler and a condition, up to an item of slot 0. 0x75E760 is Xbox Live (slot 0xC9), System Link (0xC8),
// Dumbots (0xC7) and Profiles (0x2FD); 0x75E718 the same with one more item. Xbox Live's item goes from both, and
// System Link's slot points at "Online", as does the slot of its own menu's title (0x118, the menu 0x23A2E3 runs,
// only reached from this one).
X86_EXTERN_FUNC(__imp__sub_00236030);
X86_FUNC(sub_00236030) {
    constexpr uint32_t kLists[] = {0x75E760, 0x75E718}, kStrings = 0x849800;
    constexpr uint32_t kXboxLive = 0xC9, kSystemLink = 0xC8, kSystemLinkTitle = 0x118;
    for (uint32_t list : kLists) {
        if (R32(list) != kXboxLive) continue;
        uint32_t i = 0;
        do {
            for (uint32_t k = 0; k < 12; k += 4) W32(list + i * 12 + k, R32(list + (i + 1) * 12 + k));
        } while (R32(list + i++ * 12));
    }
    static const uint32_t online = GuestText("Online");  // the same in every language
    if (const uint32_t strings = R32(kStrings)) {
        W32(strings + kSystemLink * 4, online);
        W32(strings + kSystemLinkTitle * 4, online);
    }
    __imp__sub_00236030(ctx, base);
}

// 0x1F9EB0: the frontend's attract timer, run from 0x1F9DA0 and at the end of 0x1FA7D0. With no pad held
// (0x1F9E60: no button and every stick axis within 20) it adds the frame's seconds ([0x7FA21C]) to [0x8756A8], and
// past 120 (0x648EA0) puts the frontend in state 0x10 ([0x776574]), which plays frontend\attract.xmv (0x1F9F80), and
// returns 1. menus::g_attract off: the timer stays at 0 and it returns 0, so the menu waits however long it is left.
bool menus::g_attract = true;
X86_EXTERN_FUNC(__imp__sub_001F9EB0);
X86_FUNC(sub_001F9EB0) {
    if (menus::g_attract) {
        __imp__sub_001F9EB0(ctx, base);
        return;
    }
    constexpr uint32_t kIdleSeconds = 0x8756A8;
    W32(kIdleSeconds, 0);
    kernel::Return(ctx, 0, 0);
}

// [[hook]] after the stores at 0x23605A and 0x23607F, where the menu puts its cursor back on the item a game left from
// (System Link 1, Xbox Live 0, Dumbots 2): one up, for the item gone
void ConkerMpMenuCursor(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    constexpr uint32_t kCursor = 0x8757FD;
    if (base[kCursor] > 0) base[kCursor]--;
}

// The Online menu (System Link's, 0x23A2B0) runs the list 0x75E578: Play (slot 0x11A) and Host (0x119), with
// another list right after it. A copy with Options (FT_Options, slot 0x38) after them takes its place: [[hook]] after
// the push of 0x75E578 at 0x23A2DE. Options has no handler of its own: ConkerOnlineOptions takes its A.
namespace {
constexpr uint32_t kOnlineItems = 0x75E578;
constexpr uint32_t kOnlineOptionsSlot = 0x38;
uint32_t g_online_items = 0;  // the copy, in guest memory
}  // namespace

void ConkerOnlineItems(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_online_items) {
        uint32_t n = 0;
        while (R32(kOnlineItems + n * 12)) n++;
        const uint32_t list = mem::AllocSystem((n + 2) * 12);
        for (uint32_t k = 0; k < n * 12; k += 4) W32(list + k, R32(kOnlineItems + k));
        const uint32_t options = list + n * 12;
        W32(options, kOnlineOptionsSlot);
        W32(options + 4, 0);
        W32(options + 8, 0);
        for (uint32_t k = 0; k < 12; k += 4) W32(options + 12 + k, 0);
        g_online_items = list;
    }
    if (R32(ctx.esp) == kOnlineItems) W32(ctx.esp, g_online_items);
}

// [[hook]] before 0x240517, where the item list 0x2401F0 runs calls the handler of the item A picked (eax the item):
// the Online menu's Options opens the settings menu on its Multiplayer tab instead (any pad drives it, out of a
// match), and the call is skipped (0x24051A)
bool ConkerOnlineOptions(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_online_items || R32(ctx.eax) != kOnlineOptionsSlot || ctx.eax < g_online_items ||
        ctx.eax >= g_online_items + 0x100)
        return false;
    ui::Show(ui::Tab::Multiplayer);
    return true;
}

// A match's pause menu (0x1F77C0) builds its items each frame, UTF-16 key names at 0x87C828: Continue, Quit, Xbox Live,
// Controller, then Change Class, Change Team and Restart when they apply. ebp is their count, [esp+0x28 + item] set
// skips an item (greyed), and the indices of the last three are [esp+0x1C], [esp+0x20] and ebx (-1 when absent);
// Xbox Live and Controller are the fixed 2 and 3. [[hook]] before 0x1F8CEC, the main page's input and draw: Xbox Live,
// which there is none of, shows FT_Options' label ("Options") in its place, never greyed.
void ConkerPauseItems(X86Context& ctx, uint8_t* base) {
    constexpr uint32_t kItems = 0x87C828, kXboxLiveKey = 0x5F74CC;
    if (int32_t(ctx.ebp) < 4 || R32(kItems + 8) != kXboxLiveKey) return;
    static const uint32_t options = GuestText("FT_Options");
    W32(kItems + 8, options);
    base[ctx.esp + 0x28 + 2] = 0;
}

// [[hook]] before 0x1F8DEB, A on an item other than Continue and Quit, eax the item: Options (Xbox Live's 2) opens the
// settings menu for the pausing player's pad ([0x776558], a player a port), with the select sound (0x1F8E97); the
// rest go on to the game's Controller test (0x1F8E1C), so Controller opens its own page
bool ConkerPauseOptions(X86Context& ctx, uint8_t* base) {
    constexpr uint32_t kPlayer = 0x776558;
    if (ctx.eax != 2) return false;
    const int player = base[kPlayer];
    ui::Show(ui::Tab::Last, player < input::kPorts ? player : -1);
    return true;
}

// The game's menu lists: 0x20C200 draws one of key names and 0x20C3B0 one of strings (a match's pause menu, the Xbox
// Live screens, prompts), and 0x2401F0 and 0x240570 run the frontend's item lists (the multiplayer menus, whose screen
// is not the pub's). Each frame one runs, the cursor is free (mouse::InMenu).
X86_EXTERN_FUNC(__imp__sub_0020C200);
X86_FUNC(sub_0020C200) {
    mouse::InMenu();
    __imp__sub_0020C200(ctx, base);
}

// A list shows up to [0x6BD4C8] rows (5 from 0x2079EA, 1 from 0x207B56: a match's menus outside the pause, the spawn
// choice among them), an item with a second line (+0x100) taking two, and past them it scrolls. A match's pause menu
// has up to six items, so in a match the lists show every row, up to kAllRows (longer ones, the class choice's seven,
// scroll): the cap is lifted while the list is set up (0x20C3B0, the count its second argument, its items one line
// each), which runs on into placing its cursor (0x216DF0, reached only by jumps), and while it draws (0x20C870).
namespace {

constexpr uint32_t kRowCap = 0x6BD4C8;
constexpr int kAllRows = 6;  // the most rows the list's texture holds (seven run past its edge: the class choice)

// the rows of the list set up: [0x7768D4] items at [0x849924], 0x398 bytes each
int ListRows() {
    const uint32_t items = R32(0x849924);
    const int count = int32_t(R32(0x7768D4));
    if (!items || count <= 0) return 0;
    int rows = count;
    for (int i = 0; i < count && rows <= kAllRows; i++) rows += mem::Base()[items + i * 0x398 + 0x100] != 0;
    return rows;
}

// the cap lifted to the rows for its life, in a match while they fit
struct AllRows {
    uint32_t was = 0, set = 0;
    explicit AllRows(int rows) {
        if (!warp::Multiplayer() || rows > kAllRows) return;
        was = R32(kRowCap);
        if (int32_t(was) >= rows) return;
        set = uint32_t(rows);
        W32(kRowCap, set);
    }
    ~AllRows() {
        if (set && R32(kRowCap) == set) W32(kRowCap, was);
    }
};

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0020C3B0);
X86_FUNC(sub_0020C3B0) {
    mouse::InMenu();
    AllRows rows(int32_t(R32(ctx.esp + 8)));
    __imp__sub_0020C3B0(ctx, base);
}

namespace {
void PanelBegin();
void PanelEnd();
uint32_t JumpBegin(X86Context& ctx);
void JumpEnd(uint32_t added);
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0020C870);
X86_FUNC(sub_0020C870) {
    mouse::InMenu();  // a list set up once and drawn on (the map choice) keeps the cursor free too
    AllRows rows(ListRows());
    PanelBegin();  // the mouse: the panel it puts the list on
    __imp__sub_0020C870(ctx, base);
    PanelEnd();
}

X86_EXTERN_FUNC(__imp__sub_002401F0);
X86_FUNC(sub_002401F0) {
    mouse::InMenu();
    const uint32_t added = JumpBegin(ctx);  // the mouse's item, a step away
    __imp__sub_002401F0(ctx, base);
    JumpEnd(added);
}

X86_EXTERN_FUNC(__imp__sub_00240570);
X86_FUNC(sub_00240570) {
    mouse::InMenu();
    const uint32_t added = JumpBegin(ctx);
    __imp__sub_00240570(ctx, base);
    JumpEnd(added);
}

// The mouse on the menu lists. 0x20C200 and 0x20C3B0 set the list up: its items at [0x849924], 0x398 bytes each
// (+0 the text, +0x103 set greyed), [0x7768D4] of them, and the item the menu has its cursor on at [0x849910]
// (0x216DF0, from the menu's own each frame). 0x20C870 draws it into a 256 by 256 texture through the camera at
// 0x776710 (-128..128 each way), then puts that texture on a bent panel in front of the game's own camera.
//
// The text: each item's by 0x3D3E60 at 0x20D4F9, whose glyphs 0x3D30C0 places; at 0x3D343A, before it culls one
// against the clip rect, xmm2 and xmm3 hold the glyph's left and right and xmm0 and xmm1 its top and bottom, in the
// texture's units around its middle, y down. ConkerListItem and ConkerListGlyph keep the union of each item's glyphs,
// and once the last is drawn the list is shown (g_shown).
//
// The panel: 0x2170E0 builds it a quad at a time (a quad list), each vertex 0x30 bytes from esp+0x3C as it was
// entered (the position, then at +0x10 the texture's coordinates) and sends them at 0x2175B2, through the vertex
// shader with the camera's ([0x84B868]) view at +0x10 and projection at +0x90, each transposed (0x20D768, 0x20D79D).
// ConkerPanelQuad keeps each quad the list's draw sends (g_panel_capture), and its end shows them with the list.
//
// menus::Pad, on the menu's player's polls: the cursor (the window letterboxes the back buffer to app::FrameAspect)
// goes through the quad under it back to the texture, where it hits an item or not. The cursor moved over an item
// turns the list's cursor to it by presses of up or down, one at a time while the list's cursor answers (a frontend
// list's goes there in one frame, JumpBegin), a left click on one does the same and then presses A, and each notch of the wheel presses up or down (menus::Wheel). With
// no list, in the pub, each notch presses left (away from the player) or right, from picture to picture, and a click
// presses A on the one in front. XBE_TRACE_MENU_MOUSE logs the list and its panel as they change, and
// each click.
namespace {

constexpr uint32_t kListItems = 0x849924, kListCount = 0x7768D4, kListCursor = 0x849910;
constexpr uint32_t kItemSize = 0x398, kItemGreyed = 0x103;
constexpr int kMaxItems = 32, kMaxQuads = 64;
constexpr float kTexture = 256.0f;  // the list's texture, a side
constexpr int64_t kShownUs = 150000;  // a list or prompt not drawn for this long is gone
constexpr float kSlack = 4.0f;  // units around an item's text (the texture's) or a prompt (pixels) that still hit it

struct Item {
    float x0, y0, x1, y1;  // the texture's units around its middle; x0 > x1 when no glyph was drawn
    bool greyed;
};
struct Vertex {
    float x, y, z;  // the panel's, before the camera
    float u, v;     // into the list's texture
};
struct Quad {
    Vertex v[4];
};
struct List {
    Item items[kMaxItems];
    int count = 0;
    int cursor = -1;
    int64_t shown_us = 0;
    Quad quads[kMaxQuads];
    int quad_count = 0;
    float camera[16] = {};  // world to clip, row vectors
    float view[16] = {}, projection[16] = {};  // what it is made of, for the trace
};

std::mutex g_list_lock;
List g_building, g_shown;  // under g_list_lock
int g_item = -1;  // the item whose text is drawing, game thread
bool g_panel_capture = false;  // the list's draw runs, game thread

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

float F32(uint32_t va) {
    const uint32_t v = R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool TraceMouse() {
    static const bool on = std::getenv("XBE_TRACE_MENU_MOUSE") != nullptr;
    return on;
}

// the back buffer's size: the device's ([0x5499E8] + 0x1A14), its Size at +0x10
bool BackBuffer(float& w, float& h) {
    constexpr uint32_t kDevice = 0x5499E8, kDevBackBuffer = 0x1A14;
    const uint32_t dev = R32(kDevice);
    const uint32_t surface = dev ? R32(dev + kDevBackBuffer) : 0;
    if (!surface) return false;
    const uint32_t size = R32(surface + 0x10);
    w = float((size & 0xFFF) + 1), h = float((size >> 12 & 0xFFF) + 1);
    return true;
}

// a panel vertex in the back buffer's pixels, false behind the camera
bool Screen(const float* m, const Vertex& p, float bw, float bh, float& sx, float& sy) {
    const float cx = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
    const float cy = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
    const float cw = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
    if (!(cw > 1e-6f)) return false;
    sx = (cx / cw + 1) * 0.5f * bw;
    sy = (1 - cy / cw) * 0.5f * bh;
    return true;
}

void LogList(const List& l, const char* why) {
    LOGR("menu mouse: %s, %d items, cursor %d, %d panel quads", why, l.count, l.cursor, l.quad_count);
    for (int i = 0; i < l.count; i++) {
        const Item& it = l.items[i];
        LOGR("menu mouse:   %d: %.1f,%.1f to %.1f,%.1f%s", i, it.x0, it.y0, it.x1, it.y1, it.greyed ? " greyed" : "");
    }
    float bw = 640, bh = 480;
    BackBuffer(bw, bh);
    for (int q = 0; q < l.quad_count; q += std::max(1, l.quad_count - 1)) {
        for (const Vertex& v : l.quads[q].v) {
            float sx = -1, sy = -1;
            const bool front = Screen(l.camera, v, bw, bh, sx, sy);
            LOGR("menu mouse:   quad %d: %.2f,%.2f,%.2f uv %.3f,%.3f on screen %.1f,%.1f%s", q, v.x, v.y, v.z, v.u, v.v,
                 sx, sy, front ? "" : " (behind)");
        }
    }
    auto log = [](const char* name, const float* m) {
        LOGR("menu mouse:   %s %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", name,
             m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    };
    log("view", l.view);
    log("projection", l.projection);
}

// the list's draw: its panel's quads from the start
void PanelBegin() {
    g_panel_capture = true;
    std::lock_guard lock(g_list_lock);
    g_building.quad_count = 0;
}

// and at its end, the panel goes with the list it drew
void PanelEnd() {
    g_panel_capture = false;
    std::lock_guard lock(g_list_lock);
    if (!g_building.quad_count) return;
    const bool changed = g_shown.quad_count != g_building.quad_count;
    g_shown.quad_count = g_building.quad_count;
    std::memcpy(g_shown.quads, g_building.quads, sizeof(Quad) * size_t(g_building.quad_count));
    std::memcpy(g_shown.camera, g_building.camera, sizeof g_shown.camera);
    std::memcpy(g_shown.view, g_building.view, sizeof g_shown.view);
    std::memcpy(g_shown.projection, g_building.projection, sizeof g_shown.projection);
    if (TraceMouse() && changed) LogList(g_shown, "panel");
}

}  // namespace

// [[hook]] before 0x20D4F9, an item's text (edi the item's offset): its glyphs are this item's
void ConkerListItem(X86Context& ctx, uint8_t* base) {
    const uint32_t items = R32(kListItems);
    const int index = int(ctx.edi / kItemSize);
    if (!items || index >= kMaxItems) {
        g_item = -1;
        return;
    }
    g_item = index;
    std::lock_guard lock(g_list_lock);
    if (index == 0) g_building.count = 0;
    g_building.items[index] = {1e9f, 1e9f, -1e9f, -1e9f, base[items + ctx.edi + kItemGreyed] != 0};
}

// [[hook]] before 0x20D4FE, after it: the list is shown once its last item is
void ConkerListItemDrawn(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (g_item < 0) return;
    const int count = int32_t(R32(kListCount));
    std::lock_guard lock(g_list_lock);
    g_building.count = g_item + 1;
    if (g_building.count >= std::min(count, kMaxItems)) {
        g_building.cursor = int32_t(R32(kListCursor));
        g_building.shown_us = NowUs();
        const bool changed = g_building.count != g_shown.count || g_building.cursor != g_shown.cursor;
        g_shown.count = g_building.count;
        std::memcpy(g_shown.items, g_building.items, sizeof(Item) * size_t(g_building.count));
        g_shown.cursor = g_building.cursor;
        g_shown.shown_us = g_building.shown_us;
        if (TraceMouse() && changed) LogList(g_shown, "shown");
    }
    g_item = -1;
}

// [[hook]] before 0x3D343A, a glyph placed: into the item drawing
void ConkerListGlyph(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (g_item < 0) return;
    const float l = ctx.xmm[2].f[0], r = ctx.xmm[3].f[0], t = ctx.xmm[0].f[0], b = ctx.xmm[1].f[0];
    std::lock_guard lock(g_list_lock);
    Item& it = g_building.items[g_item];
    it.x0 = std::min({it.x0, l, r});
    it.x1 = std::max({it.x1, l, r});
    it.y0 = std::min({it.y0, t, b});
    it.y1 = std::max({it.y1, t, b});
}

// [[hook]] before 0x2175B2, a panel quad sent (two pushes since its vertices were entered at esp+0x3C): one of the
// list's, while its draw runs
void ConkerPanelQuad(X86Context& ctx, uint8_t* base) {
    (void)base;
    constexpr uint32_t kCamera = 0x84B868, kCameraView = 0x10, kCameraProjection = 0x90;
    if (!g_panel_capture) return;
    std::lock_guard lock(g_list_lock);
    if (g_building.quad_count >= kMaxQuads) return;
    Quad& q = g_building.quads[g_building.quad_count++];
    for (int i = 0; i < 4; i++) {
        const uint32_t v = ctx.esp + 8 + 0x3C + uint32_t(i) * 0x30;
        q.v[i] = {F32(v), F32(v + 4), F32(v + 8), F32(v + 0x10), F32(v + 0x14)};
    }
    const uint32_t camera = R32(kCamera);
    if (!camera) return;
    float* v = g_building.view;
    float* p = g_building.projection;
    for (int i = 0; i < 16; i++) {
        v[i] = F32(camera + kCameraView + uint32_t(i) * 4);
        p[i] = F32(camera + kCameraProjection + uint32_t(i) * 4);
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += v[r * 4 + k] * p[k * 4 + c];
            g_building.camera[r * 4 + c] = sum;
        }
}

// A multiplayer menu's page hints ("R Configure" over its list, the page under it below) are bent panels 0x217970
// draws in 32 strips, each by its own vertex alpha: the byte of its 17th argument plus rand() % 11 (0x217C3C), a
// flicker that waves across the strips. Its call at 0x216D4D passes 0x10 (pushed at 0x216CDA), which leaves the text
// at about a fifth over the menu behind; the menu's title, the other call (0x20E526), passes 0x40 and reads.
// [[hook]] before 0x216D4D: the hints' alpha the title's, with menus::g_bright_hints
bool menus::g_bright_hints = true;
void ConkerPageHintAlpha(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!menus::g_bright_hints) return;
    constexpr uint32_t kArgAlpha = 16 * 4, kTitleAlpha = 0x40;
    W32(ctx.esp + kArgAlpha, kTitleAlpha);
}

// The prompts of single player's pause screen ("A Quit?", "B Resume", "Y Options") are the pause UI's: each button's
// icon an image the sprite routines (0x356000..0x35A000) draw as a quad already in the back buffer's pixels, by
// DrawVerticesUP from one of kSpriteDraws (the vertices on the stack: count, then data and stride). The icons are the
// UI's own copies of a_butt, b_butt, x_butt and y_butt: 32 by 32 DXT textures, told apart by their colour (Button).
// ConkerSpriteDraw keeps the box of each quad drawn with one of them on stage 0, the shadow's drawn just before it in
// with it; a prompt reaches from its icon to the next icon right of it, its label between. menus::Paused, from the
// pause screen's input each frame, says the screen is up.
namespace {

struct Prompt {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int64_t seen_us = 0;
    // a word's look this frame, to tell the highlighted one: its tallest glyph, and the colour of its glyphs past the
    // shadow (the brightest, ARGB)
    float glyph = 0;
    uint32_t colour = 0;
};
std::mutex g_prompt_lock;
Prompt g_prompts[4];  // by face button: A B X Y, under g_prompt_lock
std::atomic<int64_t> g_pause_us{0};
constexpr int64_t kSameFrameUs = 8000;  // the shadow and the icon over it, drawn moments apart

// the face button whose prompt is under the point, -1 none
int PromptAt(float x, float y, int64_t now) {
    std::lock_guard lock(g_prompt_lock);
    int best = -1;
    for (int b = 0; b < 4; b++) {
        const Prompt& pr = g_prompts[b];
        if (now - pr.seen_us > kShownUs) continue;
        // its label runs right to the next icon, or as far again as its icon is wide for the last
        float right = pr.x1 + (pr.x1 - pr.x0) * 2.5f;
        for (int o = 0; o < 4; o++)
            if (o != b && now - g_prompts[o].seen_us <= kShownUs && g_prompts[o].x0 > pr.x0)
                right = std::min(right, g_prompts[o].x0 - kSlack);
        const float pad = (pr.y1 - pr.y0) * 0.25f;
        if (x >= pr.x0 - kSlack && x <= right && y >= pr.y0 - pad && y <= pr.y1 + pad) best = b;
    }
    return best;
}

Prompt g_words[2];  // the quit prompt's Yes and No, under g_prompt_lock

// face: the button clicked, -1 none, -2 Yes, -3 No
void LogPrompts(float x, float y, int face, int64_t now) {
    LOGR("menu mouse: pause click at %.1f,%.1f on button %d", x, y, face);
    constexpr const char* kNames[] = {"A", "B", "X", "Y", "Yes", "No"};
    std::lock_guard lock(g_prompt_lock);
    for (int b = 0; b < 6; b++) {
        const Prompt& pr = b < 4 ? g_prompts[b] : g_words[b - 4];
        LOGR("menu mouse:   %s: %.1f,%.1f to %.1f,%.1f, %lld ms ago", kNames[b], pr.x0, pr.y0, pr.x1, pr.y1,
             (long long)((now - pr.seen_us) / 1000));
    }
}

// the face button a texture is the icon of (0 A, 1 B, 2 X, 3 Y), -1 for any other: a 32 by 32 DXT texture (format
// word +0xC: colour 15:8, log2 sizes from bit 20) whose coloured blocks average green, red, blue or yellow (a
// yellowish green for A, an orange yellow for Y). Each block's two end colours (RGB565 at +8 and +0xA of DXT3 and
// DXT5's 16 bytes) count, greys and blacks not.
int Button(uint32_t tex) {
    static std::unordered_map<uint64_t, int> known;  // by the texture and its texels, game thread
    const uint64_t key = uint64_t(tex) << 32 | R32(tex + 4);
    if (const auto it = known.find(key); it != known.end()) return it->second;
    int& b = known[key];
    b = -1;
    const uint32_t format = R32(tex + 0xC), colour = format >> 8 & 0xFF;
    if ((colour != 0x0E && colour != 0x0F) || (format >> 20 & 0xF) != 5 || (format >> 24 & 0xF) != 5) return b;
    const uint32_t texels = mem::VirtualFromPhysical(R32(tex + 4) & 0x0FFFFFFF);
    constexpr uint32_t kBytes = 8 * 8 * 16;
    if (!texels || !mem::IsCommitted(texels) || !mem::IsCommitted(texels + kBytes - 1)) return b;
    float r = 0, g = 0, bl = 0;
    for (uint32_t k = 0; k < 64; k++)
        for (uint32_t e = 0; e < 2; e++) {
            const uint16_t c = kernel::G<uint16_t>(texels + k * 16 + 8 + e * 2);
            const float cr = float(c >> 11) / 31, cg = float(c >> 5 & 63) / 63, cb = float(c & 31) / 31;
            if (std::max({cr, cg, cb}) - std::min({cr, cg, cb}) < 0.25f) continue;
            r += cr, g += cg, bl += cb;
        }
    if (r + g + bl <= 0) return b;
    // green over red is about 1.2 for A, 0.8 for Y and 0.4 for B
    if (bl > r && bl > g) b = 2;
    else if (g > r && g > bl) b = 0;
    else if (r > g * 1.8f && r > bl) b = 1;
    else if (r > bl && g > bl) b = 3;
    if (TraceMouse()) LOGR("menu mouse: texture %08X colour %.2f %.2f %.2f, button %d", tex, r, g, bl, b);
    return b;
}

}  // namespace

// [[hook]] before each of the sprite routines' DrawVerticesUP calls (kSpriteDraws in game.toml): a button's icon, while
// the pause screen is up
void ConkerSpriteDraw(X86Context& ctx, uint8_t* base) {
    (void)base;
    const int64_t now = NowUs();
    const int page = menus::KeyPage();
    if (page < 0) return;
    uint32_t tex;
    {
        std::lock_guard lock(d3d::Lock());
        tex = d3d::State().textures[0];
    }
    if (!tex) return;
    const int b = Button(tex);
    // the icon as a key, while player 1 plays with the keyboard (its shadow too, drawn first): the fixed key
    // keyboard::Pad takes for the button there (the pause screen's on its quit prompt too, whose prompts stay on
    // show), else the bound one
    if (b >= 0) {
        const int fixed = page == menus::kMenuList ? menus::PauseKeys(page, b)[0] : menus::PauseKey(b);
        const uint32_t key =
            menus::FaceFixed(page, b) && fixed ? prompts::KeySprite(0, fixed) : prompts::KeyIcon(0, b);
        if (key) d3d::TextureForNextDraw(0, key);
    }
    if (now - g_pause_us.load(std::memory_order_relaxed) > kShownUs) return;  // the mouse's part: the pause screen
    const uint32_t count = R32(ctx.esp + 4), data = R32(ctx.esp + 8), stride = R32(ctx.esp + 12);
    if (!count || count > 64 || stride < 8 || !data) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < count; i++) {
        const float x = F32(data + i * stride), y = F32(data + i * stride + 4);
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (TraceMouse()) {  // each texture the pause screen's sprites use, once
        static std::set<uint32_t> seen;
        if (seen.insert(tex).second)
            LOGR("menu mouse: pause sprite texture %08X (texels %08X) at %.1f,%.1f to %.1f,%.1f, button %d", tex,
                 R32(tex + 4), x0, y0, x1, y1, b);
    }
    if (b < 0) return;
    std::lock_guard lock(g_prompt_lock);
    Prompt& pr = g_prompts[b];
    if (now - pr.seen_us <= kSameFrameUs) {
        pr.x0 = std::min(pr.x0, x0), pr.y0 = std::min(pr.y0, y0);
        pr.x1 = std::max(pr.x1, x1), pr.y1 = std::max(pr.y1, y1);
    } else {
        pr = {x0, y0, x1, y1, now};
    }
    pr.seen_us = now;
}

// A match's weapon panel (0x1CC030, drawn by the HUD 0x1C7C60) shows the buttons as pictures. Its left piece
// (userInterface\invSelectL, drawn from 0x1CC116) has X's icon by the item inside the texture, the right one
// (invSelectR, from 0x1CC1CC) Y's by the weapon, and the box between them is the left trigger's icon on its own
// (userInterface\triggerL, G, I or S by the console's language, 0x1CC620 drawing it from 0x1CC744). Each is a quad of
// 0x1CAD90: four corners at [esp+4]..[esp+0x10] (each an x and a y: top left, top right, bottom left, bottom right),
// the texture at +0x14 and the colour at +0x18. It sets the texture (D3DDevice_SetTexture 0x539960, the stage in eax,
// unless [0x771764] says it is set already) and draws by 0x1CA7F0 (the corners pushed, al a shadow first, ebx the
// colour), which writes the vertices into the push buffer. While the player whose HUD it is last used the keyboard or
// mouse, the trigger's box is drawn as its key (the texture swapped for the draw, on a smaller square in the box's
// middle), and each piece of the panel has its key drawn over its icon after it, by the same two calls. The keys go at twice the panel's alpha (the panel is
// drawn at about half), so the icon does not show through them.
namespace {
constexpr uint32_t kSetTexture = 0x539960, kHudQuad = 0x1CA7F0;
constexpr uint32_t kTriggerQuad = 0x1CC749;  // 0x1CAD90's return in 0x1CC620, drawing the trigger's box
struct PanelIcon {
    uint32_t ret;  // 0x1CAD90's return, drawing the piece
    int button;  // keyboard::Button
    float u, v, size;  // the icon's centre in the piece's texture (0..1) and the square its key covers
};
// on the pieces' 128 by 128 texels: X's icon spans 95..111 by 48..63, Y's 61..75 by 24..39
constexpr PanelIcon kPanelIcons[] = {
    {0x1CC11B, keyboard::kButtonX, 103.5f / 128, 56.0f / 128, 20.5f / 128},
    {0x1CC1D1, keyboard::kButtonY, 68.5f / 128, 32.0f / 128, 20.5f / 128},
};
constexpr float kTriggerKey = 0.6f;  // the trigger's key, of its box's side
int g_hud_port = -1;             // whose HUD 0x1C7C60 draws, -1 outside it. Game thread
uint32_t g_trigger_texture = 0;  // the trigger's box's own texture, while its key's sprite stands in
uint32_t g_corners = 0;          // a panel key's four corners in guest memory
uint32_t g_trigger_corners = 0;  // the trigger key's

void SetTexture(X86Context& ctx, uint32_t tex) {
    ctx.eax = 0;
    kernel::CallGuest(ctx, kSetTexture, {tex});
}

uint32_t KeyAlpha(uint32_t colour) { return std::min(0xFFu, (colour >> 24) * 2) << 24 | (colour & 0xFFFFFF); }

// a key's square of side size (of the quad's) centred at u, v on the quad with these corners (top left, top right,
// bottom left), written as four corners to out (allocated the first time)
void KeySquare(uint8_t* base, uint32_t& out, uint32_t tl, uint32_t tr, uint32_t bl, float u, float v, float size) {
    if (!out) out = mem::AllocSystem(32);
    const float x = F32(tl), y = F32(tl + 4);
    const float ux = F32(tr) - x, uy = F32(tr + 4) - y, vx = F32(bl) - x, vy = F32(bl + 4) - y;
    const float h = size / 2;
    const float corners[4][2] = {{u - h, v - h}, {u + h, v - h}, {u - h, v + h}, {u + h, v + h}};
    for (int c = 0; c < 4; c++) {
        const float at[2] = {x + ux * corners[c][0] + vx * corners[c][1], y + uy * corners[c][0] + vy * corners[c][1]};
        std::memcpy(base + out + c * 8, at, 8);
    }
}
}  // namespace

void menus::HudPlayer(uint32_t hud) {
    g_hud_port = -1;
    constexpr uint32_t kInventory = 0x38, kOwner = 0x0C, kActionsIndex = 0x60;
    if (!hud || !mem::IsCommitted(hud + kInventory)) return;
    const uint32_t inventory = R32(hud + kInventory);
    if (!inventory || !mem::IsCommitted(inventory + kOwner)) return;
    const uint32_t player = R32(inventory + kOwner);
    if (!player || !mem::IsCommitted(player + kActionsIndex)) return;
    const uint16_t port = kernel::G<uint16_t>(player + kActionsIndex);
    if (port < input::kPorts) g_hud_port = port;
}

// [[hook]] at 0x1CB031, 0x1CAD90 about to draw its quad (its texture set): the trigger's box as its key
void ConkerHudQuad(X86Context& ctx, uint8_t* base) {
    if (g_hud_port < 0 || R32(ctx.esp + 8) != kTriggerQuad) return;
    const uint32_t key = prompts::ButtonIcon(g_hud_port, keyboard::kButtonLT);
    if (!key) return;
    g_trigger_texture = R32(ctx.esp + 0x1C);
    SetTexture(ctx, key);
    W32(ctx.esp + 0x20, KeyAlpha(R32(ctx.esp + 0x20)));  // the colour, read into ebx next
    // the corners, read next: the key's smaller square in their place
    KeySquare(base, g_trigger_corners, R32(ctx.esp + 0xC), R32(ctx.esp + 0x10), R32(ctx.esp + 0x14), 0.5f, 0.5f,
              kTriggerKey);
    for (uint32_t c = 0; c < 4; c++) W32(ctx.esp + 0xC + c * 4, g_trigger_corners + c * 8);
    static bool told = false;
    if (!std::exchange(told, true)) LOGR("prompts: the weapon panel's trigger shows its key");
}

// [[hook]] at 0x1CB052, 0x1CAD90's quad drawn (its corners still pushed, ebx the colour, esi the texture): the trigger's
// texture back, or a panel piece's key over its icon
void ConkerHudQuadDrawn(X86Context& ctx, uint8_t* base) {
    const uint32_t eax = ctx.eax, ebx = ctx.ebx;
    if (g_trigger_texture) {
        SetTexture(ctx, g_trigger_texture);
        g_trigger_texture = 0;
        ctx.eax = eax;
        return;
    }
    if (g_hud_port < 0) return;
    const uint32_t ret = R32(ctx.esp + 0x18);
    for (const PanelIcon& icon : kPanelIcons) {
        if (icon.ret != ret) continue;
        const uint32_t key = prompts::ButtonIcon(g_hud_port, icon.button);
        if (!key) return;
        KeySquare(base, g_corners, R32(ctx.esp), R32(ctx.esp + 4), R32(ctx.esp + 8), icon.u, icon.v, icon.size);
        SetTexture(ctx, key);
        ctx.eax = 0;  // no shadow
        ctx.ebx = KeyAlpha(ebx);
        kernel::CallGuest(ctx, kHudQuad, {g_corners, g_corners + 8, g_corners + 16, g_corners + 24});
        SetTexture(ctx, ctx.esi);
        ctx.eax = eax, ctx.ebx = ebx;
        static bool told = false;
        if (!std::exchange(told, true)) LOGR("prompts: the weapon panel's icons show their keys");
        return;
    }
}

// A match's HUD prompts A where a player can act ("exit" on a vehicle): 0x1C7C60 draws up to five through 0x1C9DF0,
// each fading by its own value. 0x1C9DF0 draws A's icon (texture 0x226, through 0x1CAAA0, which sets it by 0x761F0
// and draws by 0x1CA7F0) pulsing about (320, 100), its half side 26.5 + 4 cos(5t), and its label (0x1F25F0 from
// 0x1C9FBD) 1.4 times 24 right of the icon's middle. With its byte at [esp+0x10] set, for a split screen's view, the
// floats at ecx scale and offset its x, the ones at eax its y, and [esp+0x14] scales the icon. ActionPrompt hands it
// that always, so the icon and label go centred as a pair: the middle moved left by half of what the label adds past
// the icon, the label's width as the text list last drew it (HudTextDrawn; one frame late for a new label, which
// fades in from nothing). While the player last used the keyboard, the icon is drawn as A's key.
namespace {
constexpr float kPromptHalf = 26.5f, kPromptLabel = 24 * 1.4f;  // the icon's mean half side; the label's offset
bool g_action_prompt = false;  // 0x1C9DF0 draws. Game thread
uint32_t g_prompt_key = 0;     // A's key's sprite, drawn for the icon; 0 none
uint32_t g_prompt_pairs = 0;   // 0x1C9DF0's x scale and offset, then its y's (16 bytes of guest memory)
std::unordered_map<std::u16string, float> g_label_width;  // a label's width in the back buffer's pixels
std::set<uint32_t> g_labels;   // text entries the prompts made, not drawn yet
uint32_t g_label_drawing = 0;  // the HUD prompt's label the text list draws, 0 none (HudTextDrawing)
float g_label_x0 = 1e9f, g_label_x1 = -1e9f;  // its glyphs' sides so far

std::u16string Label(uint32_t va) {
    std::u16string s;
    for (uint32_t i = 0; va && i < 128; i++) {
        const uint16_t c = kernel::G<uint16_t>(va + i * 2);
        if (!c) break;
        s.push_back(char16_t(c));
    }
    return s;
}

void WF32(uint32_t va, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    W32(va, v);
}
}  // namespace

void menus::ActionPrompt(X86Context& ctx, uint8_t* base, void (*draw)(X86Context&, uint8_t*)) {
    const uint32_t esp = ctx.esp;
    float sx = 1, ox = 0, sy = 1, oy = 0, scale = 1;
    if (base[esp + 0x10]) {  // a split screen's view
        sx = F32(ctx.ecx), ox = F32(ctx.ecx + 4), sy = F32(ctx.eax), oy = F32(ctx.eax + 4), scale = F32(esp + 0x14);
    }
    float width = 0;
    if (const auto it = g_label_width.find(Label(R32(esp + 0xC))); it != g_label_width.end()) width = it->second;
    // from the icon's left side to the label's right, about the icon's middle
    const float shift = (kPromptLabel + width - kPromptHalf * scale) / 2;
    if (!g_prompt_pairs) g_prompt_pairs = mem::AllocSystem(16);
    WF32(g_prompt_pairs, sx), WF32(g_prompt_pairs + 4, ox - shift);
    WF32(g_prompt_pairs + 8, sy), WF32(g_prompt_pairs + 12, oy);
    ctx.ecx = g_prompt_pairs, ctx.eax = g_prompt_pairs + 8;
    W32(esp + 0x10, 1);
    WF32(esp + 0x14, scale);
    g_prompt_key = g_hud_port >= 0 ? prompts::ButtonIcon(g_hud_port, keyboard::kButtonA) : 0;
    g_action_prompt = true;
    draw(ctx, base);
    g_action_prompt = false;
    g_prompt_key = 0;
}

void menus::HudTextMade(uint32_t entry) {
    if (!g_action_prompt) return;
    if (g_labels.size() > 64) g_labels.clear();  // made and never drawn
    g_labels.insert(entry);
}

void menus::HudTextDrawing(uint32_t entry) {
    if (!g_labels.erase(entry)) return;
    g_label_drawing = entry;
    g_label_x0 = 1e9f, g_label_x1 = -1e9f;
}

void menus::HudTextDrawn() {
    const uint32_t entry = std::exchange(g_label_drawing, 0);
    if (!entry || g_label_x1 < g_label_x0) return;
    if (g_label_width.size() > 256) g_label_width.clear();
    g_label_width[Label(entry + 0x28)] = g_label_x1 - g_label_x0;
}

// [[hook]] at 0x1CAD60 in 0x1CAAA0, its texture set (esi) and its quad next: a HUD prompt's A icon as its key
void ConkerHudPromptQuad(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_action_prompt || !g_prompt_key) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    SetTexture(ctx, g_prompt_key);
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    static bool told = false;
    if (!std::exchange(told, true)) LOGR("prompts: the HUD's A prompt shows its key");
}

// [[hook]] at 0x1CAD81, the quad drawn: the icon's texture back, which 0x1CAAA0 keeps as set ([0x771764])
void ConkerHudPromptQuadDrawn(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_action_prompt || !g_prompt_key) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    SetTexture(ctx, ctx.esi);
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
}

// The quit prompt (page 2 of the pause screen) asks with the words Yes and No, which the pause UI highlights by the
// pad's down and up; on A, 0x203C87 takes the larger value of its layers 7 and 3, 0 for No (back to the page
// before) and else Yes (0x1F9AB0, quit). Every string is drawn by 0x3D3D00 (the text at [esp+4], from character ecx),
// each glyph a quad of four 28-byte vertices at 0x807230 sent at 0x3D3981; on the pause screen they are in the text's
// own units, which the vertex shader takes through c0..c3 and then the viewport's scale and offset (c-38, c-37).
// While Yes or No draws, ConkerTextQuad keeps its glyphs' box in the back buffer's pixels, and how tall and bright it is
// drawn. The cursor moved over one presses down (Yes) or up (No) until the prompt highlights it (Highlighted: the word
// drawn taller or brighter), and a click on one presses A, and ConkerQuitChoice, where the prompt tests the value,
// makes it the word clicked.
namespace {

int g_word = 0;  // the string drawing is Yes (1) or No (2), game thread
std::atomic<int> g_quit_choice{-1};  // the word clicked: 1 Yes, 0 No, -1 none
std::atomic<int64_t> g_quit_choice_us{0};
constexpr int64_t kChoiceUs = 500000;  // a click's choice waits this long for the prompt's A

// a string to match the prompt's words by: its spaces aside, ASCII letters in lower case, up to 16 characters
std::u16string WordKey(uint32_t va) {
    std::u16string w;
    for (uint32_t i = 0; i < 16; i++) {
        char16_t c = kernel::G<uint16_t>(va + i * 2);
        if (!c) break;
        if (c == u' ') continue;
        if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
        w += c;
    }
    return w;
}

// the prompt's Yes and No in the language the game runs in: the text the string lookup (0x3F9CB0) found for the
// keys YES and NO, the English words until it has
std::mutex g_yes_no_lock;
std::u16string g_yes_no[2] = {u"yes", u"no"};

void KeepYesNo(int word, uint32_t text) {
    std::lock_guard hold(g_yes_no_lock);
    g_yes_no[word] = WordKey(text);
}

// Yes 1, No 2, else 0: the string at va, its spaces aside, in any case
int Word(uint32_t va) {
    const std::u16string w = WordKey(va);
    if (w.empty()) return 0;
    std::lock_guard hold(g_yes_no_lock);
    return w == g_yes_no[0] ? 1 : w == g_yes_no[1] ? 2 : 0;
}

// the word under the point, 0 none
int WordAt(float x, float y, int64_t now) {
    std::lock_guard lock(g_prompt_lock);
    for (int w = 0; w < 2; w++) {
        const Prompt& pr = g_words[w];
        if (now - pr.seen_us <= kShownUs && x >= pr.x0 - kSlack && x <= pr.x1 + kSlack && y >= pr.y0 - kSlack &&
            y <= pr.y1 + kSlack)
            return w + 1;
    }
    return 0;
}

// the word the quit prompt highlights, 1 Yes or 2 No, -1 unknown: the one drawn taller, or else brighter
int Highlighted(int64_t now) {
    std::lock_guard lock(g_prompt_lock);
    const Prompt& yes = g_words[0];
    const Prompt& no = g_words[1];
    if (now - yes.seen_us > kShownUs || now - no.seen_us > kShownUs) return -1;
    if (yes.glyph > no.glyph * 1.04f) return 1;
    if (no.glyph > yes.glyph * 1.04f) return 2;
    auto light = [](uint32_t c) { return int(c >> 16 & 0xFF) + int(c >> 8 & 0xFF) + int(c & 0xFF); };
    const int ly = light(yes.colour), ln = light(no.colour);
    if (ly > ln + 24) return 1;
    if (ln > ly + 24) return 2;
    return -1;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_003D3D00);
X86_FUNC(sub_003D3D00) {
    const uint32_t text = R32(ctx.esp + 4);
    const bool pause = NowUs() - g_pause_us.load(std::memory_order_relaxed) <= kShownUs;
    g_word = pause && text ? Word(text + ctx.ecx * 2) : 0;
    __imp__sub_003D3D00(ctx, base);
    g_word = 0;
}

namespace {
// the glyph's quad being sent (0x3D3981) in the back buffer's pixels; false if it can't be placed
bool GlyphBox(float& x0, float& y0, float& x1, float& y1) {
    constexpr uint32_t kQuad = 0x807230, kVertex = 0x1C;
    constexpr size_t kC0 = 96, kScale = 96 - 38, kOffset = 96 - 37;  // c0, c-38, c-37 in hardware order
    x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    d3d::FlushPending();
    std::lock_guard lock(d3d::Lock());
    const gpu::State& st = d3d::State();
    const bool programmable = st.vs_handle & 1;
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t v = kQuad + i * kVertex;
        float x = F32(v), y = F32(v + 4);
        if (programmable) {
            const float z = F32(v + 8);
            float c[4];
            for (int r = 0; r < 4; r++) {
                const auto& m = st.vsc[kC0 + size_t(r)];
                c[r] = x * m[0] + y * m[1] + z * m[2] + m[3];
            }
            if (!(std::fabs(c[3]) > 1e-6f)) return false;
            x = c[0] / c[3] * st.vsc[kScale][0] + st.vsc[kOffset][0];
            y = c[1] / c[3] * st.vsc[kScale][1] + st.vsc[kOffset][1];
        }
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    return true;
}

}  // namespace

// [[hook]] before 0x3D3981, a glyph's quad sent: into Yes's or No's box, or the HUD prompt's label's
void ConkerTextQuad(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    if (!g_word && !g_label_drawing) return;
    constexpr uint32_t kQuad = 0x807230;
    float x0, y0, x1, y1;
    if (!GlyphBox(x0, y0, x1, y1)) return;
    if (g_label_drawing) {
        g_label_x0 = std::min(g_label_x0, x0), g_label_x1 = std::max(g_label_x1, x1);
        if (!g_word) return;
    }
    const uint32_t colour = R32(kQuad + 0x10);  // the first vertex's diffuse
    const int64_t now = NowUs();
    std::lock_guard lock(g_prompt_lock);
    Prompt& pr = g_words[g_word - 1];
    if (now - pr.seen_us <= kSameFrameUs) {
        pr.x0 = std::min(pr.x0, x0), pr.y0 = std::min(pr.y0, y0);
        pr.x1 = std::max(pr.x1, x1), pr.y1 = std::max(pr.y1, y1);
    } else {
        pr = {x0, y0, x1, y1, now};
    }
    pr.seen_us = now;
    pr.glyph = std::max(pr.glyph, y1 - y0);
    auto light = [](uint32_t c) { return (c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF); };
    if (light(colour) >= light(pr.colour)) pr.colour = colour;
}

// [[hook]] before 0x203CE9, the quit prompt's A: eax the value it goes by (0 No), the word clicked instead
void ConkerQuitChoice(X86Context& ctx, uint8_t* base) {
    (void)base;
    const int choice = g_quit_choice.exchange(-1);
    if (choice < 0 || NowUs() - g_quit_choice_us.load() > kChoiceUs) return;
    ctx.eax = uint32_t(choice);
}

namespace {

constexpr uint16_t kPadUp = 1 << 0, kPadDown = 1 << 1, kPadLeft = 1 << 2, kPadRight = 1 << 3;
constexpr int64_t kPressUs = 70000, kGapUs = 70000;  // a press held, then let go, long enough for a frame to see each
constexpr int64_t kRetryHoldUs = 35000, kRetryUs = 400000;  // a step to the mouse's item unanswered: let go, again

struct Pointer {
    float x = -1, y = -1;  // the cursor in the window when last seen
    bool button = false;  // the left button, as last seen
    int target = -1;  // the item the list's cursor goes to
    bool pick = false;  // and A once it is there
    uint16_t press = 0;  // the d-pad press held, or 0 with face
    int face = -1;  // the face button held (its analog: A B X Y), or -1
    int64_t press_us = 0;  // the press's start
    int from = -1;  // the list's cursor when it was pressed
    bool paced = false;  // a step to the mouse's item, held and let go by the list's draws (kPressUs a cap)
    bool released = false;  // and let go, since release_us
    int64_t release_us = 0;
    int64_t step_us = 0;  // the step's first press
    int word = 0;  // the quit prompt's word (1 Yes, 2 No) the highlight goes to, 0 none
    int steps = 0;  // presses toward it so far
    int jump = -1;  // the item a frontend list was sent to (g_jump), -1 none
    int64_t jump_us = 0;  // when
    float drag = 0;  // a frontend screen without a list: the mouse's motion up (-) or down not stepped yet, 480 high
    float drag_y = -1;  // and the cursor's y in the frame when last seen there, -1 none
};
Pointer g_pointer;  // game thread
std::atomic<int> g_wheel{0};  // the wheel's turn not stepped yet, 120 a notch
constexpr int kNotch = 120;

// The frontend's item lists (0x2401F0, 0x240570; the multiplayer menus) take the cursor to the mouse's item in the
// frame it points at it, not a press at a time: the list's cursor (a byte, its second argument) is put a step short of
// the item and the step's bit (1 up, 2 down) added to the menu's presses ([0x776238]) for the list's call, so the list
// takes that one step as its own, the move's sound with it. The prongs beside the list ease to the item on a spring
// (0x2B0760 on [0x7768F0] and its speed [0x7768EC], toward [0x7768F4], from 0x2080B0), vel += (gap * 6 - vel) * 14 * dt,
// about half a second to settle; after a move of the mouse's, until the pad steps a list again, they spring at
// kProngGap and kProngSpeed, settled in about a sixth of one.
constexpr uint32_t kMenuPresses = 0x776238, kStepUp = 1, kStepDown = 2;
constexpr int64_t kJumpUs = 150000;  // a jump not taken by then is dropped
std::atomic<int> g_jump{-1};  // the item the next frontend list goes to, from menus::Pad
std::atomic<int64_t> g_jump_us{0};  // when it was asked for
std::atomic<int64_t> g_frontend_us{0};  // a frontend list last ran
bool g_fast_prongs = false;  // game thread
constexpr uint32_t kProngs = 0x7768F0;
constexpr float kProngGap = 18.0f, kProngSpeed = 30.0f;

uint32_t JumpBegin(X86Context& ctx) {
    const int64_t now = NowUs();
    g_frontend_us.store(now);
    const int target = g_jump.exchange(-1);
    const uint32_t presses = R32(kMenuPresses);
    if (presses & (kStepUp | kStepDown)) {  // the pad steps it
        g_fast_prongs = false;
        return 0;
    }
    if (target < 0 || now - g_jump_us.load() > kJumpUs) return 0;
    const uint32_t items = R32(ctx.esp + 4), cursor = R32(ctx.esp + 8);
    if (!items || !cursor) return 0;
    int count = 0;  // 12 bytes an item, up to one of slot 0
    while (count < 64 && R32(items + uint32_t(count) * 12)) count++;
    const int at = int8_t(mem::Base()[cursor]);
    if (target >= count || at == target) return 0;
    const uint32_t step = target > at ? kStepDown : kStepUp;
    mem::Base()[cursor] = uint8_t(int8_t(step == kStepDown ? target - 1 : target + 1));
    W32(kMenuPresses, presses | step);
    g_fast_prongs = true;
    if (TraceMouse()) LOGR("menu mouse: frontend list from %d to %d of %d", at, target, count);
    return step;
}

void JumpEnd(uint32_t added) {
    if (added) W32(kMenuPresses, R32(kMenuPresses) & ~added);
}

}  // namespace

// the spring: xmm0 the frame's seconds, edx the value, ecx its speed, then on the stack the target, the gap's weight
// and the speed's rate (the caller's pushes, popped by it)
X86_EXTERN_FUNC(__imp__sub_002B0760);
X86_FUNC(sub_002B0760) {
    if (ctx.edx == kProngs && g_fast_prongs) {
        uint32_t gap, speed;
        std::memcpy(&gap, &kProngGap, 4);
        std::memcpy(&speed, &kProngSpeed, 4);
        W32(ctx.esp + 8, gap);
        W32(ctx.esp + 12, speed);
    }
    __imp__sub_002B0760(ctx, base);
}

namespace {

// the port whose pad works the menu: a paused match's player ([0x776558]), else a player's with a mouse, else 1's
int MenuPort() {
    constexpr uint32_t kMatchPaused = 0x862D90, kPausing = 0x776558;
    if (mem::Base()[kMatchPaused] && mem::Base()[kPausing] < input::kPorts) return mem::Base()[kPausing];
    for (int p = 0; p < input::kPorts; p++)
        if (input::Has(p, input::Kind::Mouse)) return p;
    return 0;
}

// the cursor in the back buffer's pixels, false when it is off the picture
bool CursorInFrame(float& x, float& y, float& wx, float& wy, float& bw, float& bh) {
    float ww, wh;
    if (!app::Cursor(wx, wy, ww, wh) || ww <= 0 || wh <= 0 || !BackBuffer(bw, bh)) return false;
    const float aspect = app::FrameAspect();
    const float vw = std::min(ww, wh * aspect), vh = vw / aspect;
    x = (wx - (ww - vw) / 2) / vw * bw;
    y = (wy - (wh - vh) / 2) / vh * bh;
    return x >= 0 && y >= 0 && x < bw && y < bh;
}

// the point in the panel's texture, in its units around its middle with y down the screen: through the quad under it,
// its texture coordinates weighed by where in the quad's triangles it falls. The coordinates are a fraction of the
// texture's side, or texels for one past 1.5; which way each runs on the screen comes from the first quad.
bool InTexture(const List& l, float px, float py, float bw, float bh, float& tx, float& ty, int& quad) {
    if (!l.quad_count) return false;
    float sx[4], sy[4];
    bool flip_u = false, flip_v = false, texels = false;
    {
        const Quad& q = l.quads[0];
        float mu = 0, mv = 0, mx = 0, my = 0;
        for (int i = 0; i < 4; i++) {
            if (!Screen(l.camera, q.v[i], bw, bh, sx[i], sy[i])) return false;
            mu += q.v[i].u / 4, mv += q.v[i].v / 4, mx += sx[i] / 4, my += sy[i] / 4;
            texels = texels || std::fabs(q.v[i].u) > 1.5f || std::fabs(q.v[i].v) > 1.5f;
        }
        float cu = 0, cv = 0;
        for (int i = 0; i < 4; i++) cu += (q.v[i].u - mu) * (sx[i] - mx), cv += (q.v[i].v - mv) * (sy[i] - my);
        flip_u = cu < 0, flip_v = cv < 0;
    }
    for (int k = 0; k < l.quad_count; k++) {
        const Quad& q = l.quads[k];
        bool front = true;
        for (int i = 0; i < 4 && front; i++) front = Screen(l.camera, q.v[i], bw, bh, sx[i], sy[i]);
        if (!front) continue;
        constexpr int kTriangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& t : kTriangles) {
            const int a = t[0], b = t[1], c = t[2];
            const float d = (sy[b] - sy[c]) * (sx[a] - sx[c]) + (sx[c] - sx[b]) * (sy[a] - sy[c]);
            if (std::fabs(d) < 1e-6f) continue;
            const float wa = ((sy[b] - sy[c]) * (px - sx[c]) + (sx[c] - sx[b]) * (py - sy[c])) / d;
            const float wb = ((sy[c] - sy[a]) * (px - sx[c]) + (sx[a] - sx[c]) * (py - sy[c])) / d;
            const float wc = 1 - wa - wb;
            if (wa < 0 || wb < 0 || wc < 0) continue;
            const float scale = texels ? 1 / kTexture : 1;
            float u = (wa * q.v[a].u + wb * q.v[b].u + wc * q.v[c].u) * scale;
            float v = (wa * q.v[a].v + wb * q.v[b].v + wc * q.v[c].v) * scale;
            if (flip_u) u = 1 - u;
            if (flip_v) v = 1 - v;
            tx = (u - 0.5f) * kTexture, ty = (v - 0.5f) * kTexture;
            quad = k;
            return true;
        }
    }
    return false;
}

int Hit(const List& l, float x, float y) {
    for (int i = 0; i < l.count; i++) {
        const Item& it = l.items[i];
        if (it.x0 <= it.x1 && x >= it.x0 - kSlack && x <= it.x1 + kSlack && y >= it.y0 - kSlack && y <= it.y1 + kSlack)
            return i;
    }
    return -1;
}

}  // namespace

void menus::Wheel(int turn) {
    if (!ui::Open()) g_wheel.fetch_add(turn);  // the settings menu keeps its own wheel turns
}

void menus::Paused() { g_pause_us.store(NowUs(), std::memory_order_relaxed); }

int menus::KeyPage() {
    using menus::kMenuList;
    // 0 the pause screen, 2 the quit prompt over it, which keeps its prompts on show (sub_00203B50 in gamepad.cpp)
    constexpr uint32_t kPage = 0x849AF8, kMatchPaused = 0x862D90;
    const int64_t now = NowUs();
    if (now - g_pause_us.load(std::memory_order_relaxed) <= kShownUs) {
        const uint32_t page = R32(kPage);
        return page == 0 || page == 2 ? int(page) : -1;
    }
    // a menu list (the frontend's multiplayer menus, A Select and B Back); anywhere in the frontend too, whose screens
    // without such a list (the multiplayer character choice) prompt A and B as well, the pub's pictures aside, where
    // Escape stays the console's Start and opens the settings menu (gamepad::Pad); and a match's pause menu and its
    // quit prompt (A Yes, B No) while player 1 ([0x776558], the port) paused it, the keys being port 0's
    constexpr uint32_t kPausing = 0x776558;
    if (mem::Base()[kMatchPaused]) return mem::Base()[kPausing] == 0 ? kMenuList : -1;
    if (warp::Frontend() && !gamepad::Pub()) return kMenuList;
    std::lock_guard lock(g_list_lock);
    return now - g_shown.shown_us <= kShownUs ? kMenuList : -1;
}

bool menus::PauseScreen() {
    const int page = KeyPage();
    return page == 0 || page == 2;
}

int menus::PauseKey(int face) {
    constexpr int kKeys[4] = {'Q', 0x1B, 0, 'E'};  // A, B (Escape), X, Y
    return face >= 0 && face < 4 ? kKeys[face] : 0;
}

std::array<int, 2> menus::PauseKeys(int page, int face) {
    if (face < 0 || face >= 4) return {};
    if (page == 2) {  // the quit prompt: Space and Enter answer with the word lit (Q and Escape: QuitBackKeys)
        constexpr std::array<int, 2> kQuit[4] = {{0x20, 0x0D}, {}, {}, {}};
        return kQuit[face];
    }
    if (page == kMenuList) {  // Space selects, Escape or the right mouse button goes back
        constexpr std::array<int, 2> kList[4] = {{0x20, 0}, {0x1B, 0x02}, {}, {}};  // 0x02 the right mouse button
        return kList[face];
    }
    return {PauseKey(face), 0};
}

bool menus::PauseKeysTakeStart(int page) { return page == 2; }

bool menus::FaceFixed(int page, int face) { return page == kMenuList ? face == 0 || face == 1 : page >= 0; }

std::array<int, 2> menus::QuitBackKeys() { return {'Q', 0x1B}; }

void menus::AnswerQuit(bool yes) {
    g_quit_choice_us.store(NowUs());
    g_quit_choice.store(yes ? 1 : 0);
}

void menus::Pad(int port, input::Pad& pad) {
    if (input::t_presence || port != MenuPort()) return;
    Pointer& p = g_pointer;
    const int64_t now = NowUs();
    List l;
    {
        std::lock_guard lock(g_list_lock);
        l = g_shown;
    }
    // no list: the pub's pictures, while the frontend shows them (its UI at [0x85E550])
    constexpr int kFrontend = 29;
    constexpr uint32_t kFrontendUi = 0x85E550;
    const bool list = now - l.shown_us <= kShownUs;
    const bool pub = !list && warp::CurrentLevel() == kFrontend && R32(kFrontendUi);
    const bool pause = !list && !pub && now - g_pause_us.load(std::memory_order_relaxed) <= kShownUs;
    // a frontend screen with none of those (the multiplayer character choice's army and class carousels): the wheel
    // steps it up or down as the d-pad does, and nothing else
    const bool wheel_only = !list && !pub && !pause && warp::Frontend();
    // the on-screen keyboard over a list takes the mouse itself (sub_002563E0)
    if (ui::Open() || app::Background() || Typing() || (!list && !pub && !pause && !wheel_only)) {
        p = {};
        p.button = app::KeyDown(1);  // the click that shut the settings menu is not one here
        g_wheel.store(0);
        return;
    }

    // a step to the mouse's item: held until the list is drawn with its cursor moved, then let go until it is drawn
    // again, which the game's poll saw, so a step takes two frames rather than kPressUs and kGapUs. A list that takes
    // no step for a while after one (the class choice, 130 to 200 ms, and only a fresh press then) is pressed again a
    // couple of frames at a time until it does, for up to kRetryUs
    if (p.press && p.paced) {
        if (!p.released) {
            const bool answered = l.shown_us > p.press_us && l.cursor != p.from;
            if (!answered && now - p.press_us < kRetryHoldUs) {
                pad.buttons |= p.press;
                return;
            }
            if (TraceMouse())
                LOGR("menu mouse: step %s from %d, %s after %lld ms (cursor %d, target %d)",
                     p.press == kPadDown ? "down" : "up", p.from, answered ? "answered" : "no answer",
                     (long long)((now - p.press_us) / 1000), l.cursor, p.target);
            p.released = true, p.release_us = now;
            return;
        }
        if (l.shown_us <= p.release_us && now - p.release_us < kGapUs) return;
        if (TraceMouse())
            LOGR("menu mouse: step let go for %lld ms, cursor %d", (long long)((now - p.release_us) / 1000), l.cursor);
        if (l.cursor == p.from && p.target >= 0 && p.target < l.count && p.target != l.cursor &&
            now - p.step_us < kRetryUs) {  // pressed again
            p.press_us = now, p.released = false;
            pad.buttons |= p.press;
            return;
        }
        if (l.cursor == p.from) p.target = -1, p.pick = false;  // the list did not answer the step
        p.press = 0, p.paced = false, p.released = false;
    }

    // a press runs its course: held, then let go
    if (p.press || p.face >= 0) {
        if (now - p.press_us < kPressUs) {
            if (p.face >= 0) pad.analog[p.face] = 0xFF;
            else pad.buttons |= p.press;
            return;
        }
        if (now - p.press_us < kPressUs + kGapUs) return;
        // the list's cursor did not answer the step: leave it where it is
        if (p.press && l.cursor == p.from) p.target = -1, p.pick = false;
        p.press = 0, p.face = -1;
    }

    // a notch of the wheel: a step, over where the cursor points
    if (const int w = g_wheel.load(); std::abs(w) >= kNotch) {
        g_wheel.fetch_sub(w > 0 ? kNotch : -kNotch);
        p.target = -1, p.pick = false;
        p.press = pub ? (w > 0 ? kPadLeft : kPadRight) : w > 0 ? kPadUp : kPadDown;
        p.from = -1;  // a step past the end is no reason to stop anything
        p.press_us = now;
        pad.buttons |= p.press;
        return;
    }
    // and the mouse moved up or down steps it a press for each kDragStep of the frame's height it goes, at most
    // kDragKept steps waiting
    if (!wheel_only) p.drag_y = -1, p.drag = 0;
    if (wheel_only) {
        constexpr float kDragStep = 40.0f, kDragKept = 2.0f;
        p.button = app::KeyDown(1);
        float x, y, wx, wy, bw = 640, bh = 480;
        if (!CursorInFrame(x, y, wx, wy, bw, bh) || bh <= 0) {
            p.drag_y = -1, p.drag = 0;
            return;
        }
        if (p.drag_y >= 0)
            p.drag = std::clamp(p.drag + (y - p.drag_y) * 480.0f / bh, -kDragStep * kDragKept, kDragStep * kDragKept);
        p.drag_y = y;
        if (std::abs(p.drag) < kDragStep) return;
        p.press = p.drag > 0 ? kPadDown : kPadUp;
        p.drag -= p.drag > 0 ? kDragStep : -kDragStep;
        p.target = -1, p.pick = false, p.from = -1;
        p.press_us = now;
        pad.buttons |= p.press;
        return;
    }

    // the pub: a click anywhere picks the picture in front (gamepad::Pad takes A on Options for the settings menu)
    if (pub) {
        float wx, wy, ww, wh;
        const bool button = app::KeyDown(1), clicked = button && !p.button && app::Cursor(wx, wy, ww, wh);
        p.button = button;
        if (!clicked) return;
        p.face = 0;
        p.press_us = now;
        pad.analog[0] = 0xFF;
        return;
    }

    // single player's pause screen: a click on a prompt presses its button; on the quit prompt the cursor moved over
    // Yes or No moves the highlight there, and a click on one answers with it
    if (pause) {
        float x = -1, y = -1, wx = -1, wy = -1, bw = 640, bh = 480;
        const bool button = app::KeyDown(1), clicked = button && !p.button;
        p.button = button;
        const bool in = CursorInFrame(x, y, wx, wy, bw, bh);
        const bool moved = wx != p.x || wy != p.y;
        p.x = wx, p.y = wy;
        if (!clicked) {
            if (const int word = in && moved ? WordAt(x, y, now) : 0; word && word != p.word) p.word = word, p.steps = 0;
            if (!p.word) return;
            // while the other is lit (or once on arriving when which is lit is not known), the press that lights it:
            // the prompt's highlight goes by down to Yes and up to No, though the two sit side by side
            const int lit = Highlighted(now);
            const uint16_t toward =
                (lit > 0 ? lit != p.word && p.steps < 2 : p.steps < 1) ? (p.word == 1 ? kPadDown : kPadUp) : 0;
            if (toward && TraceMouse()) {
                std::lock_guard lock(g_prompt_lock);
                LOGR("menu mouse: over %s, lit %d (Yes %.1f high %08X, No %.1f high %08X), presses %s",
                     p.word == 1 ? "Yes" : "No", lit, g_words[0].glyph, g_words[0].colour, g_words[1].glyph,
                     g_words[1].colour, toward == kPadDown ? "down" : "up");
            }
            if (!toward) {
                p.word = 0;
                return;
            }
            p.steps++;
            p.press = toward;
            p.from = -1;
            p.press_us = now;
            pad.buttons |= toward;
            return;
        }
        p.word = 0;
        // the quit prompt's words: the one clicked is the answer, and A gives it
        if (const int word = in ? WordAt(x, y, now) : 0) {
            if (TraceMouse()) LogPrompts(x, y, word == 1 ? -2 : -3, now);
            g_quit_choice_us.store(now);
            g_quit_choice.store(word == 1 ? 1 : 0);
            p.face = 0;
            p.press_us = now;
            pad.analog[0] = 0xFF;
            return;
        }
        const int face = in ? PromptAt(x, y, now) : -1;
        if (TraceMouse()) LogPrompts(x, y, face, now);
        if (face < 0) return;
        p.face = face;
        p.press_us = now;
        pad.analog[face] = 0xFF;
        return;
    }

    float x = -1, y = -1, wx = -1, wy = -1, bw = 640, bh = 480, tx = 0, ty = 0;
    int quad = -1;
    const bool over = CursorInFrame(x, y, wx, wy, bw, bh) && InTexture(l, x, y, bw, bh, tx, ty, quad);
    const int hit = over ? Hit(l, tx, ty) : -1;
    const bool moved = wx != p.x || wy != p.y;
    const float dy = wy - p.y;  // the window's y, down
    const bool button = app::KeyDown(1), clicked = button && !p.button;
    p.x = wx, p.y = wy, p.button = button;
    if (hit >= 0 && !l.items[hit].greyed) {
        if (clicked) p.target = hit, p.pick = true;
        else if (moved && !p.pick) p.target = hit;
    } else if (hit >= 0 && dy != 0 && !p.pick) {
        // a greyed item: the first one past it that is not, the way the mouse moves (down the list for down), where
        // the d-pad goes over it, so a scrolling list goes on scrolling under a greyed row at its edge
        const int dir = dy > 0 ? 1 : -1;
        for (int i = hit + dir; i >= 0 && i < l.count; i += dir)
            if (!l.items[i].greyed) {
                p.target = i;
                break;
            }
    }
    if (clicked && TraceMouse())
        LOGR("menu mouse: click at %.0f,%.0f in the window, %.1f,%.1f of %.0fx%.0f, quad %d, %.1f,%.1f in the texture, "
             "on item %d (cursor %d)",
             wx, wy, x, y, bw, bh, quad, tx, ty, hit, l.cursor);

    if (p.target < 0 || p.target >= l.count) {
        p.target = -1, p.pick = false;
        return;
    }
    // a frontend list: it goes to the item itself (JumpBegin), and the pick waits for it to show there
    if (now - g_frontend_us.load() <= kShownUs && p.target != l.cursor) {
        if (p.target == p.jump && now - p.jump_us < kJumpUs) return;  // on its way
        if (p.target == p.jump) {  // the list did not take it
            p.target = -1, p.pick = false, p.jump = -1;
            return;
        }
        p.jump = p.target, p.jump_us = now;
        g_jump_us.store(now);
        g_jump.store(p.target);
        return;
    }
    p.jump = -1;
    if (p.target != l.cursor) {
        p.press = p.target > l.cursor ? kPadDown : kPadUp;
        p.from = l.cursor;
        p.paced = true, p.released = false, p.step_us = now;
    } else if (p.pick) {
        p.face = 0;
        p.target = -1, p.pick = false;
    } else {
        p.target = -1;
        return;
    }
    p.press_us = now;
    if (p.face >= 0) pad.analog[p.face] = 0xFF;
    else pad.buttons |= p.press;
}

// The pub's pictures are labelled from the frontend's strings (UIFrontend), each with its _POSITION_X, _POSITION_Y
// and _SCALE beside it, all looked up by 0x3F9CB0(table, key, &text) as the frontend loads. A value table holds
// [table] its header (+4 the count), [table+4] its entries (6 bytes: the id, then the text's offset in characters)
// and [table+8] the text; [table+0xC] set keeps them elsewhere. The first picture's label is LIVE ("Xbox Live") over
// LIVE2 ("& Co"): the first time the frontend's table is seen, by a key ending _POSITION_X, their entries are pointed
// at "Multiplayer" and a space (an empty string reads as missing).
namespace {
// a UTF-16 string in guest memory, up to 64 characters, non-ASCII as '?'
std::string Narrow(uint32_t va) {
    std::string out;
    for (uint32_t i = 0; i < 64; i++) {
        const uint16_t c = kernel::G<uint16_t>(va + i * 2);
        if (!c) break;
        out += c < 0x80 ? char(c) : '?';
    }
    return out;
}

// XBE_TRACE_STRINGS set: each key looked up is logged once
void Trace(const std::string& key) {
    static const bool on = std::getenv("XBE_TRACE_STRINGS") != nullptr;
    if (!on) return;
    static std::mutex lock;  // the loader thread looks strings up too
    static std::set<std::string> seen;
    std::lock_guard hold(lock);
    if (seen.insert(key).second) LOGR("strings: key %s", key.c_str());
}

void Relabel(uint32_t table) {
    static uint32_t done = 0;  // the table last relabelled; the frontend loads its own again each time
    if (table == done || R32(table + 0xC)) return;
    done = table;
    static const char* multiplayer_text =
        InLanguage("Multiplayer", "Mehrspieler", "Multijoueur", "Multijugador", "Multigiocatore");
    static const uint32_t multiplayer = multiplayer_text ? GuestText(multiplayer_text) : 0, blank = GuestText(" ");
    const uint32_t count = R32(R32(table) + 4), entries = R32(table + 4), text = R32(table + 8);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry = entries + i * 6;
        const std::string was = Narrow(text + R32(entry + 2) * 2);
        const uint32_t now = was == "Xbox Live" ? multiplayer : was == "& Co" ? blank : 0;  // 0 for Japanese
        if (now) W32(entry + 2, (now - text) / 2);  // the offset wraps as the game adds it
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_003F9CB0);
X86_FUNC(sub_003F9CB0) {
    // the table, the key and where the text found goes ([out], the table's empty string when the key is missing)
    const uint32_t table = R32(ctx.esp + 4), key = R32(ctx.esp + 8), out = R32(ctx.esp + 0xC);
    const std::string name = key ? Narrow(key) : std::string();
    if (key) {
        Trace(name);
        constexpr std::string_view kPosition = "_POSITION_X";
        if (table && name.size() > kPosition.size() && name.ends_with(kPosition)) Relabel(table);
    }
    __imp__sub_003F9CB0(ctx, base);
    const int word = name == "YES" ? 0 : name == "NO" ? 1 : -1;
    if (word >= 0 && out && int32_t(ctx.eax) >= 0)  // -1: not found, or found empty
        if (const uint32_t text = R32(out)) KeepYesNo(word, text);
}

// The multiplayer menus' on-screen keyboard (a profile's name: Rename Profile, a new profile), run by 0x256C90 each
// frame it is up (from 0x21D810 and 0x226040), which inserts what its input 0x2563E0 returns at the text's cursor:
// a character (0 < code < 0xFFFC; dropped once the name is full), -1 Backspace, 0xFFFC and 0xFFFD the cursor left and
// right, 0xFFFE Create (0x256C90 returns 4, the name taken) and 0xFFFF Cancel (3, as B does). 0x2563E0 moves its
// highlight by the pad of [[0x8470FC]+8] and on A (0x100 in that pad's buttons at +8) returns its key's code; the
// highlight is a row [0x849D88] and a column [0x849D8C]: column 0 the buttons on the left, rows 0 to 5 (Create,
// Cancel, Shift, Caps Lock, Symbol, Accented), and columns 1 to 10 the keys, rows 0 to 4, of the page [0x849D98]
// (0 letters, 1 accented letters, 2 symbols): on row 3 columns 7 to 10 are Backspace, on row 4 1 to 6 Space, 7 and 8
// the left arrow, 9 and 10 the right one. Pages 1 and 2 take rows 0 to 3 from their tables (0x6BE87E accented,
// 0x6BE82E symbols, a UTF-16 character at (row * 10 + column) * 2, 0 no key); an accented letter is typed in lower
// case as it plus 0x20 (0x178 as 0xFF, 0xDF as it is). 0x254F80 draws it, its origin its third and fourth arguments
// (0, 0 from both): the keys 26 by 26 pixels of the 640 by 480 frame every 29, the first at 301, 176, the buttons on
// the left 151 to 273 wide.
//
// While it is up, the keyboard types into it (the window's text input on: menus::Message queues what is typed and
// 0x2563E0 returns one a frame, with the game's key sound 7 of 0x2431E0) and keyboard::Pad leaves the pad to the
// controllers; the mouse moved over a key moves the highlight there (the move's sound 6) and a click on one presses A
// on it for that frame.
namespace {

constexpr uint32_t kOskRow = 0x849D88, kOskColumn = 0x849D8C, kOskPage = 0x849D98;
constexpr uint32_t kOskPadPointer = 0x8470FC, kOskA = 0x100;
constexpr uint32_t kOskAccented = 0x6BE87E, kOskSymbols = 0x6BE82E;
constexpr uint32_t kOskBackspace = 0xFFFFFFFF, kOskLeft = 0xFFFC, kOskRight = 0xFFFD, kOskCreate = 0xFFFE,
                   kOskCancel = 0xFFFF;
constexpr uint32_t kOskSound = 0x2431E0, kOskKeySound = 7, kOskMoveSound = 6;
constexpr float kOskTop = 176 - 1.5f, kOskStep = 29, kOskKeys = 301 - 1.5f, kOskButtons0 = 149, kOskButtons1 = 276;
constexpr size_t kOskQueue = 64;

std::atomic<int64_t> g_typing_us{0};  // 0x2563E0 last ran
std::atomic<bool> g_text_on{false};   // the window's text input asked on
std::mutex g_typed_lock;
std::deque<uint32_t> g_typed;  // codes for 0x2563E0 to return, under g_typed_lock
float g_osk_x = 0, g_osk_y = 0;  // 0x254F80's origin, game thread
struct OskPointer {
    float x = -1, y = -1;  // the cursor in the window when last seen
    bool button = false;
};
OskPointer g_osk_pointer;  // game thread

// a character one of its pages has
bool Typeable(uint32_t c) {
    if (c == ' ' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (c < 0x21 || c > 0xFFFF) return false;
    for (int i = 0; i < 50; i++) {
        const uint32_t symbol = kernel::G<uint16_t>(kOskSymbols + uint32_t(i) * 2);
        const uint32_t accented = kernel::G<uint16_t>(kOskAccented + uint32_t(i) * 2);
        if (symbol && c == symbol) return true;
        if (!accented) continue;
        const uint32_t lower = accented == 0x178 ? 0xFF : accented == 0xDF ? accented : accented + 0x20;
        if (c == accented || c == lower) return true;
    }
    return false;
}

// the cell of the point (the frame's pixels), false off the keys; it may be one the page has no key on (OskKey)
bool OskCell(float x, float y, int& row, int& column) {
    x -= g_osk_x, y -= g_osk_y;
    if (y < kOskTop) return false;
    row = int((y - kOskTop) / kOskStep);
    if (x >= kOskButtons0 && x < kOskButtons1) {
        column = 0;
        return row <= 5;
    }
    if (x < kOskKeys || x >= kOskKeys + 10 * kOskStep) return false;
    column = 1 + int((x - kOskKeys) / kOskStep);
    return row <= 4;
}

// the page has a key there
bool OskKey(int row, int column) {
    if (column == 0) return row >= 0 && row <= 5;
    if (column < 1 || column > 10 || row < 0 || row > 4) return false;
    if (row == 4 || (row == 3 && column > 6)) return true;  // Space and the arrows, Backspace
    const uint32_t page = R32(kOskPage);
    if (page != 1 && page != 2) return true;
    const uint32_t table = page == 1 ? kOskAccented : kOskSymbols;
    return kernel::G<uint16_t>(table + uint32_t(row * 10 + column) * 2) != 0;
}

void OskSound(X86Context& ctx, uint32_t sound) {
    const uint32_t eax = ctx.eax;
    ctx.eax = sound;
    kernel::CallGuest(ctx, kOskSound, {});
    ctx.eax = eax;
}

// it went (Create or Cancel): the typing stops at once
void OskGone() {
    g_typing_us.store(0);
    {
        std::lock_guard lock(g_typed_lock);
        g_typed.clear();
    }
    if (g_text_on.exchange(false)) app::TextInput(false);
}

}  // namespace

bool menus::Typing() {
    const bool up = NowUs() - g_typing_us.load() <= kShownUs;
    if (!up && g_text_on.load() && g_text_on.exchange(false)) app::TextInput(false);
    return up;
}

void menus::Message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    if (NowUs() - g_typing_us.load() > kShownUs) return;
    const bool repeat = (lp & (1 << 30)) != 0;
    uint32_t code = 0;
    if (msg == app::kMsgChar) {
        if (Typeable(uint32_t(wp))) code = uint32_t(wp);
    } else if (msg == app::kMsgKeyDown) {
        switch (wp) {
            case 0x0D: code = repeat ? 0 : kOskCreate; break;   // Enter
            case 0x1B: code = repeat ? 0 : kOskCancel; break;   // Escape
            case 0x08: code = kOskBackspace; break;
            case 0x25: code = kOskLeft; break;
            case 0x27: code = kOskRight; break;
            default: break;
        }
    }
    if (!code) return;
    std::lock_guard lock(g_typed_lock);
    if (g_typed.size() < kOskQueue) g_typed.push_back(code);
}

// 0x254F80: the keyboard drawn, its origin the third and fourth arguments
X86_EXTERN_FUNC(__imp__sub_00254F80);
X86_FUNC(sub_00254F80) {
    g_osk_x = F32(ctx.esp + 0xC);
    g_osk_y = F32(ctx.esp + 0x10);
    __imp__sub_00254F80(ctx, base);
}

// 0x2563E0: the keyboard's input, each frame it is up
X86_EXTERN_FUNC(__imp__sub_002563E0);
X86_FUNC(sub_002563E0) {
    const int64_t now = NowUs();
    mouse::InMenu();
    if (now - g_typing_us.exchange(now) > kShownUs) {  // it came up
        {
            std::lock_guard lock(g_typed_lock);
            g_typed.clear();
        }
        // the click that opened it is not one on a key, nor does the cursor resting there move the highlight
        g_osk_pointer = {};
        float w, h;
        app::Cursor(g_osk_pointer.x, g_osk_pointer.y, w, h);
        g_osk_pointer.button = app::KeyDown(1);
        if (!g_text_on.exchange(true)) app::TextInput(true);
        if (TraceMouse()) LOGR("menu mouse: on-screen keyboard up");
    }

    // what was typed, one a frame
    uint32_t code = 0;
    {
        std::lock_guard lock(g_typed_lock);
        if (!g_typed.empty()) code = g_typed.front(), g_typed.pop_front();
    }
    if (code) {
        if (code != kOskCreate && code != kOskCancel) OskSound(ctx, kOskKeySound);
        else OskGone();
        kernel::Return(ctx, code, 0);
        return;
    }

    // the mouse: over a key the highlight goes there, a click presses A on it
    bool click = false;
    OskPointer& p = g_osk_pointer;
    if (!ui::Open() && !app::Background()) {
        float x = -1, y = -1, wx = -1, wy = -1, bw = 640, bh = 480;
        int row = -1, column = -1;
        const bool in = CursorInFrame(x, y, wx, wy, bw, bh) && OskCell(x, y, row, column) && OskKey(row, column);
        const bool button = app::KeyDown(1), clicked = button && !p.button;
        const bool moved = wx != p.x || wy != p.y;
        p.x = wx, p.y = wy, p.button = button;
        if (in && (moved || clicked) &&
            (int32_t(R32(kOskRow)) != row || int32_t(R32(kOskColumn)) != column)) {
            W32(kOskRow, uint32_t(row));
            W32(kOskColumn, uint32_t(column));
            if (!clicked) OskSound(ctx, kOskMoveSound);
        }
        click = in && clicked;
        if (clicked && TraceMouse())
            LOGR("menu mouse: keyboard click at %.1f,%.1f of %.0fx%.0f (origin %.1f,%.1f), row %d column %d%s", x, y,
                 bw, bh, g_osk_x, g_osk_y, row, column, in ? "" : ", no key");
    }
    uint32_t buttons = 0, was = 0;
    if (click) {
        const uint32_t input = R32(kOskPadPointer);
        const uint32_t pad = input ? R32(input + 8) : 0;
        if (pad) buttons = pad + 8, was = R32(buttons), W32(buttons, was | kOskA);
    }
    __imp__sub_002563E0(ctx, base);
    if (buttons) W32(buttons, was);
    if (ctx.eax == kOskCreate || ctx.eax == kOskCancel) OskGone();
}
