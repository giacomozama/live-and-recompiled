// Beach Bloodbath's "Bunker Padlock" cutscene (level 4, cutscene 1, checkpoint 139): the CombatSquirrel kneeling by
// the padlock (model type 88; the shot one lying beside him is 321) has his tail keyed erratically: each rigid piece
// rolls about the tail's length against the next, up to some 175 degrees and jumping by more than 150 between keys,
// which wrings the tail into a pinch where it meets his back and makes it thrash, into the wall behind him at times.
// The original does the same. Here his tail is held in the model's own shape on his pelvis for the whole cutscene,
// with a slow idle sway of its own: side to side (along the wall) in a wave that lags joint by joint and grows to
// the tip, and a slower small bob, timed by the cutscene's clock so it pauses with the game.
//
// A skinned model's bones are a matrix each in the instance's data, [data + node[0xC] + 4], 64 bytes, row vectors
// (x * M, translation in row 3), from model space (positions times 1/128, c[178]) to the world: in the model's rest
// pose every bone's matrix is the same. The skeleton node (0x3EC7D0, a node renderer: node, data) hands them to the
// palette nodes under it (0x3EF6C0 and its siblings, through 0x3EF650 / 0x3EF5B0), which multiply them by the view,
// so they are changed in place when the skeleton runs, before any of its pieces draw. The tail is the fur piece that
// uploads palette entries 27..31 (bones 6, 12, 30, 37, 18), each vertex of it weighted to one bone alone; the chain,
// from the joint each turns about on its parent, is 2 (the pelvis) -> 6 (a ring at the root) -> 12 (where the tail
// meets the back) -> 18 -> 30 -> 37 (the tip). Giving the tail's bones the pelvis's matrix draws it as modelled,
// curled up behind his back, moving with his body; the sway turns each piece about its joint (model space, the
// joints fitted from the cutscene's own motion), each child after its parent.
//
// Who is who: a player's draw (0x7FB00, cdecl, arg 0 its index in [0x84A5F8], 0x6D0 each) keeps its model's
// skeleton node at [player+0x4EC] and its type at [player+0x68].
#include "cpu/x86_ops.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common/crate_fall.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kPlayers = 0x84A5F8, kPlayerSize = 0x6D0;
constexpr uint32_t kLevel = 0x7FA1F8;
constexpr uint32_t kCutsceneIndex = 0x76F0DC, kCutsceneState = 0x76F0E0, kCutsceneSlots = 4;
constexpr uint32_t kSquirrelType = 88, kBeachRunup = 4, kPadlockCutscene = 1;

constexpr uint32_t kPelvis = 2;
constexpr int kPieces = 5;
constexpr uint32_t kTailBones[kPieces] = {6, 12, 18, 30, 37};
// the joint each piece turns about on its parent, and the tip piece's middle, in model space (the units the bone
// matrices take: positions times 1/128); the tail lies in the y-z plane, x across his body
constexpr float kJoints[kPieces + 1][3] = {{0.0f, 52.9f, -15.9f},  {-0.6f, 52.0f, -36.3f}, {0.4f, 77.6f, -61.0f},
                                           {0.0f, 115.5f, -65.3f}, {-0.7f, 141.9f, -72.1f}, {3.9f, 154.9f, -86.2f}};
// the sway at each joint, degrees: side to side, up and down
constexpr float kSide[kPieces] = {2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
constexpr float kBob[kPieces] = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
constexpr float kSidePeriod = 2.8f, kBobPeriod = 4.3f;  // seconds
constexpr float kLag = 0.55f;                            // radians of the wave, joint to joint
constexpr uint32_t kCutsceneNow = 0x76F0F8, kGameFps = 0x7F9F5C;  // the cutscene's frame per slot, its frames a second

uint32_t g_node = 0;  // the kneeling squirrel's skeleton node, from his last draw

// XBE_TAIL_TRACE=1: a line a second while the squirrel draws, saying whether his tail was held
bool Tracing() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_TAIL_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}
int g_held = 0;  // skeleton runs with the tail held since the last line

// the slot playing the padlock cutscene, or -1
int PadlockSlot() {
    if (kernel::R32(kLevel) != kBeachRunup) return -1;
    for (uint32_t i = 0; i < kCutsceneSlots; i++)
        if (kernel::g_base[kCutsceneState + i] && kernel::g_base[kCutsceneIndex + i] == kPadlockCutscene) return int(i);
    return -1;
}
bool PadlockCutscene() { return PadlockSlot() >= 0; }

// seconds into the cutscene
float CutsceneSeconds(int slot) {
    float fps;
    std::memcpy(&fps, kernel::g_base + kGameFps, 4);
    if (!std::isfinite(fps) || fps < 1.0f) fps = 30.0f;
    return float(int32_t(kernel::R32(kCutsceneNow + uint32_t(slot) * 4))) / fps;
}

