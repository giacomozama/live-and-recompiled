// Dumbots (Rare's Bots\bots_gamestate.cpp, bots_nav.cpp; the code at 0x335000..0x37A000), read so far for the bots
// that blow up the vehicle they took on its spawn pad.
//
// The bots. [0x84B330] is the bots' state; a bot's brain is at +0x38 + index * 0x8CC (index its record's in the table
// at [0x84A5F8]): +2 its state (a short; 0x33B8F0 runs the machine each frame, states from 30 by the table at
// 0x74B858), +0x101 its goal's kind, +0x98 flags, +0x138 the carrot its vehicle drives at (moved along the path by
// 0x373480, +0x144 how far ahead). The bot's control (0x3719F0) sets the throttle at [record+0x568]+0xD0 (0..1, from
// the carrot's distance) and, while the bot drives (record+0x135, the vehicle's index + 1, and the bot in the
// vehicle's seat [[vehicle+0x570]+0x1F0]), calls 0x395290 (edi the vehicle's record, the carrot, the throttle, a flag).
// Dumbots mode is [0x863D04] == 1.
//
// The drive (0x395290, on the vehicle's object V = [vehicle+0x570]): the carrot's bearing against the vehicle's
// heading (record+0x114, degrees) is the steering error; then a stuck recovery on V+0xC28 (stage) and V+0xC2C (timer):
// stage 0, while the speed V+0x79C is under the throttle (all but stopped: the speed scale is the throttle's x 40) for
// 2 s, goes to stage 1, which adds sin(t pi) x 45 degrees to the error (the bot's view swings left and right) and,
// still stopped 2 s more, goes to stage 2, which backs up with the steering flipped for 3 s, then stage 0 again. The
// vehicle's own drive (vtable +0x3C, 0x387FA0 for the jeeps and bikes) turns that into the actions' steering (+0x12C)
// and throttle (+0x130) at 0x763260 + slot * 0x1B0, backing up on its own when the carrot is over 100 degrees off.
//
// The pad. A vehicle made at a terminal (0x389800..) has V+0xCA0 = 0 and V+0xC9C = 8.5 s (turrets and the like
// start with 1, no count). Each frame (0x3916F0 and its siblings) the vehicle's collision spheres (V+0x898, radii
// V+0xB98, V+0x890 of them) are tested against the vehicle spawners (records 16..127 of kind byte 0x36 in
// [0x84A5FC], centre at [record+0x570]+0xC): within 420 + radius of one it is still on its pad, and once none is it
// has left (V+0xCA0 = 1; after that a spawner's sphere pushes it out). 0x397770 counts V+0xC9C down while V+0xCA0
// is 0 (a human driver sees the seconds) and sets V+0xCA4 at 0; 0x397730 then deals it 100 (0x2F06F0(record, 0x74,
// 0, 100, 1)): it blows up, and its blast kills whoever stands by. A bot that can't get going loses 4 s of the 8.5 to
// the stuck recovery before it even backs up.
//
// The flicker. The jeeps' and bikes' drive (0x387FA0, and the flame jeep's 0x3AAB00) backs up while the carrot is more
// than 100 degrees off, or 85 while the vehicle rolls backwards (V+0x79C under -0.05). A vehicle made facing away from
// its first carrot backs round to 85 degrees and is told to go forward,
// but it still rolls back, so the bar stays 85 and the next wobble past it backs up again: the commands flip each
// frame (steering full one way and the other, which the bot's view follows), the vehicle stands at -0.45, the stuck
// recovery's 2 s run out and it wiggles, and 3.5 s go before it drives off. The bar now goes by the way the drive
// sent it last (V+0x1322, set by the drive each frame: 1 backing up), so a turn that has backed round to 85 drives
// forward until the carrot is past 100 again.
//
// XBE_BOT_TRACE=1 logs each driving bot a few times a second while its vehicle is on its pad (and on every state or
// stuck stage change), and the pad countdown's blasts.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "core/log.h"
#include "core/diag.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

using kernel::G;

constexpr uint32_t kRecords = 0x84A5F8;  // the character records' table
constexpr uint32_t kRecordSize = 0x6D0;
constexpr uint32_t kRecordCount = 0x80;
constexpr uint32_t kBots = 0x84B330;     // -> the bots' state
constexpr uint32_t kBrain = 0x38, kBrainSize = 0x8CC;
constexpr uint32_t kActions = 0x763260, kActionsSize = 0x1B0;
constexpr uint32_t kMode = 0x863D04;     // 1 Dumbots, 2 system link

bool Trace() {
    static const bool on = diag::Env("XBE_BOT_TRACE") != nullptr;
    return on;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int Index(uint32_t record) {
    const uint32_t table = kernel::R32(kRecords);
    if (!table || record < table) return -1;
    const uint32_t i = (record - table) / kRecordSize;
    return i < kRecordCount && table + i * kRecordSize == record ? int(i) : -1;
}

// what was last logged for a vehicle (by its record's index)
struct Seen {
    double at = 0;
    int state = -1, stage = -1;
    uint32_t object = 0;
};
Seen g_seen[kRecordCount];

}  // namespace

namespace {

// the AI drive's bar for backing up, in the float at `slot`: 85 degrees while it backs up, else 100
void ReverseBar(uint32_t vehicle, uint32_t slot) {
    G<float>(slot) = G<uint8_t>(vehicle + 0x1322) ? 85.0f : 100.0f;
}

}  // namespace

