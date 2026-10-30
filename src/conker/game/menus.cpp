// The multiplayer menus without Xbox Live: the frontend's multiplayer menu drops its Xbox Live item and calls System
// Link Online Play, and the pause menu of a match drops Xbox Live and its Controller item becomes Options, which opens
// the settings menu. The mouse works the game's menu lists.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include "common/menus.h"
#include "common/mouse.h"
#include "common/ui.h"
#include "common/warp.h"

namespace {

using kernel::R32;
using kernel::W32;

// a UTF-16 string in guest memory, written once
uint32_t GuestText(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    const uint32_t va = mem::AllocSystem((n + 1) * 2);
    for (uint32_t i = 0; i <= n; i++) kernel::G<uint16_t>(va + i * 2) = uint8_t(s[i]);
    return va;
}

}  // namespace

// The frontend's multiplayer menu (0x236030, title "Welcome") runs the item list 0x2401F0 takes: 12 bytes an item, the
// slot of its label in the frontend's strings ([0x849800], a pointer each, resolved from the key names at 0x6BDAB0),
// its handler and a condition, up to an item of slot 0. 0x75E760 is Xbox Live (slot 0xC9), System Link (0xC8),
// Dumbots (0xC7) and Profiles (0x2FD); 0x75E718 the same with one more item. Xbox Live's item goes from both, and
// System Link's slot points at "Online Play", as does the slot of its own menu's title (0x118, the menu 0x23A2E3 runs,
// only reached from this one). The game runs in English: XC_LANGUAGE is 1.
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
    static const uint32_t online = GuestText("Online Play");
    if (const uint32_t strings = R32(kStrings)) {
        W32(strings + kSystemLink * 4, online);
        W32(strings + kSystemLinkTitle * 4, online);
    }
    __imp__sub_00236030(ctx, base);
}

// [[hook]] after the stores at 0x23605A and 0x23607F, where the menu puts its cursor back on the item a game left from
// (System Link 1, Xbox Live 0, Dumbots 2): one up, for the item gone
void ConkerMpMenuCursor(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    constexpr uint32_t kCursor = 0x8757FD;
    if (base[kCursor] > 0) base[kCursor]--;
}

// The Online Play menu (System Link's, 0x23A2B0) runs the list 0x75E578: Play (slot 0x11A) and Host (0x119), with
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
// the Online Play menu's Options opens the settings menu on its Multiplayer tab instead (any pad drives it, out of a
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
// Xbox Live and Controller are the fixed 2 and 3. [[hook]] before 0x1F8CEC, the main page's input and draw: Xbox Live
// goes, the rest move up, and Controller (now 2) shows FT_Options' label ("Options").
void ConkerPauseItems(X86Context& ctx, uint8_t* base) {
    constexpr uint32_t kItems = 0x87C828, kXboxLiveKey = 0x5F74CC, kControllerKey = 0x5F74B0;
    const uint32_t skip = ctx.esp + 0x28;
    const int count = int32_t(ctx.ebp);
    if (count < 4 || R32(kItems + 8) != kXboxLiveKey) return;
    for (int i = 2; i < count - 1; i++) {
        W32(kItems + i * 4, R32(kItems + (i + 1) * 4));
        base[skip + i] = base[skip + i + 1];
    }
    base[skip + count - 1] = 0;
    static const uint32_t options = GuestText("FT_Options");
    if (R32(kItems + 8) == kControllerKey) W32(kItems + 8, options);
    ctx.ebp = uint32_t(count - 1);
    auto up = [&](uint32_t& index) {
        if (int32_t(index) > 2 && int32_t(index) < count) index--;
    };
    up(kernel::G<uint32_t>(ctx.esp + 0x1C));
    up(kernel::G<uint32_t>(ctx.esp + 0x20));
    up(ctx.ebx);
}

// [[hook]] before 0x1F8DEB, A on an item other than Continue and Quit, eax the item: Options (2) opens the settings
// menu for the pausing player's pad ([0x776558], a player a port) and goes on as the Controller page did, with the
// select sound (0x1F8E97); the rest skip the fixed Xbox Live and Controller tests (0x1F8E37)
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

