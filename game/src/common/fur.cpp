#include "common/fur.h"

#include "gpu/shaders.h"

bool fur::g_shading = false;

namespace {

// The fur programs (four, 1B4CFC40E338AEDB and its variants) offset the skinned position along the normal by
// c85.x * v5.x: c85.x is the shell's offset, 0 on the skin pass under the fur, and v5.x the fur's length there. They
// light in view space: R2 is the unit normal to the end, c83 the key light's direction from the light. oPos is
// c3..c6 (a perspective projection) divided by w, then scaled by c-38 and moved by c-37; the shading undoes that for
// the view space position. Registers in the HLSL are hardware order, D3D's + 96.
constexpr const char* kMatch = "mac_mul(((c[181]).xxxx), ((i.v5).xxxx))";

// x0: the normal, and the shell's height in the fur from 0 at the skin to 1 at the frame's outermost shell;
// x1: the position
constexpr const char* kVs = R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.x0 = float4(R2.xyz, saturate(c[181].x * xc.x));
        o.x1 = float4(x, y, z, 0);
    })";

// the shells, over the colour the game lit them with: darker toward the roots (the fur shading itself), a highlight
// where the key light meets the strands edge on (Kajiya-Kay with the strands along the normal), and a rim where the
// surface turns from the eye, both toward the tips; the skin pass stays as it is
constexpr const char* kPs = R"({
    float h = i.x0.w;
    if (h > 0) {
        float3 n = normalize(i.x0.xyz), e = normalize(-i.x1.xyz), l = normalize(-c[179].xyz);
        float wrap = saturate(dot(n, l) * 0.5 + 0.5);
        float th = dot(n, normalize(l + e));
        float strand = pow(saturate(1 - th * th), 24) * wrap;
        float rim = pow(1 - saturate(dot(n, e)), 3) * wrap;
        r0.rgb *= lerp(0.6, 1, h) + h * (0.6 * strand + 0.35 * rim);
    }
})";

}  // namespace

void fur::Init() {
    gpu::g_program_shading.push_back(
        {.match = kMatch, .on = &g_shading, .vs = kVs, .ps = kPs, .top = 85 + 96, .soft = true});
}
