#include "common/water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/shaders.h"

bool water::g_dynamic = false;

namespace {

// The water program (07F95BC3079F60E6 for SHORT3 positions, BC39BFEF13558B0A for floats) scales v0 by c114.x into
// R3, the level's space with y up and about a centimetre to the unit, and takes it to view space by c96..c98 (R4) and
// to clip space by c99..c102. oT0 scrolls the swirl texture the pixel shader bumps by, oT1 is the clip position over
// w moved to 0..1 by c112.y, where it reads the copy of the screen through the water, oT2 a mask. Only these
// programs build oT1 that way. Registers in the HLSL are hardware order, D3D's + 96.
constexpr const char* kMatch = "(mac_mul(((R6).xyzw), ((c[112]).yyyy)) + ((c[112]).yyyy))";

// The game's ripples are rings drawn as flat quads on the water by a particle program (BAFEC7FDD6E09DA9), the only
// one of its kind that hands v4 on, bump mapping the copy of the screen at stage 1: v0 are the corners in the level's
// space, the water's. Conker landing and swimming spreads them around him, drops falling back small ones, in pairs a
// unit apart; each grows about its centre until it fades. A ring's first frame drops a dip into a sheet of water the
// wave equation runs on, around where the drops fall, and the waves spread from there and meet as water's do.
constexpr const char* kRingMatch = "((i.v4).xyyy)";
constexpr int kGrid = 256;            // cells a side
constexpr float kCell = 4;            // units a cell
constexpr float kExtent = kGrid * kCell;
constexpr float kStep = 1.0f / 60;    // seconds a step, at most kSteps a frame
constexpr int kSteps = 4;
constexpr float kSpeed2 = 0.06f;      // (speed * step / cell)^2, under 0.5 to stay stable: 0.24 cells a step, some 57 units a second
constexpr float kDamping = 0.975f;    // a step: a wave's height halves in some 0.45 seconds
constexpr float kDropDepth = 4;       // units
constexpr float kDropRadius = 6;
constexpr float kFollow = kExtent / 4;  // the sheet moves to a drop this far from its centre

// Characters are skinned by programs alike but for registers: v[p] (p 0 or 1) scaled by c[S].x, two bones at
// c[108 + v[p+1].x] and c[108 + v[p+1].y], three rows each to view space, weighed by v[p+2].x and .y; c96..c98 are the
// view, in the shadow pass too (its own). Points of a character's skin, spread over each draw, skinned as the program
// does and taken back to the level, press the water where they are near its surface: a footprint, each point a soft
// disc, and as it changes from a frame to the next the water is pressed by the change, so a moving body pushes a wake
// and one entering or leaving it a swell.
constexpr const char* kSkinMatch = "c[clamp(a0 + 108, 0, 191)]";
constexpr size_t kBodyPoints = 384;
constexpr float kBodyRadius = 12;     // units, a point's disc
constexpr float kBodyDepth = 3;       // units the water sinks under a full footprint
constexpr float kBodyAbove = 15;      // a point counts from this far above the surface
constexpr float kBodyBelow = 45;      // to this far below, fading over the last third

// The surface's height is the water's own: vertices spread over each of its draws, in the level's space, kept by
// square of kSurfaceCell; a body this square or one beside it is over water.
constexpr uint32_t kSurfaceSamples = 1024;
constexpr float kSurfaceCell = 32;
constexpr size_t kSurfaceCells = 1 << 18;  // past this many the map starts again

// The footprint knows how much of a body is in the water, not how fast it came: a dive would sink in no deeper than a
// wade. A body's points, gathered by nearness into one group per character and followed from frame to frame by it,
// that go down through the surface drop a dip as deep as they were fast, once.
constexpr float kBodySpan = 60;        // units in x and z: a point this near a group's middle is of its character
constexpr float kBodyFollow = 80;      // a group this near one of last frame's is the same character
constexpr float kEntrySpeed = 120;     // units a second down, the least that makes a splash
constexpr float kEntryFull = 500;      // the speed whose splash is kEntryDepth deep, at most 1.5 times that
constexpr float kEntryDepth = 14;
constexpr float kEntryRadius = 18;

// the sheet's height over xz, at least amp times it, faded out over its last tenth: xs[0] is its corner, 1 over its
// extent and 1 over its cells, xs[1].x 1 over two cells
constexpr const char* kField = R"(
#define FIELD(amp) { float2 fuv = (xz - xs[0].xy) * xs[0].z; if (all(fuv > 0) && all(fuv < 1)) { \
    float2 fe2 = saturate(min(fuv, 1 - fuv) * 10); float fe = fe2.x * fe2.y * (amp), d = xs[0].w; \
    float fc = xfield.SampleLevel(xfield_smp, fuv, 0); \
    float fx = xfield.SampleLevel(xfield_smp, fuv + float2(d, 0), 0) - xfield.SampleLevel(xfield_smp, fuv - float2(d, 0), 0); \
    float fz = xfield.SampleLevel(xfield_smp, fuv + float2(0, d), 0) - xfield.SampleLevel(xfield_smp, fuv - float2(0, d), 0); \
    h += fc * fe; dh += float2(fx, fz) * xs[1].x * fe; } }
)";