// A list shows up to [0x6BD4C8] rows (5, from 0x2079EA), an item with a second line (+0x100) taking two, and past
// them it scrolls. A match's pause menu has up to six items, so while a match is paused its lists show every row,
// up to kAllRows: the cap is lifted while the list is set up (0x20C3B0, the count its second argument, its items
// one line each), which runs on into placing its cursor (0x216DF0, reached only by jumps), and while it draws
// (0x20C870).
namespace {

constexpr uint32_t kRowCap = 0x6BD4C8;
constexpr int kAllRows = 8;

// the rows of the list set up: [0x7768D4] items at [0x849924], 0x398 bytes each
int ListRows() {
    const uint32_t items = R32(0x849924);
    const int count = int32_t(R32(0x7768D4));
    if (!items || count <= 0) return 0;
    int rows = count;
    for (int i = 0; i < count && rows <= kAllRows; i++) rows += mem::Base()[items + i * 0x398 + 0x100] != 0;
    return rows;
}

// the cap lifted to the rows for its life, while a match is paused and they fit
struct AllRows {
    uint32_t was = 0, set = 0;
    explicit AllRows(int rows) {
        constexpr uint32_t kMatchPaused = 0x862D90;
        if (!warp::Multiplayer() || !mem::Base()[kMatchPaused] || rows > kAllRows) return;
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
    __imp__sub_002401F0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00240570);
X86_FUNC(sub_00240570) {
    mouse::InMenu();
    __imp__sub_00240570(ctx, base);
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
// turns the list's cursor to it by presses of up or down, one at a time while the list's cursor answers, a left
// click on one does the same and then presses A, and each notch of the wheel presses up or down (menus::Wheel). XBE_TRACE_MENU_MOUSE logs the list and its panel as they change, and
// each click.
namespace {

constexpr uint32_t kListItems = 0x849924, kListCount = 0x7768D4, kListCursor = 0x849910;
constexpr uint32_t kItemSize = 0x398, kItemGreyed = 0x103;
constexpr int kMaxItems = 32, kMaxQuads = 64;
constexpr float kTexture = 256.0f;  // the list's texture, a side

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

namespace {

constexpr uint16_t kPadUp = 1 << 0, kPadDown = 1 << 1;
constexpr int64_t kShownUs = 150000;  // a list not drawn for this long is gone
constexpr int64_t kPressUs = 70000, kGapUs = 70000;  // a press held, then let go, long enough for a frame to see each
constexpr float kSlack = 4.0f;  // texture units around an item's text that still hit it

struct Pointer {
    float x = -1, y = -1;  // the cursor in the window when last seen
    bool button = false;  // the left button, as last seen
    int target = -1;  // the item the list's cursor goes to
    bool pick = false;  // and A once it is there
    uint16_t press = 0;  // the d-pad press held, or 0 with press_a
    bool press_a = false;
    int64_t press_us = 0;  // the press's start
    int from = -1;  // the list's cursor when it was pressed
};
Pointer g_pointer;  // game thread
std::atomic<int> g_wheel{0};  // the wheel's turn not stepped yet, 120 a notch
constexpr int kNotch = 120;

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

void menus::Wheel(int turn) { g_wheel.fetch_add(turn); }

void menus::Pad(int port, input::Pad& pad) {
    if (input::t_presence || port != MenuPort()) return;
    Pointer& p = g_pointer;
    const int64_t now = NowUs();
    List l;
    {
        std::lock_guard lock(g_list_lock);
        l = g_shown;
    }
    if (ui::Open() || app::Background() || now - l.shown_us > kShownUs) {
        p = {};
        g_wheel.store(0);
        return;
    }

    // a press runs its course: held, then let go
    if (p.press || p.press_a) {
        if (now - p.press_us < kPressUs) {
            if (p.press_a) pad.analog[0] = 0xFF;
            else pad.buttons |= p.press;
            return;
        }
        if (now - p.press_us < kPressUs + kGapUs) return;
        // the list's cursor did not answer the step: leave it where it is
        if (p.press && l.cursor == p.from) p.target = -1, p.pick = false;
        p.press = 0, p.press_a = false;
    }

    // a notch of the wheel: a step, over where the cursor points
    if (const int w = g_wheel.load(); std::abs(w) >= kNotch) {
        g_wheel.fetch_sub(w > 0 ? kNotch : -kNotch);
        p.target = -1, p.pick = false;
        p.press = w > 0 ? kPadUp : kPadDown;
        p.from = -1;  // a step past the end is no reason to stop anything
        p.press_us = now;
        pad.buttons |= p.press;
        return;
    }

    float x = -1, y = -1, wx = -1, wy = -1, bw = 640, bh = 480, tx = 0, ty = 0;
    int quad = -1;
    const bool over = CursorInFrame(x, y, wx, wy, bw, bh) && InTexture(l, x, y, bw, bh, tx, ty, quad);
    const int hit = over ? Hit(l, tx, ty) : -1;
    const bool moved = wx != p.x || wy != p.y;
    const bool button = app::KeyDown(1), clicked = button && !p.button;
    p.x = wx, p.y = wy, p.button = button;
    if (hit >= 0 && !l.items[hit].greyed) {
        if (clicked) p.target = hit, p.pick = true;
        else if (moved && !p.pick) p.target = hit;
    }
    if (clicked && TraceMouse())
        LOGR("menu mouse: click at %.0f,%.0f in the window, %.1f,%.1f of %.0fx%.0f, quad %d, %.1f,%.1f in the texture, "
             "on item %d (cursor %d)",
             wx, wy, x, y, bw, bh, quad, tx, ty, hit, l.cursor);

    if (p.target < 0 || p.target >= l.count) {
        p.target = -1, p.pick = false;
        return;
    }
    if (p.target != l.cursor) {
        p.press = p.target > l.cursor ? kPadDown : kPadUp;
        p.from = l.cursor;
    } else if (p.pick) {
        p.press_a = true;
        p.target = -1, p.pick = false;
    } else {
        p.target = -1;
        return;
    }
    p.press_us = now;
    if (p.press_a) pad.analog[0] = 0xFF;
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
    static const uint32_t multiplayer = GuestText("Multiplayer"), blank = GuestText(" ");
    const uint32_t count = R32(R32(table) + 4), entries = R32(table + 4), text = R32(table + 8);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry = entries + i * 6;
        const std::string was = Narrow(text + R32(entry + 2) * 2);
        const uint32_t now = was == "Xbox Live" ? multiplayer : was == "& Co" ? blank : 0;
        if (now) W32(entry + 2, (now - text) / 2);  // the offset wraps as the game adds it
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_003F9CB0);
X86_FUNC(sub_003F9CB0) {
    const uint32_t table = R32(ctx.esp + 4), key = R32(ctx.esp + 8);
    if (key) {
        const std::string name = Narrow(key);
        Trace(name);
        constexpr std::string_view kPosition = "_POSITION_X";
        if (table && name.size() > kPosition.size() && name.ends_with(kPosition)) Relabel(table);
    }
    __imp__sub_003F9CB0(ctx, base);
}
