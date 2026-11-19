// The medgun's beam. A match's Long Ranger heals teammates with the medgun, and the console draws nothing for it: the
// fire state (0x323E70) plays medigun_active (sound 0xC0F) and a voice, and the heal tick (0x323BC0, every
// [weapon table +0x20] seconds of [player+0x568]+0x604) picks the nearest hurt teammate in a cone ahead and hits it by
// 0x323BA0: 0x2F0470 (eax the target, the healer pushed) with damage type 0x64, which reaches the hit function
// 0x2EDDD0 as its fourth argument. Trace (2026-11-17): a type 0x64 hit on the target after each tick, about 1.03 s apart.
//
// Who sees it. The heal tick returns at once for a remote player (controller +0xBC == 2), and the heal's hit reaches no
// other machine but the target's (read from the code: 0x2EDDD0 hands 0x281B80 a flag that sends the HitMsg to the
// victim's machine alone for this type), so the first build drew the beam only on the healer's machine (user: no one
// else saw it). Here the tick runs for a remote healer too (sub_00323BC0): its own
// timer at controller +0x604 and its own choice of target (the nearest hurt teammate in a cone ahead, 0x3252E0), the
// controller's +0xBC let to 0 for the call, the heal's hit (0x323BA0's 0x2F0470) and its score event (0x242A80, type
// 0x1D) skipped: no machine but the healer's heals. A healer's heal from 0x2EDDD0 (friendly_fire.cpp, the healer the
// record that dealt it followed to its owner; its fifth argument is the victim's own index + 1) is taken only when this
// machine has seen no tick of that healer lately (HealHit). A copy's +0x3AA (the medgun's overheal) is never sent, so a
// remote tick takes a teammate at full health for hurt, as the healer's own machine does.
//
// A heal holds the beam up a little longer than the gap between this healer's last two heals (the tick's period,
// whatever the class's table says), then fades it. It is drawn into each view after the view's world effects
// (0x0D9950, category 0: the effect types 1..0x57), at 0x2A52FA, edi the view: the game's own beam effect's state
// (CHAREFFECTS_TYPE_BEAM, 0x51: its setup 0x1AA620 sets the world space vertex shader [0x6B826C], the view's matrices
// from [0x84A13C] + view * 0x2B0 + 0x100 and + 0xC0, no texture, the diffuse colour, depth tested LEQUAL, depth writes
// on, no culling), blended additively (the destination blend ONE for the draw, 0x303 again after, as the setup leaves
// it) with depth writes off for the draw (on again after): written, the player outlines took the beam for a solid thing
// in front and were cut where it crossed them (user, a system link match, where the beam itself didn't show);
// drawn by D3DDevice_DrawVerticesUP, all of a view's beams in one draw.
//
// The look (DrawBeam): a curve from the gun to the target through two bends, a third and two thirds along, each where
// the line was a moment ago (kLag) and swung past that (kWhip), so it whips as either player moves; raised (kArc) and
// swaying sideways out of step with each other (kSway), so it snakes in an S at rest; a sideways wave runs along it. Along
// it, ribbons facing the view's eye (from the view matrix at +0x100), fading to nothing at their edges: a wide soft glow
// that breathes, a thin bright core with pulses running to the target, and two thin strands twisting about the core,
// brighter on the near side. Sparkles drift down it; a glow sits at the gun and on the target. A new beam reaches out
// from the gun (kReach); each heal sends a bright pulse down it (kTravel) that flashes on the target as it arrives.
//
// The beam starts at the healer's position (+0x78, a record of [0x84A5F8]) raised and moved ahead toward the target
// and to the right, and ends on the target raised (kStartUp.., tuned by eye in a match). XBE_MEDBEAM_TRACE=1 logs each
// heal and, about once a second, a beam's ends and the eye.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include "common/medbeam.h"
#include "common/warp.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kRecords = 0x84A5F8, kRecordSize = 0x6D0, kRecordCount = 0x80;
constexpr uint32_t kKind = 0x64, kKindPlayer = 1;
constexpr uint32_t kPosition = 0x78;  // x y z
constexpr uint32_t kHealth = 0x3AE;   // signed word, 0 dead
constexpr uint32_t kMatchFlag = 0x7819D5;
constexpr uint32_t kViews = 0x84A13C, kViewSize = 0x2B0, kViewMatrix = 0x100;  // rows: out = row . (p, 1)
constexpr uint32_t kBeamSetup = 0x1AA620, kBeamType = 0x51;
constexpr uint32_t kRenderState = 0x537990;  // ecx the method (count 1 << 18), edx the value
constexpr uint32_t kDestBlendMethod = 0x40348, kDestBlendShadow = 0x549AF4, kDestBlendCache = 0x771808;
// ZWRITEENABLE: NV097_SET_DEPTH_MASK, render state 64; the beam's setup turns it on
constexpr uint32_t kDepthMaskMethod = 0x4035C, kDepthMaskShadow = 0x549AF8, kDepthMaskCache = 0x771814;
constexpr uint32_t kNetworked = 0x863D11;  // byte: a system link session (0x281B00's test)
constexpr uint32_t kController = 0x568, kControl = 0xBC;  // controller +0xBC: 2 a remote player
constexpr uint32_t kDrawVerticesUP = 0x53A600;
constexpr uint32_t kTriangleList = 5;

struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }

float GetF(uint32_t va) {
    float f;
    std::memcpy(&f, mem::Base() + va, 4);
    return f;
}
Vec GetVec(uint32_t va) { return {GetF(va), GetF(va + 4), GetF(va + 8)}; }

// where the beam goes, world units: its start raised from the healer's position and moved toward the target and to
// the right, its end raised from the target's; the glow's and the core's half widths
constexpr float kStartUp = 42, kStartAhead = 14, kStartRight = 8, kEndUp = 34;
constexpr float kGlowWidth = 8, kCoreWidth = 2.2f;

bool Trace() {
    static const bool on = std::getenv("XBE_MEDBEAM_TRACE") != nullptr;
    return on;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Beam {
    uint32_t healer = 0, target = 0;
    double last = 0;   // the last heal, seconds: its pulse leaves the gun then
    double start = 0;  // when this healer began on this target: the beam reaches out from then
    float hold = 0;    // seconds it stays after a heal
    Vec s_lag{}, e_lag{};  // the ends, trailing the real ones: the curve's middle follows them, so it bends
    double lag_at = 0;     // when the trailing ends were last moved, 0 not yet
};
constexpr float kFirstHold = 0.7f, kMinHold = 0.25f, kMaxHold = 1.5f, kFade = 0.15f;
Beam g_beams[16];
double g_traced = 0;
double g_ticked[kRecordCount];  // when each record's heal tick last healed (Heal), seconds
bool g_remote_tick = false;     // in a remote healer's heal tick (sub_00323BC0)

// a player record of the table, alive
bool LivePlayer(uint32_t rec) {
    const uint32_t table = kernel::R32(kRecords);
    if (!table || rec < table || (rec - table) % kRecordSize || (rec - table) / kRecordSize >= kRecordCount) return false;
    return kernel::R32(rec + kKind) == kKindPlayer && kernel::G<int16_t>(rec + kHealth) > 0;
}

uint32_t Index(uint32_t rec) { return (rec - kernel::R32(kRecords)) / kRecordSize; }

// the eye of a view from its matrix at [0x84A13C] + view * 0x2B0 + 0x100 (rows: out = row . (p, 1)). Here it is the
// view matrix (rotation and translation, its last row 0 0 0 1; trace 2026-11-17), whose eye is -R^T t; were it a
// projection, the eye is the point its x, y and w rows send to 0
bool Eye(uint32_t view, Vec& eye) {
    const uint32_t views = kernel::R32(kViews);
    if (!views) return false;
    const uint32_t m = views + view * kViewSize + kViewMatrix;
    float r[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i][j] = GetF(m + uint32_t(i) * 16 + uint32_t(j) * 4);
    if (r[3][0] == 0 && r[3][1] == 0 && r[3][2] == 0 && r[3][3] == 1) {
        const Vec t{r[0][3], r[1][3], r[2][3]};
        eye = {-(r[0][0] * t.x + r[1][0] * t.y + r[2][0] * t.z), -(r[0][1] * t.x + r[1][1] * t.y + r[2][1] * t.z),
               -(r[0][2] * t.x + r[1][2] * t.y + r[2][2] * t.z)};
    } else {
        const int rows[3] = {0, 1, 3};
        const Vec a{r[rows[0]][0], r[rows[1]][0], r[rows[2]][0]}, b{r[rows[0]][1], r[rows[1]][1], r[rows[2]][1]};
        const Vec c{r[rows[0]][2], r[rows[1]][2], r[rows[2]][2]}, d{-r[rows[0]][3], -r[rows[1]][3], -r[rows[2]][3]};
        const float det = Dot(a, Cross(b, c));
        if (!(std::fabs(det) > 1e-12f)) return false;
        eye = {Dot(d, Cross(b, c)) / det, Dot(a, Cross(d, c)) / det, Dot(a, Cross(b, d)) / det};
    }
    return std::isfinite(eye.x) && std::isfinite(eye.y) && std::isfinite(eye.z);
}

struct Vertex {
    float x, y, z;
    uint32_t colour;  // D3DCOLOR
    float u, v;
};
static_assert(sizeof(Vertex) == 24);

uint32_t Colour(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

float Smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

Vec Normal(Vec a, Vec fallback) {
    const float l = Length(a);
    return l > 1e-5f ? a * (1 / l) : fallback;
}

// a point of a strip: where, its half width, its colour along the middle (the edges are the same colour, clear)
struct StripPoint {
    Vec at;
    float w;
    float r, g, b, a;
};

// a strip through the points as triangles: a ribbon facing the eye, fading to nothing at its edges
void Strip(std::vector<Vertex>& out, const std::vector<StripPoint>& pts, Vec eye) {
    const size_t n = pts.size();
    if (n < 2) return;
    auto side = [&](size_t i) {
        const Vec tan = pts[std::min(i + 1, n - 1)].at - pts[i ? i - 1 : 0].at;
        const Vec s = Normal(Cross(tan, eye - pts[i].at), Normal(Cross(tan, Vec{0, 1, 0}), Vec{1, 0, 0}));
        return s * pts[i].w;
    };
    auto put = [&](Vec at, const StripPoint& p, float a, float v) {
        out.push_back({at.x, at.y, at.z, Colour(p.r, p.g, p.b, a), 0.5f, v});
    };
    Vec s0 = side(0);
    for (size_t i = 0; i + 1 < n; i++) {
        const StripPoint &p0 = pts[i], &p1 = pts[i + 1];
        const Vec s1 = side(i + 1);
        const Vec l0 = p0.at - s0, r0 = p0.at + s0, l1 = p1.at - s1, r1 = p1.at + s1;
        put(l0, p0, 0, 0), put(p0.at, p0, p0.a, 0), put(p1.at, p1, p1.a, 1);
        put(l0, p0, 0, 0), put(p1.at, p1, p1.a, 1), put(l1, p1, 0, 1);
        put(p0.at, p0, p0.a, 0), put(r0, p0, 0, 0), put(r1, p1, 0, 1);
        put(p0.at, p0, p0.a, 0), put(r1, p1, 0, 1), put(p1.at, p1, p1.a, 1);
        s0 = s1;
    }
}

// a soft disc facing the eye: its colour at the middle, clear at the rim
void Disc(std::vector<Vertex>& out, Vec c, Vec eye, float radius, float r, float g, float b, float a) {
    if (radius <= 0 || a <= 0.002f) return;
    const Vec look = Normal(eye - c, Vec{0, 0, 1});
    const Vec right = Normal(Cross(look, Vec{0, 1, 0}), Vec{1, 0, 0});
    const Vec up = Cross(right, look);
    constexpr int kRim = 10;
    const uint32_t mid = Colour(r, g, b, a), rim = Colour(r, g, b, 0);
    for (int k = 0; k < kRim; k++) {
        const float a0 = 6.2831853f * float(k) / kRim, a1 = 6.2831853f * float(k + 1) / kRim;
        const Vec p0 = c + (right * std::cos(a0) + up * std::sin(a0)) * radius;
        const Vec p1 = c + (right * std::cos(a1) + up * std::sin(a1)) * radius;
        out.push_back({c.x, c.y, c.z, mid, 0.5f, 0.5f});
        out.push_back({p0.x, p0.y, p0.z, rim, 0.5f, 0.5f});
        out.push_back({p1.x, p1.y, p1.z, rim, 0.5f, 0.5f});
    }
}

float Hash(uint32_t n) {
    n = (n << 13) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7FFFFFFF) / float(0x7FFFFFFF);
}

constexpr float kTau = 6.2831853f, kPi = 3.14159265f;
constexpr float kLag = 0.32f;       // seconds the curve's bends trail the ends by
constexpr float kWhip = 1.8f;       // how far past that trail the bends swing
constexpr float kArc = 0.1f;        // its rise, of the beam's length
constexpr float kSway = 0.16f, kSwayRate = 0.21f;  // the bends' slow sideways sway (of the length, up to kSwayMax) and turns a second
constexpr float kSwayMax = 30;
constexpr float kReach = 0.18f;     // seconds a new beam takes to reach its target
constexpr float kTravel = 0.22f;    // seconds a heal's pulse takes down the beam
constexpr float kFlash = 0.35f;     // seconds the flash on the target lasts
constexpr float kTwist = 38;        // units along the beam per turn of the strands
constexpr float kSpin = 5;          // the strands' turn, radians a second
constexpr float kWobble = 70, kWobbleRate = 1.1f;  // the sideways wave's length (units) and passes a second
constexpr float kWave = 3.5f;       // the sideways wave's reach, units
constexpr int kSparkles = 10;

// one beam from s to e into out (fade its strength, 0..1)
void DrawBeam(std::vector<Vertex>& out, Beam& b, uint32_t id, Vec s, Vec e, Vec eye, float fade, double now) {
    const float t = float(std::fmod(now, 1000.0));
    // the trailing ends, moved once a frame (a second view in the same frame finds dt 0)
    if (!b.lag_at || Length(b.s_lag - s) > 400 || Length(b.e_lag - e) > 400) {
        b.s_lag = s, b.e_lag = e;
    } else {
        const float k = 1 - std::exp(-float(now - b.lag_at) / kLag);
        b.s_lag = b.s_lag + (s - b.s_lag) * k;
        b.e_lag = b.e_lag + (e - b.e_lag) * k;
    }
    b.lag_at = now;
    const float len = Length(e - s);
    if (len < 2) return;
    // two bends, a third and two thirds along: where the ends were a moment ago (overshot by kWhip), raised, swaying
    // sideways out of step with each other, so the beam snakes in an S even when no one moves
    const Vec side = Normal(Cross(e - s, Vec{0, 1, 0}), Vec{1, 0, 0});
    const float sway = std::min(kSway * len, kSwayMax);
    const float phase = kTau * kSwayRate * t + kTau * Hash(id * 31u);
    auto bend = [&](float f, float lag_phase) {
        const Vec real = s + (e - s) * f, trailed = b.s_lag + (b.e_lag - b.s_lag) * f;
        return real + (trailed - real) * kWhip + Vec{0, kArc * len, 0} + side * (sway * std::sin(phase + lag_phase));
    };
    const Vec c1 = bend(1.0f / 3, 0), c2 = bend(2.0f / 3, 2.3f);
    const float reach = Smooth(0, 1, float(now - b.start) / kReach);
    const float bolus = float(now - b.last) / kTravel;  // the heal's pulse: where along the beam, past 1 arrived
    const int n = std::clamp(int(len * reach / 6), 6, 48);

    // the curve: through s and e, bent toward the trailing middle, a sideways wave along it
    struct Point {
        Vec at, n1, n2, eyeward;
        float f, ends;
    };
    std::vector<Point> path(size_t(n) + 1);
    for (int i = 0; i <= n; i++) {
        const float f = reach * float(i) / float(n), g = 1 - f;
        Point& p = path[size_t(i)];
        p.f = f;
        const Vec tan = Normal((c1 - s) * (3 * g * g) + (c2 - c1) * (6 * g * f) + (e - c2) * (3 * f * f),
                               Normal(e - s, Vec{0, 0, 1}));
        p.n1 = Normal(Cross(tan, Vec{0, 1, 0}), Vec{1, 0, 0});
        p.n2 = Cross(p.n1, tan);
        const float env = std::sin(kPi * f);
        const float wave = std::sin(kTau * (f * len / kWobble - t * kWobbleRate));
        const float wave2 = std::cos(kTau * (f * len / (kWobble * 0.63f) - t * kWobbleRate * 1.37f));
        const float amp = kWave * env;
        p.at = s * (g * g * g) + c1 * (3 * g * g * f) + c2 * (3 * g * f * f) + e * (f * f * f) + p.n1 * (amp * wave) +
               p.n2 * (amp * 0.6f * wave2);
        p.eyeward = Normal(eye - p.at, Vec{0, 0, 1});
        // soft at the gun, soft where it reaches (the target once it has)
        const float tip = reach < 1 ? std::max(0.12f, 1 - reach) : std::min(0.05f, 6 / len);
        p.ends = Smooth(0, std::min(0.08f, 10 / len), f) * Smooth(reach, reach - tip, f);
    }
    auto pulse_at = [&](float f) {
        if (bolus > 1.15f) return 0.0f;
        const float d = (f - bolus) / 0.07f;
        return std::exp(-d * d);
    };

    std::vector<StripPoint> pts;
    pts.reserve(path.size());
    // the glow
    for (const Point& p : path) {
        const float breathe = 0.85f + 0.15f * std::sin(kTau * (p.f * len / 55 - t * 1.6f));
        pts.push_back({p.at, kGlowWidth * breathe * (1 + 0.6f * pulse_at(p.f)), 0.25f, 1.0f, 0.45f,
                       fade * p.ends * (0.42f + 0.5f * pulse_at(p.f))});
    }
    Strip(out, pts, eye);
    // the core, its pulses running to the target
    pts.clear();
    for (const Point& p : path) {
        const float wave = 0.5f + 0.5f * std::sin(kTau * (p.f * len / 40 - t * 2.5f));
        pts.push_back({p.at, kCoreWidth * (1 + 0.8f * pulse_at(p.f)), 0.8f, 1.0f, 0.85f,
                       fade * p.ends * (0.6f + 0.35f * wave + 0.6f * pulse_at(p.f))});
    }
    Strip(out, pts, eye);
    // two strands twisting about the core, brighter on the near side
    for (int k = 0; k < 2; k++) {
        pts.clear();
        for (const Point& p : path) {
            const float theta = kTau * p.f * len / kTwist - t * kSpin + kPi * float(k);
            const Vec dir = p.n1 * std::cos(theta) + p.n2 * std::sin(theta);
            const float radius = kGlowWidth * 0.6f * std::sqrt(std::max(0.0f, std::sin(kPi * std::min(p.f, 1.0f))));
            const float near = 0.5f + 0.5f * Dot(dir, p.eyeward);
            pts.push_back({p.at + dir * radius, kCoreWidth * 0.55f, 0.55f, 1.0f, 0.7f,
                           fade * p.ends * (0.25f + 0.6f * near)});
        }
        Strip(out, pts, eye);
    }
    // sparkles drifting to the target
    auto along = [&](float f) {
        const float x = std::clamp(f / std::max(reach, 1e-3f), 0.0f, 1.0f) * float(n);
        const int i = std::min(int(x), n - 1);
        const float w = x - float(i);
        const Point &a = path[size_t(i)], &c = path[size_t(i) + 1];
        return std::pair<Vec, const Point*>(a.at + (c.at - a.at) * w, &a);
    };
    for (int k = 0; k < kSparkles; k++) {
        const uint32_t h = id * 97u + uint32_t(k) * 13u;
        const float speed = 0.55f + 0.5f * Hash(h);
        const float f = std::fmod(t * speed + Hash(h + 1), 1.0f);
        if (f > reach) continue;
        const auto [at, p] = along(f);
        const float spin = kTau * Hash(h + 2) + t * (2 + 3 * Hash(h + 3));
        const Vec off = (p->n1 * std::cos(spin) + p->n2 * std::sin(spin)) * (kGlowWidth * (0.3f + 0.9f * Hash(h + 4)));
        const float twinkle = 0.55f + 0.45f * std::sin(t * (9 + 6 * Hash(h + 5)) + 20 * Hash(h + 6));
        Disc(out, at + off, eye, kCoreWidth * (1.2f + 0.8f * Hash(h + 7)), 0.85f, 1.0f, 0.9f,
             fade * std::sin(kPi * f) * twinkle);
    }
    // a glow at the gun, and on the target once the beam is there: a flash as each heal's pulse arrives
    Disc(out, s, eye, kGlowWidth * (1.1f + 0.15f * std::sin(t * 13)), 0.35f, 1.0f, 0.55f, fade * 0.55f);
    if (reach >= 1) {
        Disc(out, e, eye, kGlowWidth * 1.3f, 0.3f, 1.0f, 0.5f, fade * 0.3f);
        const float flash = (float(now - b.last) - kTravel) / kFlash;
        if (flash >= 0 && flash < 1)
            Disc(out, e, eye, kGlowWidth * (1.4f + 2.6f * flash), 0.6f, 1.0f, 0.7f, fade * 0.8f * (1 - flash) * (1 - flash));
    }
}

// a render state through the library's method writer, its shadow and the effects' cache kept with it
void SetState(X86Context& ctx, uint32_t method, uint32_t shadow, uint32_t cache, uint32_t value) {
    ctx.ecx = method;
    ctx.edx = value;
    kernel::CallGuest(ctx, kRenderState, {});
    kernel::W32(shadow, value);
    kernel::W32(cache, value);
}

// the draw's vertices in guest memory, room for every beam: 16 beams of 4 strips of up to 48 segments (12 vertices
// each), 10 sparkles and 3 discs (30 vertices each)
constexpr uint32_t kCapacity = 16 * (4 * 48 * 12 + 13 * 30) * sizeof(Vertex);
uint32_t g_vertices = 0;

}  // namespace

