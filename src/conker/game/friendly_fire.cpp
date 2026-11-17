// No friendly fire (settings.toml no_friendly_fire, syslink::SessionNoFriendlyFire): in a match a player's teammates
// don't hurt them or the vehicle they drive. The rule is a row of the game's match configure page.
//
// The hits. Every hit on a character or a vehicle goes through 0x2EDDD0 (eax the victim, a record of the table at
// [0x84A5F8], 0x6D0 bytes each, +0x64 its kind: 1 a player or bot, 0x35 a vehicle; cdecl, its first argument the
// record that dealt it, the next the damage table and the type's index into it): a shot or a blow from 0x2EF1D0's call
// at 0x2EF520, a blast (0x2F0930) from 0x2F06AC and 0x2F08EF. Once it has applied the hit it sends it to the other
// machines (0x281B80 at 0x2EEE6B), which finds the player behind the record that dealt it: a blast's is a scratch
// record (0x80, at +0x36800) whose +0x23C ref holds the thrower, and 0x315260 (eax the ref) follows such refs to the
// record at their end; a vehicle is its driver, by 0x394EE0 (eax the vehicle: the driver's index or -1) or else the
// record at [[vehicle+0x570]+0x1F0]. The same lookup here finds the attacker, and a vehicle hit stands for its driver.
//
// A player's team is the dword its +0x238 points to, as the game's own teammate test (0x3488E0, the name tags'
// colour) compares them; in a free-for-all each has its own (a vehicle's own team is the neutral one). That test also
// counts as a teammate whoever wears the viewer's side (a disguised Sneeker), which this does not: a disguise still
// takes the other side's fire.
//
// With the rule on, in a match, a player hit by another player of its team, or a vehicle driven by one, takes nothing:
// the function returns before any of its work (no damage, flinch or hit message). The medgun's heal (damage type 0x64)
// is never dropped, and tells medbeam.cpp who heals whom. A player's own blasts still hurt it and its vehicle; an
// empty vehicle takes anyone's. Each machine decides the hits its own players deal and every copy
// in a session plays the host's rule, so the hit is dropped wherever it starts. XBE_FF_TRACE=1 logs each hit on a
// player or vehicle by a player, its teams and what became of it.
//
// The menu. The match's configure page (0x290F60, FT_CONFIGURE: Time Limit, Lives Limit, Flag Rest, Victory Swap,
// ..., Reset Defaults; Dumbots' and system link's alike) is built and run every frame: its rows go into a vector at
// [esp+0x20] by 0x244870 (ecx a row: the value's object, its label's key name in UTF-16, a handler, flags 3), each
// value an object with a reference count at +4 (an on/off value: vtable 0x5FA138, +8 the byte it shows and sets, +0xC
// set to show it inverted, as No Vehicles shows the vehicles' byte), then 0x245E80 draws the list and takes its input
// (each row's label found by its key name in the text table by 0x3F9CB0), and the vector goes (releasing each value).
// Friendly Fire is added before Reset Defaults: one object of our own, its count never let down to 0, over a byte of
// our own; a change made there is the setting's. In a system link session the row shows the session's rule, and while
// it holds other players (or this copy joined it) a change is put back: the players joined took the rule as the host
// advertised it.
#include <cstdint>
#include <cstdlib>

#include "app/app.h"
#include "common/link.h"
#include "common/medbeam.h"
#include "common/menus.h"
#include "common/warp.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kRecords = 0x84A5F8;   // the character records' table
constexpr uint32_t kRecordSize = 0x6D0;
constexpr uint32_t kRecordCount = 0x80;   // the scratch record (a blast's) is the last, 0x80
constexpr uint32_t kKind = 0x64;          // 1 a player or bot, 0x35 a vehicle
constexpr uint32_t kKindPlayer = 1, kKindVehicle = 0x35;
constexpr uint32_t kTeam = 0x238;         // -> the team's id
constexpr uint32_t kRef = 0x23C;          // -> the ref to the record that owns this one (a blast's thrower)
constexpr uint32_t kDriverSeat = 0x570;   // a vehicle's seat: +0x1F0 the record in it
constexpr uint32_t kMatchFlag = 0x7819D5;  // byte: a match runs
constexpr uint32_t kIsHost = 0x862C5C;     // byte: this copy hosts
constexpr uint32_t kMachines = 0x864EC1;   // byte: the machines in the session
constexpr uint32_t kTypeMedgun = 0x64;     // the hit's damage type (fourth argument): the medgun's heal (medbeam.cpp)