// [[hook]] at 0x387FF7 in 0x387FA0 (esi the vehicle's object), the bar chosen into [esp+8] by the speed
void ConkerBotReverseJeep(X86Context& ctx, uint8_t* base) {
    (void)base;
    ReverseBar(ctx.esi, ctx.esp + 8);
}

// [[hook]] at 0x3AAB53 in 0x3AAB00 (ecx the vehicle's object), the same at [esp]
void ConkerBotReverseFlameJeep(X86Context& ctx, uint8_t* base) {
    (void)base;
    ReverseBar(ctx.ecx, ctx.esp);
}

X86_EXTERN_FUNC(__imp__sub_00395290);
X86_FUNC(sub_00395290) {
    if (!Trace()) return __imp__sub_00395290(ctx, base);
    const uint32_t vehicle = ctx.edi, object = kernel::R32(vehicle + 0x570);
    const uint32_t driver = object ? kernel::R32(object + 0x1F0) : 0;
    const uint32_t carrot = kernel::R32(ctx.esp + 4);
    const float throttle = G<float>(ctx.esp + 8);
    const uint32_t flag = kernel::R32(ctx.esp + 12);
    __imp__sub_00395290(ctx, base);
    const int vi = Index(vehicle), di = driver ? Index(driver) : -1;
    if (vi < 0 || di < 0) return;
    const uint32_t bots = kernel::R32(kBots);
    const uint32_t brain = bots ? bots + kBrain + uint32_t(di) * kBrainSize : 0;
    const int state = brain ? int(G<int16_t>(brain + 2)) : -1;
    const int stage = G<uint8_t>(object + 0xC28);
    const bool on_pad = kernel::R32(object + 0xCA0) == 0;
    Seen& seen = g_seen[vi];
    const double now = Now();
    const bool fresh = seen.object != object;
    const bool changed = fresh || state != seen.state || stage != seen.stage;
    if (!changed && now - seen.at < (on_pad ? 0.25 : 2.0)) return;
    seen = {now, state, stage, object};
    const float vx = G<float>(vehicle + 0x78), vy = G<float>(vehicle + 0x7C), vz = G<float>(vehicle + 0x80);
    const float cx = G<float>(carrot), cy = G<float>(carrot + 4), cz = G<float>(carrot + 8);
    const float bearing = std::atan2(cx - vx, cz - vz) * 57.29578f;
    const uint32_t plan = kernel::R32(driver + 0x568);
    uint32_t slot = G<uint16_t>(driver + 0x60);
    if (slot >= 0x14) slot = 0x14;
    const uint32_t actions = kActions + slot * kActionsSize;
    LOGR("bots: veh %d (%08X) bot %d state %d goal %d flags %08X | pos %.0f %.0f %.0f heading %.0f speed %.2f | "
         "carrot %.0f %.0f %.0f (dist %.0f, bearing %.0f) ahead %.0f throttle %.2f flag %u | goal %.0f %.0f %.0f "
         "nodes %d/%d | stuck %d %.2f | pad %s %.2f | out steer %.2f throttle %.2f",
         vi, vehicle, di, state, brain ? int(G<int8_t>(brain + 0x101)) : -1, brain ? kernel::R32(brain + 0x98) : 0,
         vx, vy, vz, G<float>(vehicle + 0x114), G<float>(object + 0x79C), cx, cy, cz,
         std::sqrt((cx - vx) * (cx - vx) + (cz - vz) * (cz - vz)), bearing, brain ? G<float>(brain + 0x144) : 0.0f,
         throttle, flag, plan ? G<float>(plan + 0x4BC) : 0.0f, plan ? G<float>(plan + 0x4C0) : 0.0f,
         plan ? G<float>(plan + 0x4C4) : 0.0f, plan ? int(G<int16_t>(plan + 0x498)) : -1,
         plan ? int(G<int16_t>(plan + 0x4C8)) : -1, stage, G<float>(object + 0xC2C), on_pad ? "on" : "off",
         G<float>(object + 0xC9C), G<float>(actions + 0x12C), G<float>(actions + 0x130));
}

// 0x397730 (esi the vehicle's object): the pad countdown has run out (V+0xCA4), the vehicle takes 100
X86_EXTERN_FUNC(__imp__sub_00397730);
X86_FUNC(sub_00397730) {
    const uint32_t object = ctx.esi;
    static uint32_t logged = 0;  // once per vehicle
    if (Trace() && object && G<uint8_t>(object + 0xCA4) && object != logged) {
        logged = object;
        const uint32_t vehicle = kernel::R32(object + 4), driver = kernel::R32(object + 0x1F0);
        const int vi = vehicle ? Index(vehicle) : -1, di = driver ? Index(driver) : -1;
        const uint32_t bots = kernel::R32(kBots);
        LOGR("bots: veh %d (%08X) pad countdown ran out, driver %d (state %d), mode %u: it blows up", vi, vehicle, di,
             bots && di >= 0 ? int(G<int16_t>(bots + kBrain + uint32_t(di) * kBrainSize + 2)) : -1,
             kernel::R32(kMode));
    }
    __imp__sub_00397730(ctx, base);
}