bool medbeam::g_enabled = true;

// the heal tick (0x323BC0, eax the player, from the fire state 0x323E70): run for a remote healer too, its heal and
// score event skipped, so every machine sees whom it heals
X86_EXTERN_FUNC(__imp__sub_00323BC0);
X86_FUNC(sub_00323BC0) {
    const uint32_t player = ctx.eax;
    const uint32_t control = player ? kernel::R32(player + kController) : 0;
    if (!medbeam::g_enabled || !control || kernel::R32(control + kControl) != 2 || !kernel::G<uint8_t>(kNetworked) ||
        !kernel::G<uint8_t>(kMatchFlag) || !warp::Multiplayer() || g_remote_tick)
        return __imp__sub_00323BC0(ctx, base);
    if (Trace()) {
        static double traced = 0;
        if (Now() - traced > 1) traced = Now(), LOGR("medbeam: heal tick of remote player %u", Index(player));
    }
    g_remote_tick = true;
    kernel::W32(control + kControl, 0);
    __imp__sub_00323BC0(ctx, base);
    kernel::W32(control + kControl, 2);
    g_remote_tick = false;
}

// the heal tick's hit (0x323BA0: eax the healer, [esp+4] the target); in a remote healer's tick only the beam's
X86_EXTERN_FUNC(__imp__sub_00323BA0);
X86_FUNC(sub_00323BA0) {
    const uint32_t healer = ctx.eax, target = kernel::R32(ctx.esp + 4);
    if (g_remote_tick) {
        medbeam::Heal(healer, target);
        return kernel::Return(ctx, ctx.eax, 0);
    }
    __imp__sub_00323BA0(ctx, base);
    if (medbeam::g_enabled && kernel::G<uint8_t>(kMatchFlag) && warp::Multiplayer()) medbeam::Heal(healer, target);
}

