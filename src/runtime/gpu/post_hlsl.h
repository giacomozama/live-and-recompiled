#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "gpu/smaa_hlsl.h"

namespace gpu {

// Full-screen passes shared by the native renderers.
// the frame as an SDR display shows it (gamma p.y, Rec.709) into HDR10: Rec.2020, PQ, white at p.x x 10000 nits.
// encode: the game's picture (t0) under the overlay (t2, premultiplied over transparent, when q.y), its highlights
// taken up to p.z x white above p.w where the mask (t1 when q.x, the picture's 3D, vp its viewport) has them.
// mask: 1 where the finished picture (t0) is still the 3D picture as the 2D came to draw over it (t1), else 0
inline const char* kHdrHlsl = R"(
cbuffer B : register(b1) { float4 p; float4 vp; float4 q; };
Texture2D img : register(t0);
Texture2D aux : register(t1);
Texture2D ui : register(t2);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float3 Pq(float3 y) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 ym = pow(saturate(y), m1);
    return pow((c1 + c2 * ym) / (1 + c3 * ym), m2);
}
float Hash(float2 v) { return frac(sin(dot(v, float2(12.9898, 78.233))) * 43758.5453); }
// the brightest channel above the knee p.w bent up to p.z at white, as steep as below at the knee; the hue kept
float3 Expand(float3 c) {
    float x = max(c.r, max(c.g, c.b));
    if (x <= p.w) return c;
    float t = (x - p.w) / (1 - p.w);
    return c * ((x + (p.z - 1) * t * t) / x);
}
float4 encode(O i) : SV_Target {
    int2 px = int2(i.p.xy);
    float3 g = img.Load(int3(px, 0)).rgb;
    float4 u = q.y > 0 ? ui.Load(int3(px, 0)) : float4(0, 0, 0, 0);
    float3 lin = pow(saturate(u.rgb + g * (1 - u.a)), p.y);
    if (q.x > 0 && u.a < 1) {
        float m = aux.SampleLevel(ls, (i.p.xy - vp.xy) * vp.zw, 0).r;
        if (m > 0) {
            // a step of triangular dither: the 8 bit picture's steps are stretched with its highlights
            float d = (Hash(i.p.xy + q.z) + Hash(i.p.xy + q.z + 17.0) - 1) / 255.0;
            float3 gl = pow(saturate(g + d), p.y);
            lin += (Expand(gl) - pow(saturate(g), p.y)) * ((1 - u.a) * m);
        }
    }
    const float3x3 to2020 = {0.627404, 0.329283, 0.043313,
                             0.069097, 0.919540, 0.011362,
                             0.016391, 0.088013, 0.895595};
    return float4(Pq(mul(to2020, lin) * p.x), 1);
}
float4 mask(O i) : SV_Target {
    int3 px = int3(int2(i.p.xy), 0);
    return any(abs(img.Load(px).rgb - aux.Load(px).rgb) > 0.5 / 255.0) ? 0 : 1;
}
)";