struct M4 {
    float m[4][4];
};

M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// a turn by angle (radians) about unit axis a through point j, as a row vector matrix: x' = (x - j) R + j
M4 TurnAbout(const float a[3], float angle, const float j[3]) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float x = a[0], y = a[1], z = a[2];
    // the column form rotation, transposed into the row form
    const float col[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                             {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                             {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    M4 r{};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) r.m[i][k] = col[k][i];
    for (int k = 0; k < 3; k++) r.m[3][k] = j[k] - (j[0] * r.m[0][k] + j[1] * r.m[1][k] + j[2] * r.m[2][k]);
    r.m[3][3] = 1.0f;
    return r;
}

void HoldTail(uint32_t mats, float seconds) {
    M4 parent;
    std::memcpy(parent.m, kernel::g_base + mats + kPelvis * 64, sizeof(parent.m));
    for (auto& row : parent.m)
        for (float v : row)
            if (!std::isfinite(v)) return;
    constexpr float kTau = 6.2831853f, kDeg = 0.017453293f;
    for (int k = 0; k < kPieces; k++) {
        // the piece's direction, down the tail (in the y-z plane); the side sway turns about the axis across it in
        // that plane, the bob about x
        float d[3] = {0.0f, kJoints[k + 1][1] - kJoints[k][1], kJoints[k + 1][2] - kJoints[k][2]};
        float n = std::sqrt(d[1] * d[1] + d[2] * d[2]);
        const float across[3] = {0.0f, -d[2] / n, d[1] / n};
        const float x[3] = {1.0f, 0.0f, 0.0f};
        const float side = kSide[k] * kDeg * std::sin(kTau * seconds / kSidePeriod - kLag * float(k));
        const float bob = kBob[k] * kDeg * std::sin(kTau * seconds / kBobPeriod - kLag * float(k) + 1.3f);
        M4 turn = Mul(TurnAbout(across, side, kJoints[k]), TurnAbout(x, bob, kJoints[k]));
        parent = Mul(turn, parent);  // x * piece = (x * turn) * parent
        std::memcpy(kernel::g_base + mats + kTailBones[k] * 64, parent.m, sizeof(parent.m));
    }
    g_held++;
}

}  // namespace

// 0x7FB00: a player's draw (arg 0 its index): notes the kneeling squirrel's skeleton node, and a crate's (crate_fall.cpp)
X86_EXTERN_FUNC(__imp__sub_0007FB00);
X86_FUNC(sub_0007FB00) {
    const uint32_t player = kernel::R32(kPlayers) + kernel::R32(ctx.esp + 4) * kPlayerSize;
    crate_fall::NoteDraw(player);
    if (kernel::R32(player + 0x68) == kSquirrelType) {
        g_node = kernel::R32(player + 0x4EC);
        static std::chrono::steady_clock::time_point last;
        const auto now = std::chrono::steady_clock::now();
        if (Tracing() && now - last >= std::chrono::seconds(1)) {
            last = now;
            LOGR("tail: squirrel player %u node %08X level %u cutscene slots %u/%u %u/%u %u/%u %u/%u: %s, tail held "
                 "%d times, %.2f s in",
                 kernel::R32(ctx.esp + 4), g_node, kernel::R32(kLevel), kernel::g_base[kCutsceneIndex],
                 kernel::g_base[kCutsceneState], kernel::g_base[kCutsceneIndex + 1], kernel::g_base[kCutsceneState + 1],
                 kernel::g_base[kCutsceneIndex + 2], kernel::g_base[kCutsceneState + 2],
                 kernel::g_base[kCutsceneIndex + 3], kernel::g_base[kCutsceneState + 3],
                 PadlockCutscene() ? "padlock cutscene" : "not the padlock cutscene", g_held,
                 PadlockCutscene() ? CutsceneSeconds(PadlockSlot()) : 0.0f);
            g_held = 0;
        }
    }
    __imp__sub_0007FB00(ctx, base);
}

// 0x3EC7D0 (cdecl: node, data): a skeleton node hands its bone matrices to the palettes under it; a crate's slam eased
// first (crate_fall.cpp)
X86_EXTERN_FUNC(__imp__sub_003EC7D0);
X86_FUNC(sub_003EC7D0) {
    const uint32_t node = kernel::R32(ctx.esp + 4), data = kernel::R32(ctx.esp + 8);
    crate_fall::Skeleton(node, data);
    if (node && node == g_node) {
        if (int slot = PadlockSlot(); slot >= 0)
            HoldTail(kernel::R32(kernel::R32(node + 0xC) + data + 4), CutsceneSeconds(slot));
    }
    __imp__sub_003EC7D0(ctx, base);
}
