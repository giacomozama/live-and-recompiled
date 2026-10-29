#include "common/water.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <array>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "gpu/shaders.h"

bool water::g_dynamic = false;

namespace {

// The water program (07F95BC3079F60E6 for SHORT3 positions, BC39BFEF13558B0A for floats) scales v0 by c114.x into
// R3, the level's space with y up and about a centimetre to the unit, and takes it to view space by c96..c98 (R4) and
// to clip space by c99..c102. oT0 scrolls the swirl texture the pixel shader bumps by, oT1 is the clip position over
// w moved to 0..1 by c112.y, where it reads the copy of the screen through the water, oT2 a mask. Only these
// programs build oT1 that way. Registers in the HLSL are hardware order, D3D's + 96.
constexpr const char* kMatch = "(mac_mul(((R6).xyzw), ((c[112]).yyyy)) + ((c[112]).yyyy))";
// The sewer's sludge (D90817ED2CB8CC09) builds the same spot the same way but hands it on in oT1.z and oT2.z, for the
// pixel shader's dot products: oT1.x and oT2.y are the bump's scale, v4.x times c112.w, which only it reads.
constexpr const char* kSludgeMatch = "mac_mul(((c[112]).wwww), ((i.v4).xxxx))";

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
constexpr float kDamping = 0.982f;    // a step: a wave's height halves in some 0.65 seconds
constexpr float kDropDepth = 4;       // units
constexpr float kDropRadius = 6;
constexpr float kFollow = kExtent / 4;  // the sheet moves to what disturbs it this far from its centre

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
// A pool's mesh is a few long triangles, its vertices hundreds of units apart, too few for the squares under a body:
// its heights are kept by kPoolCell square too, a few apart each, and a body over no square of the river's takes the
// one nearest its own height in its pool square or one beside it.
constexpr float kPoolCell = 1024;
constexpr int kPoolLevels = 4;

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
        float2 screen = (cp.xy / w * float2(1, -1)) * c[112].y + c[112].y;)";
constexpr const char* kScreen = R"(
        o.tc1.xy = screen;)";
constexpr const char* kSludgeScreen = R"(
        o.tc1.z = screen.x;
        o.tc2.z = screen.y;)";

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
// console's colour already holds: still water is as bright as it was. Some water draws (the sewer's sludge) set their
// stages up otherwise, a dot product reading the copy at stage 2 and no swirl: those are shaded and lit by the
// surface the same, but not read again.
constexpr float kRefract = 0.15f;  // screen widths of bend per unit of slope
constexpr float kSwirlKeep = 1;  // the share of the console's swirl bend kept under the surface's
constexpr const char* kPsHead = R"({
    float h = 0;
    float2 xz = i.x0.xz, dh = 0;)";
constexpr const char* kPsTail = R"(
    FIELD(1)
    {
#if STAGE1_MODE == 6 || STAGE1_MODE == 7
        float3 tilt = float3(-dh.x, 0, -dh.y);
        float2 bend = float2(dot(tilt, c[96].xyz), -dot(tilt, c[97].xyz)) * REFRACT;
        float2 swirl = float2(dot(bumpmat[1].xz, duv1), dot(bumpmat[1].yw, duv1)), at = tc1.xy + texsize[1].zw;
        float4 under = tex1.Sample(smp1, at + SWIRL_KEEP * swirl + bend), was = tex1.Sample(smp1, at + swirl);
        r0.rgb += (1 - saturate(r0.a)) * (under.rgb - was.rgb) * pc[10].rgb;
#endif
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

// The cave's pools (CFE8DDFEFA94FA5B) have a program of their own: their mesh rises and falls along its normals (v3) by
// a sine series of the clock (ilu_exp, c110's terms) into R11, the level's space (v1 by c132.x), a noise volume drifts
// over them (t1 at v0 by c123..c125) on a cell texture (t0), a mask fades the shore (t3); they blend over the cave and
// Conker mirrored under them at the surface's alpha. Then the game draws Conker mirrored again over the water, so his
// reflection never moves with it. c96..c98 take R11 to view space: their rows over their length take a point back from
// view space to the level's, the space the rings and the characters' skin are in, for programs whose rows aren't unit.
constexpr const char* kPoolMatch = "(mac_mul(((i.v3).xyzz), ((R6).yyyy)) + ((R11).xyzz))";
constexpr const char* kPoolScale = "mac_mul(((c[132]).xxxw), ((i.v1).xyzw))";
constexpr float kPoolShift = 30;     // units the textures are read off by per unit of slope
constexpr float kPoolMurk = 0.35f;   // how far what lies under the surface is clouded toward its colour
constexpr float kPoolBlur = 0.003f;  // screen widths what lies under it is softened over
constexpr float kPoolGain = 2.5f;    // the sheet's ripples on a pool, seen from above, against a river's
constexpr float kPoolRipple = 24;    // units, about the shortest ripple the sheet holds

// the level's position (the game's waves in) in x0, and how much the surface faces up (1 flat, 0 from 25 degrees off)
// in x1.x
constexpr const char* kPoolVs = R"(    {
        float3 rx = normalize(c[96].xyz), ry = normalize(c[97].xyz), rz = normalize(c[98].xyz);
        float3 d = float3(dot(R11.xyz, c[96].xyz), dot(R11.xyz, c[97].xyz), dot(R11.xyz, c[98].xyz));
        float3 n = float3(dot(i.v3.xyz, c[96].xyz), dot(i.v3.xyz, c[97].xyz), dot(i.v3.xyz, c[98].xyz));
        o.x0 = float4(rx * d.x + ry * d.y + rz * d.z, 1);
        n = rx * n.x + ry * n.y + rz * n.z;
        o.x1 = float4(smoothstep(0.9, 0.98, n.y * rsqrt(max(dot(n, n), 1e-12))), 0, 0, 0);
    })";