// mp, the level's position the sheet lifts by h, taken to the screen again as the program does; the copy of
// the screen is read where the lifted surface lies (the pixel shader bends it)
constexpr const char* kProject = R"(
        float4 m = float4(mp.x, mp.y + h, mp.z, 1);
        float4 v = float4(dot(m, c[96]), dot(m, c[97]), dot(m, c[98]), 1);
        float4 cp = float4(dot(v, c[99]), dot(v, c[100]), dot(v, c[101]), dot(v, c[102]));
        float w = magclamp(cp.w);
        float3 sp = cp.xyz * c[58].xyz / w + c[59].xyz;
        o.pos = float4((sp.x * 2 / surf.x - 1) * w, (1 - sp.y * 2 / surf.y) * w, sp.z * surf.z * w, w);
        o.tc1.xy = (cp.xy / w * float2(1, -1)) * c[112].y + c[112].y;)";

// The vertex shader leaves the river's own vertices, some 30 units apart, where they are: what the water shows where
// the GPU can't tessellate, too coarse for the ripples. It hands on the level's position in x0 for the domain shader,
// which lifts the tessellated surface by the ripples, a few units between vertices near the eye, and for the pixel
// shader.
constexpr const char* kVsHead = R"(    {
        float3 mp = R3.xyz;
        float h = 0;)";
constexpr const char* kVsTail = R"(
        o.x0 = float4(mp, 1);
        o.x1 = 0;
    })";
constexpr const char* kDsHead = R"(    {
        float3 mp = o.x0.xyz;
        float h = 0;
        float2 xz = mp.xz, dh = 0;)";

// Per pixel, the ripples' slope drives what the swirl drove on the console: the combiners read the copy of the screen
// (t1) bumped by the swirl (t0) as much as a tenth of the screen, which buries any movement of the surface under the
// riverbed's, and mix it with the water's colour by r0.a (stage 4, then times c10 of the final stage). Here it is read
// again where the swirl bent it (duv1 through bumpmat[1]), kSwirlKeep of that, and bent further by the surface's tilt in
// view space, and the difference put in its place, so the surface shows as it moves over the swirl's look; the slopes
// are shaded a little, lighter facing a light from +x+z; and the surface catches the sky at grazing angles (Schlick's
// Fresnel) and a glint of a high sun, seen from above or from under it, less what a flat surface would, which the
// console's colour already holds: still water is as bright as it was.
constexpr float kRefract = 0.15f;  // screen widths of bend per unit of slope
constexpr float kSwirlKeep = 1;  // the share of the console's swirl bend kept under the surface's
constexpr const char* kPsHead = R"({
    float h = 0;
    float2 xz = i.x0.xz, dh = 0;)";