// a D24S8 depth buffer as the console's memory holds it, read through a 32 bit color format:
// R G B A take the bytes order picks.
// fur.x 1: the frame (t2) holds fur's mark in its alpha (ProgramShading::unsharpened), and a pixel fur mostly covers
// takes fur's depth for the depth of field: the strands at its edge and the shells between write none (the depth
// pass leaves what they cover faintly to what's behind), so the composite blurred them by the background's, sharp
// fur fading into a blurred background and blurred fur left sharp at its edge over a sharp one. Fur's depth there is
// the nearest of the covered pixels around that wrote theirs, the skin and the dense shells near it: rings out to 8
// console pixels (fur.z host pixels each), the first ring with one nearer. fur.y: frame pixels per depth pixel
inline const char* kDepthBytesHlsl = R"(
cbuffer B : register(b1) { float4 order; float4 fur; };
#ifdef DEPTH_MULTISAMPLED
Texture2DMS<float> img : register(t0);
Texture2DMS<uint2> sten : register(t1);
float DepthAt(int2 p) { return img.Load(p,0); }
uint StencilAt(int2 p) { return sten.Load(p,0).y; }
uint2 DepthSize() { uint2 s; uint samples; img.GetDimensions(s.x,s.y,samples); return s; }
#else
Texture2D<float> img : register(t0);
#ifdef DEPTH_STENCIL_GREEN
Texture2D<uint2> sten : register(t1);
uint StencilAt(int2 p) { return sten.Load(int3(p,0)).y; }
#else
Texture2D<uint> sten : register(t1);
uint StencilAt(int2 p) { return sten.Load(int3(p,0)); }
#endif
float DepthAt(int2 p) { return img.Load(int3(p,0)); }
uint2 DepthSize() { uint2 s; img.GetDimensions(s.x,s.y); return s; }
#endif
Texture2D mark : register(t2);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
bool furred(int2 p) { return mark.Load(int3(int2(p * fur.y), 0)).a >= 0.5; }
float depthof(int2 p) {
    float own = saturate(DepthAt(p));
    if (fur.x == 0 || !furred(p)) return own;
    uint2 size = DepthSize();
    [loop] for (int ring = 1; ring <= 8; ring++) {
        float best = own;
        [unroll] for (int t = 0; t < 8; t++) {
            float ang = (t + (ring & 1) * 0.5) * 0.7853982;
            int2 q = clamp(p + int2(round(float2(cos(ang), sin(ang)) * ring * fur.z)), 0, int2(size) - 1);
            float z = saturate(DepthAt(q));
            if (z < best && furred(q)) best = z;
        }
        if (best < own) return best;
    }
    return own;
}
uint depthword(O i) {
    uint z = (uint)round(depthof(int2(i.p.xy)) * 16777215.0);
    return z << 8 | (StencilAt(int2(i.p.xy)) & 255);
}
float4 psmain(O i) : SV_Target {
    uint d = depthword(i);
    return float4(((d >> ((uint)order.x * 8)) & 255) / 255.0, ((d >> ((uint)order.y * 8)) & 255) / 255.0,
                  ((d >> ((uint)order.z * 8)) & 255) / 255.0, ((d >> ((uint)order.w * 8)) & 255) / 255.0);
}
)";

// the effects over the frame's 3D picture (app::Settings) past SMAA (kSmaaHeadHlsl): grade's chromatic aberration and
// film grain. px: 1 / size, size; opts: aberration, grain (0 to 1), the frame's grain seed
inline const char* kPostHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 opts; };
Texture2D img : register(t0);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float hash(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float4 grade(O i) : SV_Target {
    float4 c = img.SampleLevel(ls, i.uv, 0);
    // red and blue pulled apart along the radius, up to 0.6% of the frame at the corners
    float2 d = (i.uv - 0.5) * opts.x * 0.012;
    c.r = img.SampleLevel(ls, i.uv + d, 0).r;
    c.b = img.SampleLevel(ls, i.uv - d, 0).b;
    // grain: noise per pixel, two hashes summed so it gathers around 0, new each frame, strongest in the shadows
    float2 g = floor(i.p.xy) + opts.z * float2(37.13, 17.71);
    float n = 0.5 * (hash(g) + hash(g + float2(113.7, 271.3)));
    float l = dot(c.rgb, float3(0.299, 0.587, 0.114));
    c.rgb = saturate(c.rgb + (n - 0.5) * opts.y * 0.3 * (1 - 0.6 * l));
    return c;
}
)";