// the tessellated surface lifted by the ripples where it faces up, taken to the screen again as the
// program does
constexpr const char* kPoolDs = R"(    {
        float3 mp = o.x0.xyz;
        float h = 0;
        float2 xz = mp.xz, dh = 0;
        FIELD(POOL_GAIN)
        h *= saturate(o.x1.x);
        float3 rx = normalize(c[96].xyz), ry = normalize(c[97].xyz), rz = normalize(c[98].xyz);
        float3 m = float3(mp.x, mp.y + h, mp.z);
        float4 v = float4(dot(m, rx) + c[96].w, dot(m, ry) + c[97].w, dot(m, rz) + c[98].w, 1);
        float4 cp = float4(dot(v, c[99]), dot(v, c[100]), dot(v, c[101]), dot(v, c[102]));
        float w = magclamp(cp.w);
        float3 sp = cp.xyz * c[58].xyz / w + c[59].xyz;
        o.pos = float4((sp.x * 2 / surf.x - 1) * w, (1 - sp.y * 2 / surf.y) * w, sp.z * surf.z * w, w);
    })";

// Per pixel, before the textures are read: the slope of the ripples moves the cell texture and the noise
// volume as far as the surface tilts (kPoolShift, through the coordinates' change across the screen), and what lies
// under the surface (the copy of the screen just before it) is read bent by it, the slopes shaded and lit as the
// river's are. After the final combiner the game's surface is laid over that at its own alpha, so the blend writes the
// whole and what shows through moves with the water; the depth pass, if any, keeps the game's alpha.
constexpr const char* kPoolCoords = R"(
    float3 xunder = 0, xlight = 0;
    float xshade = 1;
    {
        float h = 0;
        float2 xz = i.x0.xz, dh = 0;
        // units a pixel across the surface: ripples shorter than some 16 pixels fade out before they alias into stripes
        float2 ax = ddx(xz), ay = ddy(xz);
        float foot = max(max(length(ax), length(ay)), 1e-6);
        FIELD(POOL_GAIN * saturate(POOL_RIPPLE / foot / 8 - 1))
        dh *= saturate(i.x1.x);
        // the textures read off as far as the surface tilts, in their units: the ratio of their area to the level's on
        // the screen, which stays steady where the surface turns edge on
        float area = abs(ax.x * ay.y - ay.x * ax.y);
        float2 shift = -dh * POOL_SHIFT;
        float2 b0x = ddx(tc0.xy), b0y = ddy(tc0.xy), b1x = ddx(tc1.xy), b1y = ddy(tc1.xy);
        float s0 = area > 1e-9 ? sqrt(abs(b0x.x * b0y.y - b0y.x * b0x.y) / area) : 0;
        float s1 = area > 1e-9 ? sqrt(abs(b1x.x * b1y.y - b1y.x * b1x.y) / area) : 0;
        tc0.xy += shift * s0 * tc0.w;
        tc1.xy += shift * s1 * tc1.w;
        float3 rx = normalize(c[96].xyz), ry = normalize(c[97].xyz), rz = normalize(c[98].xyz);
        float3 tilt = float3(-dh.x, 0, -dh.y);
        float2 bend = float2(dot(tilt, rx), -dot(tilt, ry)) * REFRACT;
        uint sw, sh;
        xscreen.GetDimensions(sw, sh);
        // murky: what lies under is read soft, five taps a little apart
        float2 at = i.pos.xy / float2(sw, sh) + bend, soft = POOL_BLUR * float2(1, float(sw) / float(sh));
        xunder = (2 * xscreen.SampleLevel(xscreen_smp, at, 0).rgb + xscreen.SampleLevel(xscreen_smp, at + soft, 0).rgb +
                  xscreen.SampleLevel(xscreen_smp, at - soft, 0).rgb +
                  xscreen.SampleLevel(xscreen_smp, at + float2(soft.x, -soft.y), 0).rgb +
                  xscreen.SampleLevel(xscreen_smp, at - float2(soft.x, -soft.y), 0).rgb) / 6;
        xshade = 1 - clamp(0.8 * dot(dh, float2(0.6, 0.8)), -0.15, 0.15);
        float3 tv = float3(c[96].w, c[97].w, c[98].w);
        float3 eye = -(rx * tv.x + ry * tv.y + rz * tv.z);
        float3 e = normalize(eye - float3(xz.x, i.x0.y + h, xz.y)), n = normalize(float3(-dh.x, 1, -dh.y));
        float3 flat = float3(0, 1, 0), sun = normalize(float3(0.4, 0.8, 0.45));
        if (e.y < 0) { n = -n; flat = -flat; }
        float fresnel = pow(1 - saturate(dot(n, e)), 5) - pow(1 - saturate(dot(flat, e)), 5);
        float glint = pow(saturate(dot(reflect(-e, n), sun)), 80) - pow(saturate(dot(reflect(-e, flat), sun)), 80);
        xlight = (0.25 * fresnel + 0.4 * glint) * float3(0.9, 0.95, 1);
    }
)";
constexpr const char* kPoolPost = R"(
#if !DEPTH_PASS
    xunder = lerp(xunder, outc.rgb * 0.8, POOL_MURK);  // and clouded toward the water's own colour
    outc.rgb = lerp(xunder, outc.rgb, saturate(outc.a)) * xshade + xlight;
    outc.a = 1;