constexpr const char* kPsTail = R"(
    FIELD(1)
    {
        float3 tilt = float3(-dh.x, 0, -dh.y);
        float2 bend = float2(dot(tilt, c[96].xyz), -dot(tilt, c[97].xyz)) * REFRACT;
        float2 swirl = float2(dot(bumpmat[1].xz, duv1), dot(bumpmat[1].yw, duv1)), at = tc1.xy + texsize[1].zw;
        float4 under = tex1.Sample(smp1, at + SWIRL_KEEP * swirl + bend), was = tex1.Sample(smp1, at + swirl);
        r0.rgb += (1 - saturate(r0.a)) * (under.rgb - was.rgb) * pc[10].rgb;
        r0.rgb *= 1 - clamp(0.8 * dot(dh, float2(0.6, 0.8)), -0.15, 0.15);
        float3 eye = -(c[96].xyz * c[96].w + c[97].xyz * c[97].w + c[98].xyz * c[98].w);
        float3 e = normalize(eye - float3(xz.x, i.x0.y + h, xz.y)), n = normalize(float3(-dh.x, 1, -dh.y));
        float3 flat = float3(0, 1, 0), sun = normalize(float3(0.4, 0.8, 0.45));
        if (e.y < 0) { n = -n; flat = -flat; }
        float fresnel = pow(1 - saturate(dot(n, e)), 5) - pow(1 - saturate(dot(flat, e)), 5);
        float glint = pow(saturate(dot(reflect(-e, n), sun)), 80) - pow(saturate(dot(reflect(-e, flat), sun)), 80);
        r0.rgb += (0.25 * fresnel + 0.4 * glint) * float3(0.9, 0.95, 1);
    }
})";

struct Drop {
    float x, y, z;
};
std::vector<Drop> g_seen, g_last;  // this frame's rings' centres, last frame's
std::unordered_map<uint64_t, float> g_surface;  // the water's height by square
float g_clock = 0;  // seconds of frames
std::vector<std::array<float, 3>> g_body;  // this frame's skin points

// the sheet: height now and a step before, its centre, and how many times it moved on
std::vector<float> g_now(kGrid * kGrid), g_before(kGrid * kGrid), g_next(kGrid * kGrid);
std::vector<float> g_foot(kGrid * kGrid), g_foot_next(kGrid * kGrid);  // the bodies' footprint, last frame's and now
float g_cx = 0, g_cz = 0;
bool g_placed = false;
uint64_t g_version = 1;
std::chrono::steady_clock::time_point g_then;
float g_owed = 0;  // seconds not yet stepped

// a flat quad with a bump mapped stage 1, by its corners
void Seen(const gpu::WatchedDraw& d) {
    if (d.vertices != 4) return;
    uint32_t stage1 = (d.tex_modes >> 5) & 0x1F;
    if (stage1 != 6 && stage1 != 7) return;
    const float (*v)[4] = d.v[0];
    if (!v) return;
    float lo = v[0][1], hi = lo, x = 0, z = 0;
    for (uint32_t i = 0; i < 4; i++) {
        lo = std::min(lo, v[i][1]);
        hi = std::max(hi, v[i][1]);
        x += v[i][0] / 4;
        z += v[i][2] / 4;
    }
    if (hi - lo > 0.5f) return;
    for (const Drop& o : g_seen)
        if (std::abs(o.x - x) < 2 && std::abs(o.z - z) < 2) return;  // a pair's other half
    g_seen.push_back({x, hi, z});
}