// ambient occlusion over the 3D picture, before the title's own passes read it (ApplyOcclusion). occlusion, at half
// the size, from the depth (t0 and t1): each pixel's view space position and its normal from its neighbours, then
// samples on a spiral within the reach, each occluding by the cosine of its direction to the normal, less where it lies
// farther than the reach; blurx and blury (the occlusion at t0, the depth at t1) blur it across like depths alone;
// apply (the picture at t0, the occlusion at t1) darkens the picture by it. b1: px the picture's 1 / size, the reach,
// the strength; frame the guest size and A, B of depth = A + B / w; vp the viewport's x y scale and offset; pr and pr2
// the projection's x row (x, z, w) and y row (y, z, w)
inline const char* kOcclusionHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 frame; float4 vp; float4 pr; float4 pr2; };
Texture2D a : register(t0);
Texture2D b : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float DepthAt(float2 uv) { return b.SampleLevel(ps, uv, 0).r; }
float ViewW(float d) { return frame.w / min(d - frame.z, -1e-7); }
float3 ViewAt(float2 uv) {
    float w = ViewW(DepthAt(uv)), z = -w;
    float2 clip = (uv * frame.xy - vp.zw) * w / vp.xy;
    return float3((clip.x - pr.y * z - pr.z) / pr.x, (clip.y - pr2.y * z - pr2.z) / pr2.x, z);
}
float4 occlusion(O i) : SV_Target {
    float2 uv = i.uv;
    if (DepthAt(uv) >= 0.9999999) return 1;
    float3 p = ViewAt(uv);
    // the neighbour on the nearer side of each axis, so an edge doesn't bend the normal
    float3 r = ViewAt(uv + float2(px.x, 0)) - p, l = p - ViewAt(uv - float2(px.x, 0));
    float3 u = ViewAt(uv + float2(0, px.y)) - p, d = p - ViewAt(uv - float2(0, px.y));
    float3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(u.z) < abs(d.z) ? u : d));
    if (dot(n, p) > 0) n = -n;
    // the reach in guest pixels; under two it would sample the pixel's own surface
    float reach = px.z, w = -p.z;
    float rpx = reach * abs(pr.x * vp.x) / w;
    if (rpx < 2) return 1;
    float turn = 6.2831853 * frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    float sum = 0, r2 = reach * reach;
    const int kTaps = 12;
    [unroll] for (int k = 0; k < kTaps; k++) {
        float f = (k + 0.5) / kTaps, ang = f * 7 * 6.2831853 + turn;  // seven turns
        float2 q = uv + rpx * f * float2(cos(ang), sin(ang)) / frame.xy;
        float3 v = ViewAt(q) - p;
        float vv = dot(v, v), fall = max(1 - vv / r2, 0);
        sum += fall * fall * max(dot(v, n) * rsqrt(vv + 1e-4 * r2) - 0.1, 0);
    }
    float o = saturate(1 - px.w * 2 * sum / kTaps);
    return lerp(1, o, saturate(rpx / 2 - 1));
}
float4 Blur(float2 uv, float2 dir) {
    float w0 = ViewW(DepthAt(uv)), sum = 0, weights = 0;
    [unroll] for (int k = -3; k <= 3; k++) {
        float2 q = uv + dir * k;
        float wt = exp(-k * k / 8.0) * saturate(1 - 20 * abs(ViewW(DepthAt(q)) - w0) / w0);
        sum += a.SampleLevel(ps, q, 0).r * wt;
        weights += wt;
    }
    return weights > 0 ? sum / weights : a.SampleLevel(ps, uv, 0).r;
}
float4 blurx(O i) : SV_Target { return Blur(i.uv, float2(px.x * 2, 0)); }
float4 blury(O i) : SV_Target { return Blur(i.uv, float2(0, px.y * 2)); }
float4 apply(O i) : SV_Target {
    float4 c = a.SampleLevel(ps, i.uv, 0);
    c.rgb *= b.SampleLevel(ls, i.uv, 0).r;
    return c;
}
)";