#endif
)";

struct Drop {
    float x, y, z;
};

// XBE_TRACE_WATER: a line a second of what reached the sheet, to tell a pool that doesn't ripple from one whose
// ripples don't show
bool Trace() {
    static const bool trace = std::getenv("XBE_TRACE_WATER") != nullptr;
    return trace;
}
struct TraceCounts {
    float since = 0;
    int frames = 0, drops = 0, pool_draws = 0, points = 0, over = 0, near = 0;
    float pool_lo = 1e30f, pool_hi = -1e30f, body_lo = 1e30f, body_hi = -1e30f, level_lo = 1e30f, level_hi = -1e30f;
    float bx = 0, bz = 0;
};
TraceCounts g_trace;
std::vector<Drop> g_seen, g_last;  // this frame's rings' centres, last frame's
std::unordered_map<uint64_t, float> g_surface;  // the water's height by square
struct PoolLevels {
    float y[kPoolLevels];
    int n = 0;
};
std::unordered_map<uint64_t, PoolLevels> g_pool;  // the pools' heights by kPoolCell square
float g_clock = 0;  // seconds of frames
std::vector<std::array<float, 3>> g_body;  // this frame's skin points
float g_eye_x = 0, g_eye_z = 0;  // the camera in the level, from the characters' view
bool g_eye = false;

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
    g_eye_x = -(view[0][0] * view[0][3] + view[1][0] * view[1][3] + view[2][0] * view[2][3]);
    g_eye_z = -(view[0][2] * view[0][3] + view[1][2] * view[1][3] + view[2][2] * view[2][3]);
    g_eye = true;
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

