// The Creaky Crate (model type 21, characters\creakycrate: creaky_stance, creaky_hop, creaky_hop_long,
// crate_stance_squash; its clip table at 0x706AC8, 12 bytes an action) gets about by hopping and slamming down. Its hop
// clips end with it up in the air and the next one starts on the ground, so the fall back down is a single tick: at 30
// FPS one picture's step among others as coarse, at 60 (and more with frame generation) a teleport among smooth ones.
//
// Here the slam falls. The crate's bones are a matrix each in the instance's data (see squirrel_tail.cpp: the skeleton
// node 0x3EC7D0 hands [data + node[0xC] + 4] to the palettes under it, [node+0x30] of them, 64 bytes, row vectors,
// model space to the world), written once a frame by its animation before any of its draws. When those matrices jump
// in one tick, by far more than they moved the tick before, the gap (the pose shown before less the new one, per bone)
// is carried and taken off over kFall, slowly first and then faster, as a fall from rest (1 - u^2): the clip that
// follows plays on under it, so it is the new clip's own motion from the start, offset by a shrinking amount. The game
// itself (where the crate is, its hits, sounds and dust) is not touched; only the matrices it draws with.
//
// Only at 60 FPS and up (display::GameRate): at 30 the one picture's drop sits among steps as coarse and stays as the
// console had it.
//
// XBE_CRATE_TRACE=1 logs each crate frame's largest bone move and each jump taken. XBE_CRATE_FALL_MS sets the fall's
// length (120 ms), 0 turns it off.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/crate_fall.h"
#include "core/diag.h"
#include "common/display.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace {

constexpr uint32_t kCrateType = 21;  // CreakyCrate in the model names (0x6A58F0)
constexpr uint32_t kFrameTicks = 0x7FA20C;
constexpr uint32_t kMaxBones = 128;
constexpr float kJumpMin = 20.0f;   // world units a tick: a smaller move is never a jump
constexpr float kJumpTimes = 3.0f;  // and it must be this many times the tick before's
constexpr float kJumpMax = 1500.0f;  // past this it was put somewhere else (a respawn), shown at once

using Clock = std::chrono::steady_clock;