// a skinning program's position register p and scale register S, by its translation; p < 0 for another kind
struct Skin {
    int p = -1, scale = 0;
};
Skin ParseSkin(const std::string& hlsl) {
    static const std::regex pos(R"(mac_mul\(\(\(c\[(\d+)\]\)\.xxxw\), \(\(i\.v(\d)\)\.xyzw\)\))");
    std::smatch m;
    if (!std::regex_search(hlsl, m, pos)) return {};
    int p = std::stoi(m[2]), scale = std::stoi(m[1]);
    std::string weight = "a0 + 108, 0, 191)]).xyzw), ((i.v" + std::to_string(p + 2) + ").xxxx))";
    std::string second = "((i.v" + std::to_string(p + 1) + ").yyyy)";
    if (p > 13 || hlsl.find(weight) == std::string::npos || hlsl.find(second) == std::string::npos) return {};
    return {p, scale};
}

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void SeenSkin(const gpu::WatchedDraw& d) {
    static std::unordered_map<uint64_t, Skin> skins;
    auto [it, fresh] = skins.try_emplace(d.program);
    if (fresh) it->second = ParseSkin(*d.hlsl);
    const Skin& k = it->second;
    if (k.p < 0 || !d.v[k.p] || !d.v[k.p + 1] || !d.v[k.p + 2]) return;
    const float* view[3] = {d.c[96].data(), d.c[97].data(), d.c[98].data()};
    for (const float* row : view)
        if (std::abs(Dot3(row, row) - 1) > 0.02f) return;  // no rotation: not the view
    const float* scale = d.c[k.scale].data();
    for (uint32_t i = 0; i < d.vertices && g_body.size() < kBodyPoints; i++) {
        const float *at = d.v[k.p][i], *bones = d.v[k.p + 1][i], *weights = d.v[k.p + 2][i];
        float pm[4] = {at[0] * scale[0], at[1] * scale[0], at[2] * scale[0], at[3] * scale[3]};
        int b0 = int(std::floor(bones[0] + 1e-3f)), b1 = int(std::floor(bones[1] + 1e-3f));
        if (b0 < -108 || b0 > 191 - 110 || b1 < -108 || b1 > 191 - 110) continue;
        float v[3];
        for (int j = 0; j < 3; j++) {
            const float *r0 = d.c[108 + b0 + j].data(), *r1 = d.c[108 + b1 + j].data();
            v[j] = 0;
            for (int e = 0; e < 4; e++) v[j] += pm[e] * (r0[e] * weights[0] + r1[e] * weights[1]);
            v[j] -= view[j][3];
        }
        std::array<float, 3> w = {view[0][0] * v[0] + view[1][0] * v[1] + view[2][0] * v[2],
                                  view[0][1] * v[0] + view[1][1] * v[1] + view[2][1] * v[2],
                                  view[0][2] * v[0] + view[1][2] * v[1] + view[2][2] * v[2]};
        bool again = std::any_of(g_body.begin(), g_body.end(), [&](const std::array<float, 3>& o) {
            return std::abs(o[0] - w[0]) < 1 && std::abs(o[1] - w[1]) < 1 && std::abs(o[2] - w[2]) < 1;
        });
        if (!again) g_body.push_back(w);  // the fur's shells and the shadow pass draw the same skin again
    }
}

uint64_t SurfaceKey(int i, int j) { return uint64_t(uint32_t(i)) << 32 | uint32_t(j); }

// the water's vertices, kept by square
void SeenWater(const gpu::WatchedDraw& d) {
    const float (*v)[4] = d.v[0];
    if (!v) return;
    if (g_surface.size() > kSurfaceCells) g_surface.clear();
    float scale = d.c[114][0];
    for (uint32_t k = 0; k < d.vertices; k++) {
        int i = int(std::floor(v[k][0] * scale / kSurfaceCell)), j = int(std::floor(v[k][2] * scale / kSurfaceCell));
        g_surface[SurfaceKey(i, j)] = v[k][1] * scale;
    }
}

// the surface's height at x z, the mean of the squares there and beside it that hold water
bool LevelAt(float x, float z, float& y) {
    int ci = int(std::floor(x / kSurfaceCell)), cj = int(std::floor(z / kSurfaceCell)), n = 0;
    float sum = 0;
    for (int j = cj - 1; j <= cj + 1; j++) {
        for (int i = ci - 1; i <= ci + 1; i++) {
            auto it = g_surface.find(SurfaceKey(i, j));
            if (it == g_surface.end()) continue;
            sum += it->second;
            n++;
        }
    }
    if (n) y = sum / float(n);
    return n != 0;
}

// the sheet moved so its centre is on x z, by whole cells, what it held kept where it lies; at rest the first time
void Recentre(float x, float z) {
    if (!g_placed) {
        g_cx = x;
        g_cz = z;
        g_placed = true;
        return;
    }
    int sx = int(std::lround((x - g_cx) / kCell)), sz = int(std::lround((z - g_cz) / kCell));
    g_cx += float(sx) * kCell;
    g_cz += float(sz) * kCell;
    for (std::vector<float>* f : {&g_now, &g_before, &g_foot}) {
        std::fill(g_next.begin(), g_next.end(), 0.0f);
        for (int j = std::max(0, -sz); j < std::min(kGrid, kGrid - sz); j++)
            for (int i = std::max(0, -sx); i < std::min(kGrid, kGrid - sx); i++)
                g_next[size_t(j * kGrid + i)] = (*f)[size_t((j + sz) * kGrid + i + sx)];
        f->swap(g_next);
    }
}