// camera motion blur over the 3D picture (ApplyPost, first of its effects): each pixel's view space position from the
// depth (t1), as the occlusion's, taken by d0..d2 to where the camera of the frame before saw it, the pixel's way
// across the screen since then; the picture (t0) averaged along it, scaled and held to a length. What lies near the
// camera (the player's character the camera follows, a weapon held up to it) stays sharp and keeps out of the blur
// around it: the camera's own motion says nothing of how it moves; so does the screen's middle, where the camera
// looks. b1: px the picture's 1 / size, the scale (the
// shutter's share of the time between pictures over the frame's pictures), the longest way as a share of the height;
inline const char* kMotionBlurHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 frame; float4 vp; float4 pr; float4 pr2; float4 d0; float4 d1; float4 d2; };
Texture2D a : register(t0);
Texture2D b : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float ViewW(float d) { return frame.w / min(d - frame.z, -1e-7); }
float3 ViewAt(float2 uv, float w) {
    float z = -w;
    float2 clip = (uv * frame.xy - vp.zw) * w / vp.xy;
    return float3((clip.x - pr.y * z - pr.z) / pr.x, (clip.y - pr2.y * z - pr2.z) / pr2.x, z);
}
float2 UvOf(float3 v) {
    float2 clip = float2(pr.x * v.x + pr.y * v.z + pr.z, pr2.x * v.y + pr2.y * v.z + pr2.z);
    return (clip / -v.z * vp.xy + vp.zw) / frame.xy;
}
// none of the blur nearer than 150 units (a metre and a half), all of it past 400
float Far(float w) { return saturate((w - 150) / 250); }
// none of it within 12% of the height of the screen's middle, where the camera looks (the character it follows round),
// all of it from 35%
float Edge(float2 uv) { return saturate((length((uv - 0.5) * float2(frame.x / frame.y, 1)) - 0.12) / 0.23); }
float Keep(float2 uv, float w) { return Far(w) * Edge(uv); }
float4 blur(O i) : SV_Target {
    float2 uv = i.uv;
    float4 c = a.SampleLevel(ps, uv, 0);
    float w = ViewW(b.SampleLevel(ps, uv, 0).r);
    float3 p = ViewAt(uv, w);
    float3 q = float3(dot(d0.xyz, p) + d0.w, dot(d1.xyz, p) + d1.w, dot(d2.xyz, p) + d2.w);
    if (q.z > -1) return c;  // behind the camera before
    float2 way = (UvOf(q) - uv) * frame.xy * px.z * Keep(uv, w);  // guest pixels
    float len = length(way), most = px.w * frame.y;
    if (len < 0.5) return c;
    if (len > most) way *= most / len;
    float2 step = way / frame.xy;
    // taps across the way, centred on the pixel, each pixel's own offset into them so the steps don't band
    float jitter = frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    float4 sum = c;
    float weights = 1;
    const int kTaps = 16;
    [unroll] for (int k = 0; k < kTaps; k++) {
        float2 t = uv + step * ((k + jitter) / kTaps - 0.5);
        float wt = Keep(t, ViewW(b.SampleLevel(ps, t, 0).r));
        sum += a.SampleLevel(ls, t, 0) * wt;
        weights += wt;
    }
    return sum / weights;
}
)";

