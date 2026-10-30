// The multiplayer menus without Xbox Live: the frontend's multiplayer menu drops its Xbox Live item and calls System
// Link Online Play, and the pause menu of a match drops Xbox Live and its Controller item becomes Options, which opens
// the settings menu.
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include "common/mouse.h"
#include "common/ui.h"

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

X86_EXTERN_FUNC(__imp__sub_0020C3B0);
X86_FUNC(sub_0020C3B0) {
    mouse::InMenu();
    __imp__sub_0020C3B0(ctx, base);
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