// a dip pressed in at rest: it springs back and rings out
void Press(float x, float z, float depth = kDropDepth, float radius = kDropRadius) {
    float ox = g_cx - kExtent / 2, oz = g_cz - kExtent / 2;
    int ci = int((x - ox) / kCell), cj = int((z - oz) / kCell), reach = int(radius * 3 / kCell) + 1;
    for (int j = std::max(cj - reach, 1); j <= std::min(cj + reach, kGrid - 2); j++) {
        for (int i = std::max(ci - reach, 1); i <= std::min(ci + reach, kGrid - 2); i++) {
            float dx = ox + (float(i) + 0.5f) * kCell - x, dz = oz + (float(j) + 0.5f) * kCell - z;
            float dip = depth * std::exp(-(dx * dx + dz * dz) / (radius * radius));
            g_now[size_t(j * kGrid + i)] -= dip;
            g_before[size_t(j * kGrid + i)] -= dip;
        }
    }
}

// the bodies' footprint now, and the water pressed by its change since last frame
void Wade() {
    std::fill(g_foot_next.begin(), g_foot_next.end(), 0.0f);
    float ox = g_cx - kExtent / 2, oz = g_cz - kExtent / 2;
    int reach = int(kBodyRadius * 3 / kCell) + 1;
    for (const auto& p : g_body) {
        float level;
        if (!LevelAt(p[0], p[2], level)) continue;
        float depth = level - p[1];
        if (depth < -kBodyAbove || depth > kBodyBelow) continue;
        float weight = std::min(1.0f, (depth + kBodyAbove) / kBodyAbove) *
                       std::min(1.0f, (kBodyBelow - depth) / (kBodyBelow / 3));
        int ci = int((p[0] - ox) / kCell), cj = int((p[2] - oz) / kCell);
        for (int j = std::max(cj - reach, 1); j <= std::min(cj + reach, kGrid - 2); j++) {
            for (int i = std::max(ci - reach, 1); i <= std::min(ci + reach, kGrid - 2); i++) {
                float dx = ox + (float(i) + 0.5f) * kCell - p[0], dz = oz + (float(j) + 0.5f) * kCell - p[2];
                g_foot_next[size_t(j * kGrid + i)] += weight * std::exp(-(dx * dx + dz * dz) / (kBodyRadius * kBodyRadius));
            }
        }
    }
    for (size_t c = 0; c < g_foot_next.size(); c++) {
        float f = std::min(g_foot_next[c], 1.0f), change = (f - g_foot[c]) * kBodyDepth;
        g_now[c] -= change;
        g_before[c] -= change;
        g_foot[c] = f;
    }
}

// a character's points this frame: their middle in x and z, their mean and lowest height, and those of its last
// kRecent seconds of frames
struct Sample {
    float t, y, low;
};
struct Group {
    float x = 0, z = 0, y = 0, low = 0;
    int n = 0;
    std::vector<Sample> past;
};
constexpr float kRecent = 0.5f;
std::vector<Group> g_groups;  // last frame's

// the splashes of the characters whose lowest point went down through the surface since it was last above it, fast
// enough between that frame and this
void Dive() {
    std::vector<Group> now;
    for (const auto& p : g_body) {
        auto g = std::find_if(now.begin(), now.end(), [&](const Group& o) {
            return std::abs(o.x - p[0]) < kBodySpan && std::abs(o.z - p[2]) < kBodySpan;
        });
        if (g == now.end()) {
            now.push_back({p[0], p[2], p[1], p[1], 1, {}});
            continue;
        }
        g->n++;
        g->x += (p[0] - g->x) / float(g->n);
        g->z += (p[2] - g->z) / float(g->n);
        g->y += (p[1] - g->y) / float(g->n);
        g->low = std::min(g->low, p[1]);
    }
    for (Group& g : now) {
        auto was = std::find_if(g_groups.begin(), g_groups.end(), [&](const Group& o) {
            return std::abs(o.x - g.x) < kBodyFollow && std::abs(o.z - g.z) < kBodyFollow;
        });
        if (was != g_groups.end()) g.past = std::move(was->past);
        std::erase_if(g.past, [](const Sample& o) { return o.t < g_clock - kRecent; });
        float level;
        if (LevelAt(g.x, g.z, level) && g.low <= level) {
            auto above = std::find_if(g.past.rbegin(), g.past.rend(), [&](const Sample& o) { return o.low > level; });
            float span = above != g.past.rend() ? g_clock - above->t : 0;
            float speed = span > 0 ? (above->y - g.y) / span : 0;
            if (speed >= kEntrySpeed) {
                if (!g_placed || std::hypot(g.x - g_cx, g.z - g_cz) > kFollow) Recentre(g.x, g.z);
                Press(g.x, g.z, kEntryDepth * std::min(speed / kEntryFull, 1.5f), kEntryRadius);
            }
            if (above != g.past.rend()) g.past.clear();  // once: it is in now
        }
        g.past.push_back({g_clock, g.y, g.low});
    }
    g_groups.swap(now);
}