// a match event (0x242A80, cdecl: the player's index, the type, ...): a remote heal tick's (type 0x1D, the heal's
// score) is the healer's machine's to count
X86_EXTERN_FUNC(__imp__sub_00242A80);
X86_FUNC(sub_00242A80) {
    if (g_remote_tick) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_00242A80(ctx, base);
}

namespace {

// a heal: the beam from healer to target held up, a pulse sent down it
void Start(uint32_t healer, uint32_t target, const char* from) {
    const double now = Now();
    Beam* slot = nullptr;
    for (Beam& b : g_beams)
        if (b.healer == healer) slot = &b;
    if (!slot) {
        slot = &g_beams[0];
        for (Beam& b : g_beams)
            if (b.last + b.hold < slot->last + slot->hold) slot = &b;
        *slot = {};
    }
    const double gap = now - slot->last;
    const bool held = slot->healer == healer && slot->target == target && gap < slot->hold;
    if (slot->healer == healer && slot->target == target && gap < kMaxHold)
        slot->hold = std::clamp(float(gap) * 1.35f + 0.05f, kMinHold, kMaxHold);
    else
        slot->hold = kFirstHold;
    if (!held) slot->start = now, slot->lag_at = 0;
    slot->healer = healer;
    slot->target = target;
    slot->last = now;
    if (Trace())
        LOGR("medbeam: player %u heals player %u (gap %.3f s, held %.2f s), %s", Index(healer), Index(target),
             gap < 1e6 ? gap : -1.0, slot->hold, from);
}

}  // namespace

