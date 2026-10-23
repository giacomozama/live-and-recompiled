// Ropes. CHAREFFECTS_TYPE_ROPE (55) and CHAREFFECTS_TYPE_ROPE_DRAWN_BELOW_WATER (102, PooCabin's) share their
// handlers in the effect type table (0x590658, 0x70 bytes a type: a 64-byte name, then handlers). 0x1586B0 makes one:
// [rope+0x54] its nodes (0x18 bytes: position, then velocity as the last step's displacement), [rope+0x60] their
// count (40) from the bottom (0) to the anchor at the top (n - 1, never moved), [rope+0x58] the segment length.
// The effect pass 0xD93F0 runs each type's +0x44 handler once a frame, the rope's step 0x158C10 (rope at [esp+4]):
//
// - A climbing character ([rope+0x64], its climb record [char+0x568]) pushes the node it holds ([rec+0xEE]) by the
//   animation's push [rec+0xE8] (set by the script command 0x30AA00, zeroed when its frame count [rec+0xED] runs
//   out) and by its own speed [char+0x110], both along its facing ([char+0x171], plus [rec+0xEC] for the push), and
//   takes 500 off that node's vertical velocity as its weight. A character letting go (0x159540) leaves an impulse
//   [rope+0x6C] on a node [rope+0x71], decaying with dt.
// - 0x158E38: one pass from the anchor down. Each node's velocity takes the gravity across its segment (9 a frame,
//   times 0.4875), keeps 0.975 of itself, and adds 1.24 times the way to its neighbours' midpoint; the node moves by
//   it and is put back at the segment length from the node above, and the velocity becomes what it moved.
// - 0x15919D: past 250 for the summed sideways velocity ([esp+0x14]) a creak (rope1..9) plays at the bottom node's
//   candidate ([esp+0x1C]).
//
// None of it is scaled by time, and the bend term is near the explicit step's limit for a zigzag (2.48 of 4), so
// kinks run up and down the rope and outlast the damping by seconds; the weight is a velocity on one node that the
// single pass never carries to the nodes above, and it jumps to the next node as the character climbs. That's the
// writhing rope the console shows too.
//
// rope::g_physics replaces the pass with a weighted chain (position based, its constraints solved in substeps of the
// frame's time): gravity at a third, air drag, the character's weight as mass on the two nodes it holds with the rope
// above them damped, its pushes leaning it by their share of its weight as on the console, and kinks damped by easing
// each node's velocity toward its neighbours'. The prelude and the creak run as they did.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/rope.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

bool rope::g_physics = true;

