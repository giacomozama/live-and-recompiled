#include "common/glass.h"

#include "gpu/shaders.h"

bool glass::g_shading = false;

namespace {

// The rigid props' program with a sphere map: R10 the unit view space normal, oT1 = 0.5 * n.xy + 0.5 from c108/c109
// into the prop's 32x32 colour (bright in the middle, black at the rim), oT2 = 0.45 * n.xy + 0.5 from c112/c113 into
// the lighting map, as the character programs; oD0 the room's light at n, a quadratic in n per channel (c166-c169 red,
// c170-c173 green, c174-c177 blue), c179 the key light's direction (negated) and c180 its colour. Its pixel shaders
// add the diffuse, a tenth of the sphere map and the lighting map, and scale the sum by 4. The tavern's beer bottles,
// green and amber, are drawn so. As metal.cpp, the shading undoes oPos's divide and viewport for the view space
// position. Registers are hardware order.
constexpr const char* kMatch = "dot(((R10).xyzw), ((c[108]).xyzw))";

// the room's light arriving from d, as the program's oD0 lights a normal d, in the final colour's scale
constexpr const char* kFunctions = R"(
float3 GlassRoom(float3 d) {
    float4 q = float4(d, 1);
    float3 e = float3(dot(q, float4(dot(q, c[166]), dot(q, c[167]), dot(q, c[168]), dot(q, c[169]))),
                      dot(q, float4(dot(q, c[170]), dot(q, c[171]), dot(q, c[172]), dot(q, c[173]))),
                      dot(q, float4(dot(q, c[174]), dot(q, c[175]), dot(q, c[176]), dot(q, c[177]))));
    return max(e, 0) * 4 / 3.14159;
}
)";

// x0: the normal, and the w the lighting map's lookup takes; x1: the position
constexpr const char* kVs = R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.x0 = R10;
        o.x1 = float4(x, y, z, 1);
    })";

// The sphere map's middle gives the glass's hue; grey maps (chrome) stay as the game drew them. Through the glass is
// what was drawn behind it, the screen copy read bent the way a full bottle, a lens, bends it: flipped, more so toward
// the edges, and lit from behind where the key light is there too. The glass and what it holds absorb by the tint,
// the more the longer the path, so the edges darken and deepen in colour. Over that, Fresnel weighted, the reflection
// of the room and of the lighting map's lights, brighter than the room they light, and the key light's GGX highlight,
// sharp, widened where the normal varies over a pixel so it doesn't sparkle
constexpr const char* kPs = R"({
    float3 tint = tex1.SampleLevel(smp1, float2(0.5, 0.5) + texsize[1].zw, 0).rgb;
    float hi = max(tint.r, max(tint.g, tint.b)), lo = min(tint.r, min(tint.g, tint.b));
    float glass = smoothstep(0.35, 0.6, (hi - lo) / max(hi, 1e-3));
    tint = saturate(tint / max(hi, 1e-3));

    float3 n = normalize(i.x0.xyz), v = normalize(-i.x1.xyz);
    float3 nx = ddx(n), ny = ddy(n);
    float nv = max(dot(n, v), 1e-3);
    float3 t = refract(-v, n, 1 / 1.5), r = reflect(-v, n), l = normalize(-c[179].xyz), key = c[180].xyz * 4;
    float2 ruv = float2(dot(float4(r, i.x0.w), c[112]), dot(float4(r, i.x0.w), c[113])) + texsize[2].zw;

    float2 size;
    xscreen.GetDimensions(size.x, size.y);
    float2 bent = i.pos.xy + float2(-n.x, n.y) * 0.03 * size.y;
    float3 behind = xscreen.SampleLevel(xscreen_smp, bent / size, 0).rgb + key * pow(saturate(dot(t, l)), 4);
    float3 absorb = pow(tint, 0.4 + 0.6 / (0.3 + nv)) * 0.9;
    float3 room = GlassRoom(r) + tex2.Sample(smp2, ruv).rgb * 8;

    float a2 = min(0.0004 + 2 * max(dot(nx, nx), dot(ny, ny)), 0.2);
    float3 h = normalize(l + v);
    float nl = saturate(dot(n, l)), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float dd = nh * nh * (a2 - 1) + 1, k = sqrt(a2) * 0.5;
    float vis = 0.25 / ((nl * (1 - k) + k) * (nv * (1 - k) + k));
    float3 spec = min(a2 / (3.14159 * dd * dd) * vis, 64) * (0.04 + 0.96 * pow(1 - vh, 5)) * nl * key;

    float f = 0.04 + 0.96 * pow(1 - nv, 5);
    r0.rgb = lerp(r0.rgb, behind * absorb * (1 - f) + room * f + spec, glass);
})";

}  // namespace

void glass::Init() {
    gpu::g_program_shading.push_back(
        {.match = kMatch, .on = &g_shading, .vs = kVs, .ps = kPs, .functions = kFunctions, .screen = true});
}