// the wave equation, the edges held at rest
void Step() {
    for (int j = 1; j < kGrid - 1; j++) {
        const float* n = &g_now[size_t(j * kGrid)];
        for (int i = 1; i < kGrid - 1; i++) {
            float lap = n[i - 1] + n[i + 1] + n[i - kGrid] + n[i + kGrid] - 4 * n[i];
            g_next[size_t(j * kGrid + i)] = (2 * n[i] - g_before[size_t(j * kGrid + i)] + kSpeed2 * lap) * kDamping;
        }
    }
    g_before.swap(g_now);
    g_now.swap(g_next);
}

void Frame() {
    auto now = std::chrono::steady_clock::now();
    float dt = g_then.time_since_epoch().count() ? std::chrono::duration<float>(now - g_then).count() : 0;
    g_then = now;
    g_clock += dt;
    for (const Drop& d : g_seen) {
        bool old = std::any_of(g_last.begin(), g_last.end(),
                               [&](const Drop& o) { return std::abs(o.x - d.x) < 3 && std::abs(o.z - d.z) < 3; });
        if (old) continue;
        if (!g_placed || std::hypot(d.x - g_cx, d.z - g_cz) > kFollow) Recentre(d.x, d.z);
        Press(d.x, d.z);
    }
    g_last.swap(g_seen);
    g_seen.clear();
    for (const auto& p : g_body) {  // the sheet goes to a body in the water, where it may not have been yet
        float level;
        if (!LevelAt(p[0], p[2], level) || std::abs(level - p[1]) > kBodyBelow) continue;
        if (!g_placed || std::hypot(p[0] - g_cx, p[2] - g_cz) > kFollow) Recentre(p[0], p[2]);
        break;
    }
    Dive();
    if (g_placed) Wade();
    g_body.clear();
    if (!g_placed) return;
    g_owed = std::min(g_owed + dt, kStep * kSteps);
    for (; g_owed >= kStep; g_owed -= kStep) Step();
    g_version++;
}

void Fill(float (*xs)[4]) {
    xs[0][0] = g_cx - kExtent / 2;
    xs[0][1] = g_cz - kExtent / 2;
    xs[0][2] = 1 / kExtent;
    xs[0][3] = 1.0f / kGrid;
    xs[1][0] = 1 / (2 * kCell);
}

gpu::ShadingField Field() {
    if (!g_placed) return {};
    return {g_now.data(), uint32_t(kGrid), uint32_t(kGrid), g_version};
}

}  // namespace

void water::Init() {
    static const std::string vs = std::string(kVsHead) + kProject + kVsTail;
    static const std::string ds = std::string(kDsHead) + kField + "        FIELD(1)" + kProject + "\n    }";
    static const std::string ps = std::string(kPsHead) + "\n#define REFRACT " + std::to_string(kRefract) +
                                  "\n#define SWIRL_KEEP " + std::to_string(kSwirlKeep) + "\n" + kField + kPsTail;
    gpu::g_program_shading.push_back({.match = kMatch,
                                      .on = &g_dynamic,
                                      .vs = vs.c_str(),
                                      .ps = ps.c_str(),
                                      .fill = Fill,
                                      .tess = ds.c_str(),
                                      .field = Field});
    gpu::g_program_watch.push_back({.match = kRingMatch, .seen = Seen, .frame = Frame});
    gpu::g_program_watch.push_back({.match = kMatch, .seen = SeenWater, .spread = true, .vertices = kSurfaceSamples});
    gpu::g_program_watch.push_back({.match = kSkinMatch, .seen = SeenSkin, .regs = 0xF, .spread = true});
}