// a pool's vertices that face up (v1, before the game's waves, and v3), taken back to the level's space, kept by square
// as the river's
void SeenPool(const gpu::WatchedDraw& d) {
    const float (*v)[4] = d.v[1];
    const float (*n)[4] = d.v[3];
    if (!v || !n) return;
    float row[3][3];
    for (int j = 0; j < 3; j++) {
        const float* r = d.c[96 + j].data();
        float len = std::sqrt(Dot3(r, r));
        if (len < 1e-6f) return;
        for (int k = 0; k < 3; k++) row[j][k] = r[k] / len;
    }
    if (g_surface.size() > kSurfaceCells) g_surface.clear();
    if (g_pool.size() > kSurfaceCells) g_pool.clear();
    float scale = d.c[132][0];
    for (uint32_t k = 0; k < d.vertices; k++) {
        float p[3] = {v[k][0] * scale, v[k][1] * scale, v[k][2] * scale}, w[3] = {}, up[3] = {};
        for (int j = 0; j < 3; j++) {
            float dv = Dot3(d.c[96 + j].data(), p), dn = Dot3(d.c[96 + j].data(), n[k]);
            for (int e = 0; e < 3; e++) {
                w[e] += row[j][e] * dv;
                up[e] += row[j][e] * dn;
            }
        }
        if (up[1] < 0.9f * std::sqrt(Dot3(up, up))) continue;
        int i = int(std::floor(w[0] / kSurfaceCell)), j = int(std::floor(w[2] / kSurfaceCell));
        g_surface[SurfaceKey(i, j)] = w[1];
        PoolLevels& l = g_pool[SurfaceKey(int(std::floor(w[0] / kPoolCell)), int(std::floor(w[2] / kPoolCell)))];
        if (l.n < kPoolLevels && std::none_of(l.y, l.y + l.n, [&](float y) { return std::abs(y - w[1]) < 8; }))
            l.y[l.n++] = w[1];
        g_trace.pool_lo = std::min(g_trace.pool_lo, w[1]);
        g_trace.pool_hi = std::max(g_trace.pool_hi, w[1]);
    }
    g_trace.pool_draws++;
}