void medbeam::Heal(uint32_t healer, uint32_t target) {
    if (!LivePlayer(healer) || !target || healer == target) return;
    g_ticked[Index(healer)] = Now();
    Start(healer, target, g_remote_tick ? "a remote healer's tick" : "the tick");
}

void medbeam::HealHit(uint32_t healer, uint32_t target) {
    if (!g_enabled || !LivePlayer(healer) || !target || healer == target || Now() - g_ticked[Index(healer)] < 2.5) return;
    Start(healer, target, "a hit");
}

// [[hook]] at 0x2A52FA, a view's world effects drawn (0x0D9950, category 0; [esp+4] the view): the medgun's beams
void ConkerMedBeams(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!medbeam::g_enabled) return;
    const double now = Now();
    bool any = false;
    for (const Beam& b : g_beams) any |= b.healer && now - b.last < b.hold;
    if (!any) return;
    const uint32_t view = kernel::R32(ctx.esp + 4) & 0xFFFF;
    Vec eye;
    if (!kernel::G<uint8_t>(kMatchFlag) || !warp::Multiplayer() || view >= 4 || !Eye(view, eye)) return;

    static std::vector<Vertex> verts;
    verts.clear();
    const bool trace = Trace() && now - g_traced > 1;
    for (uint32_t id = 0; id < std::size(g_beams); id++) {
        Beam& b = g_beams[id];
        if (!b.healer) continue;
        const float age = float(now - b.last);
        if (age >= b.hold || !LivePlayer(b.healer) || !LivePlayer(b.target)) continue;
        const float fade = std::clamp((b.hold - age) / kFade, 0.0f, 1.0f);
        const Vec from = GetVec(b.healer + kPosition), to = GetVec(b.target + kPosition);
        const Vec ahead = Normal(Vec{to.x - from.x, 0, to.z - from.z}, Vec{0, 0, 1});
        const Vec right{ahead.z, 0, -ahead.x};
        const Vec s = from + Vec{0, kStartUp, 0} + ahead * kStartAhead + right * kStartRight;
        const Vec e = to + Vec{0, kEndUp, 0};
        DrawBeam(verts, b, id, s, e, eye, fade, now);
        if (trace) {
            g_traced = now;
            LOGR("medbeam: view %u eye %.1f %.1f %.1f, player %u at %.1f %.1f %.1f -> player %u at %.1f %.1f %.1f", view,
                 eye.x, eye.y, eye.z, Index(b.healer), from.x, from.y, from.z, Index(b.target), to.x, to.y, to.z);
        }
    }
    if (verts.empty()) return;
    if (trace) LOGR("medbeam: drawing %zu vertices in view %u", verts.size(), view);

    if (!g_vertices) g_vertices = mem::AllocSystem(kCapacity);
    if (!g_vertices) return;
    if (verts.size() > kCapacity / sizeof(Vertex)) verts.resize(kCapacity / sizeof(Vertex) / 3 * 3);
    const uint32_t bytes = uint32_t(verts.size() * sizeof(Vertex));
    std::memcpy(mem::Base() + g_vertices, verts.data(), bytes);
    const X86Context saved = ctx;
    kernel::CallGuest(ctx, kBeamSetup, {kBeamType, view});
    SetState(ctx, kDestBlendMethod, kDestBlendShadow, kDestBlendCache, 1);  // ONE: the glow adds
    SetState(ctx, kDepthMaskMethod, kDepthMaskShadow, kDepthMaskCache, 0);  // no depth writes
    kernel::CallGuest(ctx, kDrawVerticesUP, {kTriangleList, uint32_t(verts.size()), g_vertices, uint32_t(sizeof(Vertex))});
    SetState(ctx, kDepthMaskMethod, kDepthMaskShadow, kDepthMaskCache, 1);      // as the setup leaves them
    SetState(ctx, kDestBlendMethod, kDestBlendShadow, kDestBlendCache, 0x303);
    ctx = saved;
}
