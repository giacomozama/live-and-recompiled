#include "common/fur.h"

#include <cstdlib>

#include "gpu/shaders.h"

bool fur::g_shading = false;

namespace {

// The fur programs (four, 1B4CFC40E338AEDB and its variants) offset the skinned position along the normal by
// c85.x * v5.x: c85.x is the shell's offset, 0 on the skin pass under the fur, and v5.x the fur's length there. They
// light in view space: R2 is the unit normal to the end, c83 the key light's direction from the light. oPos is
// c3..c6 (a perspective projection) divided by w, then scaled by c-38 and moved by c-37; the shading undoes that for
// the view space position. Registers in the HLSL are hardware order, D3D's + 96.
constexpr const char* kMatch = "mac_mul(((c[181]).xxxx), ((i.v5).xxxx))";

// They light the key light (oD1) as max(N.L, 0) (R1.y) times c84 less the blob shadows the program sums from 12 blobs
// (R10, c50..c61 placed by c63..c65), each strength / d^2 at the vertex and clipped at full shadow: over the tail's
// long triangles that steep falloff, interpolated, crawls through the creases as the body casts on it. The shading
// takes the shadows out of oD1 and the pixel shader puts them back per pixel, softer.
//
// x0: the normal, and the shell's height in the fur from 0 at the skin to 1 at the run's outermost shell;
// x1: the position, and the key light's share of the light the vertex takes
constexpr const char* kVs = R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.d1.xyz = saturate(R1.y * c[180].xyz);
        float key = dot(o.d1.xyz, 1), sum = key + dot(o.d0.xyz, 1);
        o.x0 = float4(R2.xyz, saturate(c[181].x * xc.x));
        o.x1 = float4(x, y, z, sum > 1e-4 ? key / sum : 0);
    })";

// the shells, over the colour the game lit them with, as a volume of fur the way Perlin's hypertexture lights one: the
// key light reaches a shell through the fur above it, (1 - h) deep along the normal and longer the more it slants in,
// and is let through by Beer's law with red going deepest, so the coat warms rather than greys toward the roots
// (40% of the light is let through regardless: the game's own lighting already darkens the side away from it); the
// tips, where the fur is nearly a surface, take a diffuse reflection of the light, and a soft highlight where the key
// light meets the strands edge on (Kajiya-Kay with the strands along the normal) and a rim where the surface turns
// from the eye. Each strand is rounded too: the alpha that cuts it out rises from the test's reference at its edge
// toward its middle, which makes a cross section, and the way alpha climbs on the screen points into the strand, so
// the strand is lit as a cylinder, its side toward the key light lighter, the other darker and its edges a little
// shaded, about as bright as before on the whole. The strands are thickened first: the test then cuts them where their
// alpha is 60% of its reference at the roots, 80% at the tips, so they taper, with alpha stretched from there to 1 over
// the reference to 1 so their middles blend as before. The skin pass under the fur only takes the blob shadows: the
// program's sum at the pixel, its edge eased, taking at most 40% of the key light's share of the colour
constexpr const char* kPs = R"({
    float h = i.x0.w;
    float ref = ((int)misc.y == 4 || (int)misc.y == 6) ? misc.x / 255 : 0;
    if (h > 0 && ref > 0) {
        float lo = ref * lerp(0.6, 0.8, h);
        r0.a = saturate(ref + (r0.a - lo) / (1 - lo) * (1 - ref));
    }
    float2 ag = float2(ddx(r0.a), ddy(r0.a));  // outside the branch: derivatives need the whole quad
    {
        float4 p = float4(i.x1.xyz, 1);
        float sx = dot(p, c[159]), sy = dot(p, c[160]), sz = dot(p, c[161]), blobs = 0;
        [unroll] for (int g = 0; g < 3; g++) {
            float4 dx = c[146 + 4 * g] - sx, dy = c[147 + 4 * g] - sy;
            blobs += dot((float4)(sz >= c[148 + 4 * g]) * c[149 + 4 * g], 1 / max(dx * dx + dy * dy, 1e-6));
        }
        blobs = smoothstep(0, 1, saturate(blobs));
        r0.rgb *= 1 - 0.4 * blobs * i.x1.w;
    }
    if (h > 0) {
        float3 n = normalize(i.x0.xyz), e = normalize(-i.x1.xyz), l = normalize(-c[179].xyz);
        float nl = dot(n, l), wrap = saturate(nl * 0.5 + 0.5);
        float thick = min((1 - h) / max(wrap, 0.4), 1.5);
        float3 trans = lerp(exp(-float3(0.14, 0.22, 0.32) * thick), 1, 0.4);
        float surface = h * h;
        surface *= surface;
        float th = dot(n, normalize(l + e));
        float strand = pow(saturate(1 - th * th), 24) * wrap;
        float rim = pow(1 - saturate(dot(n, e)), 3) * sqrt(wrap);
        float across = 1 - saturate((r0.a - ref) / max(1 - ref, 0.05));
        float agl = length(ag);
        float2 out2 = agl > 1e-5 ? -ag / agl : 0;  // screen space, y down
        float3 sn = float3(float2(out2.x, -out2.y) * across, sqrt(saturate(1 - across * across)));
        float vol = (0.85 + 0.3 * saturate(dot(normalize(sn), l) * 0.5 + 0.5)) * (1 - 0.1 * across * across);
        r0.rgb *= vol * (trans * (1 + surface * 0.25 * saturate(nl)) + h * (0.35 * strand + 0.3 * rim));
    }
})";

// The shells blend and write depth wherever they pass the alpha test, faint as a strand's edge may be, so a depth of
// field takes the backdrop seen through them for the fur's own depth and keeps it sharp: a band of it in focus
// around an in focus character, wide upscaled. Only the strands at least three quarters opaque write depth; the rest
// blend in over the backdrop's, which the depth of field blurs. The game's own shells (fur_shading off) take that
// alone. Only the shells: the skin pass under them (offset 0) writes its depth as drawn. XBE_FUR_DEPTH_ALPHA
// overrides the 0.75, for tuning: higher leaves more of a strand's edge to the blur.
constexpr float kDepthAlpha = 0.75f;
// Conker's head (its morph targets in v1..v5) draws its shells with a program of its own, the offset c90.x times the
// fur's length in v9; not lit by fur_shading (its registers differ), only its depth split
constexpr const char* kHeadMatch = "mac_mul(((c[186]).xxxx), ((i.v9).xxxx))";
// Berri's (two programs, her head and her body, Heist) offset by c90.x times the length in v5; their skin pass has c90
// 0 0 0 1. Missed, every strand of hers passing the alpha test wrote depth: a sharp aura around her
constexpr const char* kC90Match = "mac_mul(((c[186]).xxxx), ((i.v5).xxxx))";
const bool kAlways = true;

float DepthAlpha() {
    const char* e = std::getenv("XBE_FUR_DEPTH_ALPHA");
    return e ? float(std::atof(e)) : kDepthAlpha;
}

}  // namespace

void fur::Init() {
    const float depth_alpha = DepthAlpha();
    gpu::g_program_shading.push_back({.match = kMatch, .on = &g_shading, .vs = kVs, .ps = kPs, .top = 85 + 96,
                                      .between = 1, .soft = true, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kMatch, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 85 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kHeadMatch, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 90 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kC90Match, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 90 + 96, .depth_alpha = depth_alpha});
}