// the surface's height at x z, the mean of the squares there and beside it that hold water, else the pool's height
// there nearest near
bool LevelAt(float x, float z, float near, float& y) {
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
    if (n) {
        y = sum / float(n);
        return true;
    }
    ci = int(std::floor(x / kPoolCell));
    cj = int(std::floor(z / kPoolCell));
    float best = -1;
    for (int j = cj - 1; j <= cj + 1; j++) {
        for (int i = ci - 1; i <= ci + 1; i++) {
            auto it = g_pool.find(SurfaceKey(i, j));
            if (it == g_pool.end()) continue;
            for (int k = 0; k < it->second.n; k++) {
                float off = std::abs(it->second.y[k] - near);
                if (best < 0 || off < best) {
                    best = off;
                    y = it->second.y[k];
                }
            }
        }
    }
    return best >= 0;
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
        if (!LevelAt(p[0], p[2], p[1], level)) continue;
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
        if (LevelAt(g.x, g.z, g.y, level) && g.low <= level) {
            auto above = std::find_if(g.past.rbegin(), g.past.rend(), [&](const Sample& o) { return o.low > level; });
            float span = above != g.past.rend() ? g_clock - above->t : 0;
            float speed = span > 0 ? (above->y - g.y) / span : 0;
            if (speed >= kEntrySpeed && g_placed) {
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

// this frame's bodies against the surface, and once a second the line
void TraceFrame(float dt) {
    TraceCounts& t = g_trace;
    t.frames++;
    for (const auto& p : g_body) {
        t.points++;
        t.body_lo = std::min(t.body_lo, p[1]);
        t.body_hi = std::max(t.body_hi, p[1]);
        t.bx = p[0];
        t.bz = p[2];
        float level;
        if (!LevelAt(p[0], p[2], p[1], level)) continue;
        t.over++;
        t.level_lo = std::min(t.level_lo, level);
        t.level_hi = std::max(t.level_hi, level);
        float depth = level - p[1];
        if (depth >= -kBodyAbove && depth <= kBodyBelow) t.near++;
    }
    t.since += dt;
    if (t.since < 1) return;
    float most = 0;
    for (float h : g_now) most = std::max(most, std::abs(h));
    LOGR("water: %d frames, %d drops, %d pool draws y %.0f..%.0f, %zu squares | body points %d (last %.0f %.0f) y "
         "%.0f..%.0f, %d over water (level %.0f..%.0f), %d near its surface | sheet %s at %.0f %.0f, %.0f from the "
         "body, highest %.2f",
         t.frames, t.drops, t.pool_draws, t.pool_lo, t.pool_hi, g_surface.size(), t.points, t.bx, t.bz, t.body_lo,
         t.body_hi, t.over, t.level_lo, t.level_hi, t.near, g_placed ? "placed" : "unplaced", g_cx, g_cz,
         std::hypot(t.bx - g_cx, t.bz - g_cz), most);
    t = {};
}

void Frame() {
    auto now = std::chrono::steady_clock::now();
    float dt = g_then.time_since_epoch().count() ? std::chrono::duration<float>(now - g_then).count() : 0;
    g_then = now;
    g_clock += dt;
    std::vector<Drop> fresh;  // the rings in their first frame
    for (const Drop& d : g_seen) {
        bool old = std::any_of(g_last.begin(), g_last.end(),
                               [&](const Drop& o) { return std::abs(o.x - d.x) < 3 && std::abs(o.z - d.z) < 3; });
        if (!old) fresh.push_back(d);
    }
    // The sheet goes to what disturbs the water nearest the camera, a new ring or a body in it, where it may not
    // have been yet: one place a frame, so a second character wading further off (the submarine) doesn't
    // pull it back and forth, dropping the trail behind Conker each time; its own ripples fall off the sheet's edge.
    float best = -1, bx = 0, bz = 0;
    auto consider = [&](float x, float z) {
        float d = g_eye ? std::hypot(x - g_eye_x, z - g_eye_z) : 0;
        if (best < 0 || d < best) {
            best = d;
            bx = x;
            bz = z;
        }
    };
    for (const Drop& d : fresh) consider(d.x, d.z);
    for (const auto& p : g_body) {
        float level;
        if (LevelAt(p[0], p[2], p[1], level) && std::abs(level - p[1]) <= kBodyBelow) consider(p[0], p[2]);
    }
    if (best >= 0 && (!g_placed || std::hypot(bx - g_cx, bz - g_cz) > kFollow)) Recentre(bx, bz);
    for (const Drop& d : fresh) {
        Press(d.x, d.z);
        g_trace.drops++;
    }
    if (Trace()) TraceFrame(dt);
    g_last.swap(g_seen);
    g_seen.clear();
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
    static const std::string vs = std::string(kVsHead) + kProject + kScreen + kVsTail;
    static const std::string ds = std::string(kDsHead) + kField + "        FIELD(1)" + kProject + kScreen + "\n    }";
    static const std::string sludge_vs = std::string(kVsHead) + kProject + kSludgeScreen + kVsTail;
    static const std::string sludge_ds =
        std::string(kDsHead) + kField + "        FIELD(1)" + kProject + kSludgeScreen + "\n    }";
    static const std::string ps = std::string(kPsHead) + "\n#define REFRACT " + std::to_string(kRefract) +
                                  "\n#define SWIRL_KEEP " + std::to_string(kSwirlKeep) + "\n" + kField + kPsTail;
    gpu::g_program_shading.push_back({.match = kSludgeMatch,  // before the river's, which it matches too
                                      .on = &g_dynamic,
                                      .vs = sludge_vs.c_str(),
                                      .ps = ps.c_str(),
                                      .fill = Fill,
                                      .tess = sludge_ds.c_str(),
                                      .field = Field});
    gpu::g_program_shading.push_back({.match = kMatch,
                                      .on = &g_dynamic,
                                      .vs = vs.c_str(),
                                      .ps = ps.c_str(),
                                      .fill = Fill,
                                      .tess = ds.c_str(),
                                      .field = Field});
    static const std::string pool_functions = "#define REFRACT " + std::to_string(kRefract) + "\n#define POOL_GAIN " + std::to_string(kPoolGain) +
                                              "\n#define POOL_MURK " + std::to_string(kPoolMurk) +
                                              "\n#define POOL_BLUR " + std::to_string(kPoolBlur) + "\n#define POOL_SHIFT " +
                                              std::to_string(kPoolShift) + "\n#define POOL_RIPPLE " +
                                              std::to_string(kPoolRipple) + "\n" + kField;
    gpu::g_program_shading.push_back({.match = kPoolMatch,
                                      .on = &g_dynamic,
                                      .vs = kPoolVs,
                                      .fill = Fill,
                                      .tess = kPoolDs,
                                      .field = Field,
                                      .functions = pool_functions.c_str(),
                                      .also = {kPoolScale},
                                      .screen = true,
                                      .post = kPoolPost,
                                      .coords = kPoolCoords});
    gpu::g_program_watch.push_back({.match = kRingMatch, .seen = Seen, .frame = Frame});
    gpu::g_program_watch.push_back({.match = kMatch, .seen = SeenWater, .spread = true, .vertices = kSurfaceSamples});
    gpu::g_program_watch.push_back({.match = kSkinMatch, .seen = SeenSkin, .regs = 0xF, .spread = true});
    gpu::g_program_watch.push_back({.match = kPoolMatch,
                                    .seen = SeenPool,
                                    .regs = 0xA,
                                    .spread = true,
                                    .vertices = kSurfaceSamples,
                                    .also = kPoolScale});
}