namespace {

using kernel::G;
using kernel::R32;

constexpr uint32_t kDt = 0x7FA21C, kLevel = 0x7FA1F8;
constexpr int kMaxNodes = 40;  // the step's stack copy holds 40 (0x68..0x248)

// gravity in units a console frame squared (the console's 9 * 0.4875 is about three times the units' 9.8 m/s^2,
// at Conker's 140 units of reach); drag and the kink damping per second; the character's mass in rope nodes
constexpr float kGravity = 1.6f, kDrag = 0.35f, kSmooth = 0.35f, kMass = 12.0f;
constexpr float kHeldDrag = 1.0f;  // per second, on the rope from a holding character's hands up: it barely swings
constexpr float kWeight = 500.0f;  // the weight the prelude gives a holding character, a velocity a frame
constexpr float kReleaseKeep = 0.9369f;  // what a release's impulse keeps a console frame at 30 FPS: 1 - 0.1^1.2
// the share of a grabbing character's speed the rope takes, and its cap (units a console frame)
constexpr float kArrive = 0.4f, kArriveMax = 12.0f;
constexpr int kSubsteps = 16, kIterations = 8;  // substeps per console frame, constraint passes per substep

struct Vec {
    float x, y, z;
    Vec operator+(Vec o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec operator-(Vec o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec operator*(float s) const { return {x * s, y * s, z * s}; }
};
float Length(Vec v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec& Pos(uint32_t nodes, int i) { return G<Vec>(nodes + 0x18 * uint32_t(i)); }
Vec& Vel(uint32_t nodes, int i) { return G<Vec>(nodes + 0x18 * uint32_t(i) + 0xC); }

// the velocities before the prelude, to tell its pushes from the rope's own motion
struct Snapshot {
    uint32_t rope = 0;
    Vec vel[kMaxNodes];
};
thread_local Snapshot g_snap;

// a grab's momentum (0x159450), for the rope's next step: the character's velocity there, in units a console frame
struct Arrival {
    uint32_t rope = 0, chr = 0;
    Vec vel;
};
Arrival g_arrival;

bool Trace() {
    static const bool trace = std::getenv("XBE_TRACE_ROPE") != nullptr;
    return trace;
}

// how kinked the rope is: the mean sideways distance of a node from its neighbours' midpoint
float Kinks(uint32_t nodes, int n) {
    float sum = 0;
    for (int i = 1; i + 1 < n; i++) {
        Vec d = (Pos(nodes, i - 1) + Pos(nodes, i + 1)) * 0.5f - Pos(nodes, i);
        sum += std::sqrt(d.x * d.x + d.z * d.z);
    }
    return n > 2 ? sum / float(n - 2) : 0.0f;
}

}  // namespace

// [[hook]] at 0x158E38, after the prelude: the chain step in place of the console's pass, then on to the creak test
// (0x15919D), with the summed sideways velocity at [esp+0x14] and the bottom node at [esp+0x1C]
bool ConkerRopeStep(X86Context& ctx, uint8_t* base) {
    const uint32_t r = ctx.esi;
    const int n = base[r + 0x60];
    if (!rope::g_physics || g_snap.rope != r || n < 3 || n > kMaxNodes) return false;
    const uint32_t nodes = R32(r + 0x54);
    const float seg = G<float>(r + 0x58);
    float frames = G<float>(kDt) * 30.0f;  // the frame in console frames
    if (!(frames > 0.0f)) frames = 1.0f;
    frames = std::min(frames, 4.0f);

    Vec p[kMaxNodes], v[kMaxNodes], x[kMaxNodes];
    float w[kMaxNodes];
    for (int i = 0; i < n; i++) p[i] = Pos(nodes, i), v[i] = Vel(nodes, i), w[i] = 1.0f;
    w[n - 1] = 0.0f;

    // the character: its weight goes on the two nodes it holds (as 0x1596B0 places it) instead of a velocity, and the
    // rope from its hands up is damped (held from it on)
    int held_from = n;
    const uint32_t chr = R32(r + 0x64);
    if (chr) {
        const uint32_t rec = R32(chr + 0x568);
        const int held = base[rec + 0xEE];
        if (held < n) v[held].y += kWeight;
        const float at = float(n - int(140.0f / seg) - 1) * G<float>(rec + 0xE0);
        const int g = std::clamp(int(at), 0, n - 2);
        const float frac = std::clamp(at - float(g), 0.0f, 1.0f);
        w[g] = 1.0f / (1.0f + kMass * (1.0f - frac));
        if (g + 1 < n - 1) w[g + 1] = 1.0f / (1.0f + kMass * frac);
        held_from = g;
    }
    // what the prelude added: with no one holding on, momentum given to a rope node per console frame (a release's
    // impulse). A holding character's pushes (the animation's, 70 * 0.95 toward its facing nearly every frame while
    // it hangs, and its speed's) went on the node it holds against the 500 of weight, so the console's pass leaned
    // that node by push / weight: they're that share of gravity, sideways, on the character
    Vec push = {0, 0, 0};
    for (int i = 0; i < n - 1; i++) {
        push = push + (v[i] - g_snap.vel[i]);
        v[i] = chr ? g_snap.vel[i] : g_snap.vel[i] + (v[i] - g_snap.vel[i]) * (w[i] * frames);
    }
    if (chr) {
        Vec lean = push * (kGravity / kWeight * frames);
        // the step after a grab: the character arrives moving, and the rope swings off with it
        if (g_arrival.rope == r && g_arrival.chr == chr) lean = lean + g_arrival.vel, g_arrival = {};
        for (int i = held_from; i <= held_from + 1 && i < n - 1; i++) v[i] = v[i] + lean;
    }
    v[n - 1] = {0, 0, 0};

    const bool clamp_x = R32(kLevel) == 0x39;  // the console keeps that level's rope off a wall: x at most 1150
    const int steps = std::max(1, int(std::ceil(frames * kSubsteps)));
    const float h = frames / float(steps);
    const float drag = std::exp(-kDrag * h / 30.0f), ease = 1.0f - std::pow(1.0f - kSmooth, h);
    const float hold = std::exp(-kHeldDrag * h / 30.0f);
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < n - 1; i++) {
            v[i].y -= kGravity * h;
            v[i] = v[i] * (i >= held_from ? hold : drag);
            x[i] = p[i] + v[i] * h;
        }
        x[n - 1] = p[n - 1];
        // alternate the sweep's direction so the weight reaches the anchor as fast as the slack reaches the end
        for (int it = 0; it < kIterations; it++) {
            for (int k = 0; k < n - 1; k++) {
                const int i = it & 1 ? k : n - 2 - k;
                const Vec d = x[i] - x[i + 1];
                const float len = Length(d), ws = w[i] + w[i + 1];
                if (len <= 0.0f || ws <= 0.0f) continue;
                const float c = (len - seg) / (len * ws);
                x[i] = x[i] - d * (c * w[i]);
                x[i + 1] = x[i + 1] + d * (c * w[i + 1]);
            }
        }
        if (clamp_x)
            for (int i = 0; i < n - 1; i++) x[i].x = std::min(x[i].x, 1150.0f);
        for (int i = 0; i < n - 1; i++) v[i] = (x[i] - p[i]) * (1.0f / h);
        // ease each velocity toward its neighbours' mean: a kink loses its motion, a swing keeps it
        Vec prev = v[0];
        for (int i = 0; i < n - 1; i++) {
            const Vec cur = v[i];
            const Vec mean = i ? (prev + v[i + 1]) * 0.5f : v[1];
            v[i] = cur + (mean - cur) * ease;
            prev = cur;
        }
        for (int i = 0; i < n - 1; i++) p[i] = x[i];
    }