bool Trace() {
    static const bool on = std::getenv("XBE_FF_TRACE") != nullptr;
    return on;
}

// the record at a table index, 0 past it
uint32_t Record(uint32_t index) {
    const uint32_t table = kernel::R32(kRecords);
    return table && index < kRecordCount ? table + index * kRecordSize : 0;
}

// a guest function run from the host with the registers set: eax after it, the context as it was
uint32_t Call(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t eax, uint32_t ecx,
              uint32_t esi) {
    const X86Context saved = ctx;
    ctx.eax = eax, ctx.ecx = ecx, ctx.esi = esi;
    X86_PUSH32(kernel::R32(saved.esp));  // its return pops this
    fn(ctx, base);
    const uint32_t out = ctx.eax;
    ctx = saved;
    return out;
}

}  // namespace

X86_EXTERN_FUNC(sub_00315260);
X86_EXTERN_FUNC(sub_00394EE0);
X86_EXTERN_FUNC(sub_00244870);

namespace {

// a vehicle's driver (0x281B80's lookup), 0 for none
uint32_t Driver(X86Context& ctx, uint8_t* base, uint32_t vehicle) {
    const uint32_t index = Call(ctx, base, sub_00394EE0, vehicle, ctx.ecx, ctx.esi);
    uint32_t driver = index != 0xFFFFFFFFu ? Record(index & 0xFFFF) : 0;
    if (!driver)
        if (const uint32_t seat = kernel::R32(vehicle + kDriverSeat)) driver = kernel::R32(seat + 0x1F0);
    return driver && kernel::R32(driver + kKind) == kKindPlayer ? driver : 0;
}

// the player behind the record that dealt a hit (0x281B80's lookup), 0 for none
uint32_t Attacker(X86Context& ctx, uint8_t* base, uint32_t from) {
    if (!from) return 0;
    if (kernel::R32(from + kKind) != kKindPlayer)
        if (const uint32_t ref = kernel::R32(from + kRef))
            if (const uint32_t owner = Call(ctx, base, sub_00315260, ref, ctx.ecx, ctx.esi)) from = owner;
    if (kernel::R32(from + kKind) == kKindVehicle) return Driver(ctx, base, from);
    return kernel::R32(from + kKind) == kKindPlayer ? from : 0;
}

// a record's team id (0 and 1 the two sides), -1 for none
int64_t Team(uint32_t record) {
    const uint32_t team = kernel::R32(record + kTeam);
    return team ? int64_t(kernel::R32(team)) : -1;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002EDDD0);
X86_FUNC(sub_002EDDD0) {
    const uint32_t victim = ctx.eax;
    if (!victim || !kernel::G<uint8_t>(kMatchFlag) || !warp::Multiplayer()) return __imp__sub_002EDDD0(ctx, base);
    const uint32_t kind = kernel::R32(victim + kKind);
    // the medgun's heal: its beam, and never dropped (a teammate is all it hits)
    if (kernel::R32(ctx.esp + 0x10) == kTypeMedgun) {
        // the healer is the record that dealt it (0x2F0470's scratch one) followed to its owner; a local heal also
        // comes straight from the heal tick (medbeam.cpp's 0x323BA0)
        if (kind == kKindPlayer)
            if (const uint32_t healer = Attacker(ctx, base, kernel::R32(ctx.esp + 4)); healer && healer != victim)
                medbeam::Heal(healer, victim);
        return __imp__sub_002EDDD0(ctx, base);
    }
    if (!syslink::SessionNoFriendlyFire() && !Trace()) return __imp__sub_002EDDD0(ctx, base);
    if (kind != kKindPlayer && kind != kKindVehicle) return __imp__sub_002EDDD0(ctx, base);
    // the player the hit is on: the victim, or the vehicle's driver
    const uint32_t player = kind == kKindPlayer ? victim : Driver(ctx, base, victim);
    const uint32_t from = kernel::R32(ctx.esp + 4);
    const uint32_t attacker = player ? Attacker(ctx, base, from) : 0;
    if (!attacker || attacker == player) return __imp__sub_002EDDD0(ctx, base);
    const int64_t ours = Team(player), theirs = Team(attacker);
    const bool friendly = ours >= 0 && ours == theirs;
    const bool drop = friendly && syslink::SessionNoFriendlyFire();
    if (Trace())
        LOGR("friendly fire: %s %08X (player %08X, team %d) hit by %08X (from %08X, team %d)%s",
             kind == kKindPlayer ? "player" : "vehicle", victim, player, int(ours), attacker, from, int(theirs),
             drop ? ": dropped" : friendly ? ": a teammate's, friendly fire on" : "");
    if (drop) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002EDDD0(ctx, base);
}

namespace {

constexpr uint32_t kOnOffVtable = 0x5FA138;  // an on/off value's
constexpr uint32_t kRowHandler = 0x1E32A0;   // Even Teams' handler (returns 1)
constexpr uint32_t kRowFlags = 3;

// in guest memory, made once: the value (0x10 bytes), the row (0x10), the byte it shows (game thread)
uint32_t g_block = 0;
uint32_t g_key = 0;      // the row's key name, UTF-16
int g_shown = -1;        // the byte as last set here, -1 before

uint32_t Block() {
    if (g_block) return g_block;
    g_block = mem::AllocSystem(0x30);
    const uint32_t value = g_block, row = g_block + 0x10, byte = g_block + 0x20;
    kernel::W32(value, kOnOffVtable);
    kernel::W32(value + 4, 1);  // this file's reference: the list never frees it
    kernel::W32(value + 8, byte);
    kernel::W32(value + 0xC, 0);  // not inverted: on is friendly fire on
    static const char16_t kKey[] = u"FT_Adv_FriendlyFire";
    g_key = mem::AllocSystem(sizeof kKey);
    for (size_t i = 0; i < sizeof kKey / 2; i++) kernel::G<uint16_t>(g_key + uint32_t(i) * 2) = kKey[i];
    kernel::W32(row, value);
    kernel::W32(row + 4, g_key);
    kernel::W32(row + 8, kRowHandler);
    kernel::W32(row + 0xC, kRowFlags);
    return g_block;
}

// the row's label in the game's language (English for Japanese)
uint32_t Label() {
    if (const uint32_t text = menus::GameText("Friendly Fire", "Eigenbeschuss", "Tir ami", "Fuego amigo", "Fuoco amico"))
        return text;
    static uint32_t english = 0;
    if (!english) {
        static const char16_t kText[] = u"Friendly Fire";
        english = mem::AllocSystem(sizeof kText);
        for (size_t i = 0; i < sizeof kText / 2; i++) kernel::G<uint16_t>(english + uint32_t(i) * 2) = kText[i];
    }
    return english;
}

}  // namespace

// [[hook]] at 0x291B6C in the configure page's build (0x290F60), before the Reset Defaults row: the Friendly Fire row,
// its byte first taken back into the setting if the page changed it
void ConkerFriendlyFireRow(X86Context& ctx, uint8_t* base) {
    const uint32_t block = Block(), byte = block + 0x20;
    const bool session = syslink::InSession();
    const bool fixed = session && (!kernel::G<uint8_t>(kIsHost) || kernel::G<uint8_t>(kMachines) > 1);
    const int now = kernel::G<uint8_t>(byte) ? 1 : 0;
    if (g_shown >= 0 && now != g_shown && !fixed) {
        syslink::g_no_friendly_fire = now == 0;
        app::SaveChanged();
        if (Trace()) LOGR("friendly fire: set %s on the configure page", now ? "on" : "off");
    }
    const bool off = session ? syslink::SessionNoFriendlyFire() : syslink::g_no_friendly_fire;
    g_shown = off ? 0 : 1;
    kernel::G<uint8_t>(byte) = uint8_t(g_shown);
    Call(ctx, base, sub_00244870, ctx.eax, block + 0x10, ctx.esp + 0x20);
}

// [[hook]] at 0x245FC2 in the list's run (0x245E80), where a row's label (eax, found by 0x3F9CB0 in the text table, or
// its fallback) is copied into the row ([esp+0x14] its key name): the Friendly Fire row's label
void ConkerFriendlyFireLabel(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (g_key && kernel::R32(ctx.esp + 0x14) == g_key) ctx.eax = Label();
}