bool Tracing() {
    static const bool on = [] {
        const char* e = diag::Env("XBE_CRATE_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

float FallSeconds() {
    static const float s = [] {
        const char* e = diag::Env("XBE_CRATE_FALL_MS");
        if (!e || !*e) return 0.12f;
        return std::max(0.0f, float(std::atof(e))) / 1000.0f;
    }();
    return s;
}

struct Node {
    uint32_t node;
    Clock::time_point seen;
};
std::vector<Node> g_nodes;  // crates' skeleton nodes (a model's, shared by its instances)

struct Pose {
    uint32_t mats = 0;  // the instance's bone matrices, guest
    uint32_t count = 0;
    std::vector<float> fresh;  // as the animation last wrote them
    std::vector<float> shown;  // as left for the draws (fresh, or fresh plus the carried gap)
    std::vector<float> gap;    // shown less fresh when the last jump came
    float step = 0;            // the largest bone move a tick, the frame before
    float t = -1;              // seconds since the jump, < 0 none carried
    uint32_t frame = 0;        // d3d::Presents() when the animation last wrote it
    Clock::time_point seen;
};
std::vector<Pose> g_poses;

bool IsCrate(uint32_t node) {
    for (const Node& n : g_nodes)
        if (n.node == node) return true;
    return false;
}

float Weight(float t) {
    const float u = std::clamp(t / FallSeconds(), 0.0f, 1.0f);
    return 1.0f - u * u;
}

// the largest move of a bone's translation (row 3) from a to b
float LargestMove(const float* a, const float* b, uint32_t count) {
    float most = 0;
    for (uint32_t i = 0; i < count; i++) {
        const float* p = a + i * 16 + 12;
        const float* q = b + i * 16 + 12;
        const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
        most = std::max(most, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return most;
}

}  // namespace

void crate_fall::NoteDraw(uint32_t player) {
    if (kernel::R32(player + 0x68) != kCrateType) return;
    const uint32_t node = kernel::R32(player + 0x4EC);
    if (!node) return;
    const auto now = Clock::now();
    for (Node& n : g_nodes)
        if (n.node == node) {
            n.seen = now;
            return;
        }
    std::erase_if(g_nodes, [&](const Node& n) { return now - n.seen > std::chrono::seconds(10); });
    g_nodes.push_back({node, now});
    if (Tracing()) LOGR("crate: player %08X skeleton node %08X (%u bones)", player, node, kernel::R32(node + 0x30));
}

void crate_fall::Skeleton(uint32_t node, uint32_t data) {
    if (!node || g_nodes.empty() || !IsCrate(node) || FallSeconds() <= 0 || display::GameRate() <= 30) return;
    const uint32_t count = kernel::R32(node + 0x30);
    const uint32_t mats = kernel::R32(kernel::R32(node + 0xC) + data + 4);
    if (!count || count > kMaxBones || !mats) return;
    float* now_mats = reinterpret_cast<float*>(kernel::g_base + mats);
    const size_t floats = size_t(count) * 16, bytes = floats * sizeof(float);
    for (size_t i = 0; i < floats; i++)
        if (!std::isfinite(now_mats[i])) return;

    const auto now = Clock::now();
    Pose* pose = nullptr;
    for (Pose& p : g_poses)
        if (p.mats == mats) pose = &p;
    if (!pose) {
        std::erase_if(g_poses, [&](const Pose& p) { return now - p.seen > std::chrono::seconds(10); });
        pose = &g_poses.emplace_back();
        pose->mats = mats;
    }
    // a pose not seen for a while, or of another model: start again from this one
    if (pose->count != count || now - pose->seen > std::chrono::milliseconds(500)) {
        pose->count = count;
        pose->fresh.assign(now_mats, now_mats + floats);
        pose->shown = pose->fresh;
        pose->gap.assign(floats, 0.0f);
        pose->step = 0;
        pose->t = -1;
        pose->frame = d3d::Presents();
        pose->seen = now;
        return;
    }
    pose->seen = now;

    // the skeleton runs for each pass that draws the crate: what we left is still there (a later pass, or the
    // animation not run this frame: the game paused), or the animation wrote a pose: a new frame's, or the same
    // frame's again
    if (!std::memcmp(now_mats, pose->shown.data(), bytes)) return;
    const uint32_t frame = d3d::Presents();
    const bool new_frame = frame != pose->frame;
    pose->frame = frame;

    const uint32_t ticks = std::clamp(kernel::R32(kFrameTicks), 1u, 60u);
    if (new_frame) {
        const float step = LargestMove(pose->fresh.data(), now_mats, count) / float(ticks);
        const bool jump = step > kJumpMin && step > kJumpTimes * pose->step && step < kJumpMax;
        pose->step = step;
        if (jump) {
            // the gap from what was on screen, so a jump while a gap is carried keeps what is left of it
            for (size_t i = 0; i < floats; i++) pose->gap[i] = pose->shown[i] - now_mats[i];
            pose->t = 0;
            if (Tracing())
                LOGR("crate: %08X jumped %.1f a tick (%u ticks), falls over %.0f ms", mats, step, ticks,
                     FallSeconds() * 1000.0f);
        }
        if (pose->t >= 0) pose->t += float(ticks) / 60.0f;
        if (Tracing()) {
            const float* root = now_mats + 12;
            LOGR("crate: %08X moved %.1f a tick, root %.1f %.1f %.1f%s", mats, step, root[0], root[1], root[2],
                 pose->t >= 0 && Weight(pose->t) > 0 ? " (falling)" : "");
        }
    }
    pose->fresh.assign(now_mats, now_mats + floats);
    if (pose->t >= 0) {
        const float w = Weight(pose->t);
        if (w <= 0) {
            pose->t = -1;
        } else {
            for (size_t i = 0; i < floats; i++) now_mats[i] += pose->gap[i] * w;
        }
    }
    pose->shown.assign(now_mats, now_mats + floats);
}