    float sideways = 0;
    for (int i = 0; i < n - 1; i++) {
        Pos(nodes, i) = p[i];
        Vel(nodes, i) = v[i];
        sideways += std::fabs(v[i].x) + std::fabs(v[i].z);
    }
    G<float>(ctx.esp + 0x14) = sideways;
    G<Vec>(ctx.esp + 0x1C) = p[0];
    return true;
}

// 0x158C10: the rope's step. Keeps the velocities from before the prelude for the hook above. The prelude decays a
// release's impulse by 1 - 0.1^(fps / 25) a frame, 0.937 at 30 FPS but 0.996 at 60, where the node it pushes stays
// kinked for seconds; with rope::g_physics it decays at the console's rate in time. XBE_TRACE_ROPE logs each step of
// a rope a character holds or a release pushes, with what the character fed it.
X86_EXTERN_FUNC(__imp__sub_00158C10);
X86_FUNC(sub_00158C10) {
    const uint32_t r = R32(ctx.esp + 4);
    const int n = base[r + 0x60];
    const uint32_t nodes = R32(r + 0x54);
    if (n <= kMaxNodes) {
        g_snap.rope = r;
        for (int i = 0; i < n; i++) g_snap.vel[i] = Vel(nodes, i);
    }
    const uint32_t chr = R32(r + 0x64);
    float push = 0, speed = 0, climb = 0, release = G<float>(r + 0x6C);
    int held = -1, frames = 0, facing = 0;
    if (Trace() && chr) {
        const uint32_t rec = R32(chr + 0x568);
        push = G<float>(rec + 0xE8), climb = G<float>(rec + 0xE0), speed = G<float>(chr + 0x110);
        held = base[rec + 0xEE], frames = base[rec + 0xED], facing = base[chr + 0x171];
    }
    __imp__sub_00158C10(ctx, base);
    g_snap.rope = 0;
    if (rope::g_physics && release != 0.0f) {
        const float elapsed = std::clamp(G<float>(kDt) * 30.0f, 0.0f, 4.0f);
        const float left = release * std::pow(kReleaseKeep, elapsed);
        G<float>(r + 0x6C) = std::fabs(left) < 0.01f ? 0.0f : left;
    }
    if (!Trace() || (!chr && release == 0.0f)) return;
    float fastest = 0;
    for (int i = 0; i + 1 < n; i++) {
        const Vec v = Vel(nodes, i);
        fastest = std::max(fastest, std::sqrt(v.x * v.x + v.z * v.z));
    }
    const Vec end = Pos(nodes, 0);
    LOGR("rope: %08X %s dt %.4f held %d climb %.3f push %.2f (%d frames) speed %.2f facing %d release %.2f | kinks "
         "%.2f fastest %.2f end %.1f %.1f %.1f",
         r, rope::g_physics ? "chain" : "console", G<float>(kDt), held, climb, push, frames, speed, facing, release,
         Kinks(nodes, n), fastest, end.x, end.y, end.z);
}

// 0x159450: a character (esi) grabs the rope with id bl if one is in reach, setting [rope+0x64] and [rec+0xDC]. Its
// speed [chr+0x110] along its facing [chr+0x171] (as the prelude pushes: x by the cosine, z by minus the sine, the
// table at 0x743090) is what it had running or jumping in; with rope::g_physics a share of it goes to the rope's next
// step, so the rope takes the swing.
X86_EXTERN_FUNC(__imp__sub_00159450);
X86_FUNC(sub_00159450) {
    const uint32_t chr = ctx.esi;
    const uint32_t rec = R32(chr + 0x568);
    if (!rec) {
        __imp__sub_00159450(ctx, base);
        return;
    }
    const uint32_t before = R32(rec + 0xDC);
    const float speed = G<float>(chr + 0x110);
    const float a = float(base[chr + 0x171]) * (6.2831853f / 256.0f);
    __imp__sub_00159450(ctx, base);
    const uint32_t r = R32(rec + 0xDC);
    if (!rope::g_physics || !r || r == before || R32(r + 0x64) != chr) return;
    const float s = std::clamp(speed * kArrive, -kArriveMax, kArriveMax);
    g_arrival = {r, chr, {std::cos(a) * s, 0.0f, -std::sin(a) * s}};
    if (Trace()) LOGR("rope: %08X grabbed at speed %.2f facing %d", r, speed, base[chr + 0x171]);
}
