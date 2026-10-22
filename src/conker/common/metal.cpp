#include "common/metal.h"

#include "gpu/shaders.h"

bool metal::g_shading = false;

namespace {

// The character programs skin to view space and light alike (docs/conker.md); the shiny materials' variant also looks
// the unit normal R2 up in the character's lighting map, 32x32, which the engine renders itself: oT2 = 0.45 * n.xy +
// 0.5 from c162/c163. Its pixel shaders add that map's colour times a mask from the texture's brightness (t1, the
// colour; pc2 the weights) times the key light's map (t0), and scale the sum by 4. The goblins' armour is drawn so. As
// fur.cpp, the shading undoes oPos's divide and viewport for the view space position. Registers are hardware order.
constexpr const char* kMatch = "dot(((R2).xyzw), ((c[162]).xyzw))";
// Most characters are drawn so too, and their white or bone-coloured parts pass for metal: the menu's male bartender
// (his shirt) and Grim Reaper (his skull) keep the game's shading, their atlases at the same addresses each run
constexpr uint32_t kNotMetal[] = {0x81F3FD80, 0x820A0700, 0};

// x0: the normal, and the w the lighting map's lookup takes; x1: the position
constexpr const char* kVs = R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.x0 = R2;
        o.x1 = float4(x, y, z, 1);
    })";

// Over the colour the game lit, its metal shaded the way a physically based renderer would: texels grey and bright
// enough are metal, more so the less saturated they are (the goblins' green skin stays as it was), their colour the
// reflectance at normal incidence, smoother the brighter, and their share of the game's diffuse light mostly taken
// away. The normal is bent by the texture's brightness, read a texel (or a pixel's footprint) either side, as a height
// along the surface's own texture axes, so rivets and dents stand out; how much the normal varies over a pixel widens
// the highlight so it doesn't sparkle. The key light, through its map, takes a GGX highlight with Smith visibility and
// Schlick's Fresnel; the character's lighting map, looked up along the reflected view instead of the normal, gives the
// reflection of its lights, Fresnel weighted, so it slides over the plates as they turn
constexpr const char* kPs = R"({
    const float3 lumw = float3(0.3, 0.59, 0.11);
    float3 p = i.x1.xyz, dpx = ddx(p), dpy = ddy(p);
    float2 uv = tc1.xy / tc1.w + texsize[1].zw, dux = ddx(uv), duy = ddy(uv);
    float foot = max(max(length(dux * texsize[1].xy), length(duy * texsize[1].xy)), 1);
    float2 su = float2(foot / texsize[1].x, 0), sv = float2(0, foot / texsize[1].y);
    float hu = dot(tex1.SampleGrad(smp1, uv + su, dux, duy).rgb - tex1.SampleGrad(smp1, uv - su, dux, duy).rgb, lumw);
    float hv = dot(tex1.SampleGrad(smp1, uv + sv, dux, duy).rgb - tex1.SampleGrad(smp1, uv - sv, dux, duy).rgb, lumw);
    float3 n = normalize(i.x0.xyz);
    float3 cy = cross(dpy, n), cx = cross(n, dpx);
    float3 tu = cy * dux.x + cx * duy.x, tv = cy * dux.y + cx * duy.y;
    tu *= rsqrt(max(dot(tu, tu), 1e-20));
    tv *= rsqrt(max(dot(tv, tv), 1e-20));
    float2 g = clamp(float2(hu, hv) * 2 / foot, -0.5, 0.5);  // the slope: 4 x the brightness's change a texel
    float3 nb = normalize(n - g.x * tu - g.y * tv);
    float3 nbx = ddx(nb), nby = ddy(nb);  // outside any branch: derivatives need the whole quad

    float3 a = t1.rgb;
    float hi = max(a.r, max(a.g, a.b)), lo = min(a.r, min(a.g, a.b)), lum = dot(a, lumw);
    float m = smoothstep(0.06, 0.25, hi) * (1 - smoothstep(0.2, 0.45, (hi - lo) / max(hi, 1e-3)));
    float rough = lerp(0.65, 0.38, smoothstep(0.1, 0.5, lum));
    float a2 = min(rough * rough * rough * rough + min(2 * max(dot(nbx, nbx), dot(nby, nby)), 0.2), 1);
    float3 v = normalize(-p), l = normalize(-c[179].xyz), h = normalize(l + v);
    float nl = saturate(dot(nb, l)), nv = max(dot(nb, v), 1e-3), nh = saturate(dot(nb, h)), vh = saturate(dot(v, h));
    float3 f0 = lerp(0.04, saturate(a * 1.8 + 0.1), m);
    float dd = nh * nh * (a2 - 1) + 1, k = sqrt(a2) * 0.5;
    float vis = 0.25 / ((nl * (1 - k) + k) * (nv * (1 - k) + k));
    float3 spec = min(a2 / (3.14159 * dd * dd) * vis, 4) * (f0 + (1 - f0) * pow(1 - vh, 5)) * nl * c[180].xyz * 4 * t0.rgb;
    float3 r = reflect(-v, nb);
    float2 ruv = float2(dot(float4(r, i.x0.w), c[162]), dot(float4(r, i.x0.w), c[163])) + texsize[2].zw;
    float3 fe = f0 + (max(1 - sqrt(a2), f0) - f0) * pow(1 - nv, 5);
    r0.rgb = r0.rgb * (1 - 0.3 * m) + tex2.Sample(smp2, ruv).rgb * fe + 0.5 * spec;
})";

}  // namespace

void metal::Init() {
    gpu::g_program_shading.push_back({.match = kMatch, .on = &g_shading, .vs = kVs, .ps = kPs, .textures = kNotMetal,
                                      .texture_stage = 1, .skip_textures = true});
}