// SMAA (gpu/smaa_hlsl.h) in its custom shading language mode, a source per pass (SmaaHlsl). No texture is handed to a
// function: SMAA's texture arguments become unused uints and its sampling macros read the global of the argument's
// name, which the pass declares.
inline const char* kSmaaHeadHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 opts; };
SamplerState ls : register(s0);
SamplerState ps : register(s1);
#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS px
#define SMAATexture2D(tex) uint tex##_unused
#define SMAATexturePass2D(tex) 0u
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(ls, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(ps, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(ls, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(ls, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(ps, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(ls, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// the passes, each computing the offsets SMAA's vertex shaders would hand it
inline const char* kSmaaTailHlsl = R"(
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
#ifdef SMAA_EDGES
float4 edges(O i) : SV_Target {
    float4 off[3];
    SMAAEdgeDetectionVS(i.uv, off);
    return float4(SMAALumaEdgeDetectionPS(i.uv, off, 0u), 0, 0);
}
#endif
#ifdef SMAA_WEIGHTS
float4 weights(O i) : SV_Target {
    float4 off[3];
    float2 pix;
    SMAABlendingWeightCalculationVS(i.uv, pix, off);
    return SMAABlendingWeightCalculationPS(i.uv, pix, off, 0u, 0u, 0u, float4(0, 0, 0, 0));
}
#endif
#ifdef SMAA_BLEND
float4 blend(O i) : SV_Target {
    float4 off;
    SMAANeighborhoodBlendingVS(i.uv, off);
    return SMAANeighborhoodBlendingPS(i.uv, off, 0u, 0u);
}
#endif
)";

// SMAA's pass 0 edges (colorTex at t0), 1 blending weights (edgesTex, areaTex, searchTex at t0 to t2), 2 neighbourhood
// blending (colorTex, blendTex at t0 and t1); the names a pass doesn't read, its functions' all the same, sit at t3
inline const char* SmaaHlsl(int pass) {
    static const std::array<std::string, 3> src = [] {
        const char* names[] = {"colorTex", "edgesTex", "areaTex", "searchTex", "blendTex", "tex", "predicationTex",
                               "depthTex", "velocityTex", "currentColorTex", "previousColorTex"};
        const std::vector<std::string> used[3] = {{"colorTex"}, {"edgesTex", "areaTex", "searchTex"},
                                                  {"colorTex", "blendTex"}};
        const char* defines[3] = {"#define SMAA_EDGES\n", "#define SMAA_WEIGHTS\n", "#define SMAA_BLEND\n"};
        std::array<std::string, 3> out;
        for (size_t p = 0; p < 3; p++) {
            std::string g = defines[p];
            for (const char* n : names) {
                auto at = std::find(used[p].begin(), used[p].end(), n);
                size_t reg = at == used[p].end() ? 3 : size_t(at - used[p].begin());
                g += "Texture2D " + std::string(n) + " : register(t" + std::to_string(reg) + ");\n";
            }
            out[p] = g + kSmaaHeadHlsl + kSmaaHlsl + kSmaaTailHlsl;
        }
        return out;
    }();
    return src[size_t(pass)].c_str();
}

inline bool CameraMotion(const std::array<float, 4> (&n)[3], const std::array<float, 4> (&before)[3],
                         float elapsed, float d[12], float& gain) {
    // view now's inverse: the rotation (and any scale) part's inverse, and the offset taken back through it
    const float a = n[0][0], b = n[0][1], c = n[0][2], e = n[1][0], f = n[1][1], g = n[1][2], h = n[2][0],
                i = n[2][1], k = n[2][2];
    const float det = a * (f * k - g * i) - b * (e * k - g * h) + c * (e * i - f * h);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return false;
    const float inv[3][3] = {{(f * k - g * i) / det, (c * i - b * k) / det, (b * g - c * f) / det},
                             {(g * h - e * k) / det, (a * k - c * h) / det, (c * e - a * g) / det},
                             {(e * i - f * h) / det, (b * h - a * i) / det, (a * f - b * e) / det}};
    float inv_t[3];
    for (int r = 0; r < 3; r++) inv_t[r] = -(inv[r][0] * n[0][3] + inv[r][1] * n[1][3] + inv[r][2] * n[2][3]);
    // before . inverse(now)
    float most = 0;
    for (int r = 0; r < 3; r++) {
        const auto& p = before[r];
        for (int col = 0; col < 3; col++) {
            d[r * 4 + col] = p[0] * inv[0][col] + p[1] * inv[1][col] + p[2] * inv[2][col];
            most = std::max(most, std::fabs(d[r * 4 + col] - (r == col ? 1.0f : 0.0f)));
        }
        d[r * 4 + 3] = p[0] * inv_t[0] + p[1] * inv_t[1] + p[2] * inv_t[2] + p[3];
        most = std::max(most, std::fabs(d[r * 4 + 3]) * 1e-3f);
    }
    if (!std::isfinite(most) || most < 1e-5f) return false;  // still
    // a cut: turned more than 30 degrees, or moved more than 400 units (4 m), in a frame
    const float turn_cos = (d[0] + d[5] + d[10] - 1) * 0.5f;
    const float moved = std::sqrt(d[3] * d[3] + d[7] * d[7] + d[11] * d[11]);
    if (!(turn_cos > 0.866f && moved < 400.0f)) return false;
    const float dt = std::clamp(elapsed, 1.0f / 240, 1.0f / 15);
    const float speed = std::acos(std::min(turn_cos, 1.0f)) * (180.0f / 3.14159265f) / dt;  // degrees a second
    const float x = std::clamp((speed - 40.0f) / 100.0f, 0.0f, 1.0f);
    gain = x * x * (3 - 2 * x);
    return gain > 0.0f;
}

}  // namespace gpu
