#include "common/glass.h"

#include "gpu/shaders.h"

bool glass::g_shading = false;

namespace {

// The rigid props' program with a sphere map: R10 the unit view space normal, oT1 = 0.5 * n.xy + 0.5 from c108/c109
// into the prop's 32x32 colour (bright in the middle, black at the rim), oT2 = 0.45 * n.xy + 0.5 from c112/c113 into
// the lighting map, as the character programs; oD0 the room's light at n, a quadratic in n per channel (c166-c169 red,
// c170-c173 green, c174-c177 blue), c179 the key light's direction (negated) and c180 its colour. Its pixel shaders
// add the diffuse, a tenth of the sphere map and the lighting map, and scale the sum by 4. The tavern's beer bottles,
// green and amber, are drawn so; one held in a hand that moves (the amber one by the candle) is drawn by the skinned
// variant instead: R2 the normal, the sphere map from c162/c163 into oT1 and the lighting map from c181/c182 into oT2,
// the rest alike. Other props are drawn so too, Conker's golden crown in the intro among them, so only the bottles'
// two sphere maps take it. The shading undoes oPos's divide and viewport for the view space position. Registers are
// hardware order.
constexpr const char* kMatchRigid = "dot(((R10).xyzw), ((c[108]).xyzw))";
constexpr uint32_t kBottles[] = {0x824D4280, 0x824CD200, 0};  // green and amber, the menu's
constexpr const char* kMatchSkinned = R"(dot(((R2).xyzw), ((c[163]).xyzw)).xxxx;
        oT1.y = mr.y;
    }
    {
        precise float4 mr = ((i.v4).wwww);
        R2.w = mr.w;
    }
    {
        precise float4 mr = dot(((R2).xyzw), ((c[181]).xyzw)))";

// The tavern's pints are props of the lightmapped program that also reflects a map along the view (t0, oT0 from
// c108..c110) and looks the lighting map up by the normal (oT3 from c117/c118, R10 the normal, w its v1.w): t1 the
// painted pint, t2 the room's lightmap; the pixel shader adds the reflection, the colour times the lightmap and the
// lighting map. Much of the bar is drawn by it, so only the pints' textures take it: the lager (empty glass at the top,
// the foam a white band in the middle, the beer below) and the stout (its head at the top, the one by the Grim Reaper
// too); and the spirits hanging behind the bar, a clear one with a round label and a whisky with three, both on a
// metal band at the bottom of their textures. The textures are the menu's, at the same addresses each run.
constexpr const char* kMatchPint = R"(dot(((R10).xyzw), ((c[117]).xyzw)).xxxx;
        oT3.x)";
constexpr uint32_t kLager[] = {0x81940580, 0};
constexpr uint32_t kStout[] = {0x8193DA80, 0};
constexpr uint32_t kClearSpirit[] = {0x81943080, 0};
constexpr uint32_t kWhisky[] = {0x81945B80, 0};

// the room's light arriving from d, as the program's oD0 lights a normal d, in the final colour's scale; GlassMap,
// the lighting map's coordinates for a direction d, w the normal's
#define GLASS_ROOM R"(
float3 GlassRoom(float3 d) {
    float4 q = float4(d, 1);
    float3 e = float3(dot(q, float4(dot(q, c[166]), dot(q, c[167]), dot(q, c[168]), dot(q, c[169]))),
                      dot(q, float4(dot(q, c[170]), dot(q, c[171]), dot(q, c[172]), dot(q, c[173]))),
                      dot(q, float4(dot(q, c[174]), dot(q, c[175]), dot(q, c[176]), dot(q, c[177]))));
    return max(e, 0) * 4 / 3.14159;
}
)"
constexpr const char* kFunctionsRigid = GLASS_ROOM R"(
float2 GlassMap(float3 d, float w) { return float2(dot(float4(d, w), c[112]), dot(float4(d, w), c[113])); }
)";
constexpr const char* kFunctionsSkinned = GLASS_ROOM R"(
float2 GlassMap(float3 d, float w) { return float2(dot(float4(d, w), c[181]), dot(float4(d, w), c[182])); }
)";
constexpr const char* kFunctionsPint = R"(
float2 GlassMap(float3 d, float w) { return float2(dot(float4(d, w), c[117]), dot(float4(d, w), c[118])); }
// 1 inside the box r of a 128x128 texture's texels (x0, y0, x1, y1), uv wrapped; GlassOval inside the oval in it;
// GlassRow the texel row
float GlassIn(float2 uv, float4 r) { float2 t = frac(uv) * 128; return all(t >= r.xy) && all(t <= r.zw + 1) ? 1 : 0; }
float GlassOval(float2 uv, float4 r) {
    float2 t = frac(uv) * 128, rad = (r.zw + 1 - r.xy) / 2;
    return length((t - r.xy - rad) / rad) <= 1 ? 1 : 0;
}
float GlassRow(float2 uv) { return frac(uv.y) * 128; }
)";

#undef GLASS_ROOM

// x0: the normal, and the w the lighting map's lookup takes; x1: the position
#define GLASS_VS(normal) R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.x0 = )" normal R"(;
        o.x1 = float4(x, y, z, 1);
    })"
constexpr const char* kVsRigid = GLASS_VS("R10");
constexpr const char* kVsSkinned = GLASS_VS("R2");
#undef GLASS_VS

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
    float2 ruv = GlassMap(r, i.x0.w) + texsize[2].zw;

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

// The painted pint as the game lit it, where see is above 0 seen through: what was drawn behind, bent as by a
// cylinder of glass and liquid, clear where the glass is empty and through the beer where there is beer, beer the
// colour that leaves it. Over it all a layer of glass: Fresnel weighted, the lighting map's lights along the reflected
// view, and a sharp highlight from a light above the glass and off to one side of the viewer, which on a pint is the
// bright streak down its side; the glass's up is the model's, its y through the view matrix c96..c98
#define GLASS_PINT(liquid, see, beer) R"({
    float3 paint = t1.rgb;
    float2 uv = tc1.xy / tc1.w;
    float hi = max(paint.r, max(paint.g, paint.b)), lo = min(paint.r, min(paint.g, paint.b));
    float sat = (hi - lo) / max(hi, 1e-3);
    float foam = smoothstep(0.86, 0.94, hi) * (1 - smoothstep(0.1, 0.2, sat));
    float liquid = )" liquid R"(;
    float see = )" see R"(;
    float3 tint = saturate(paint / max(hi, 1e-3));

    float3 n = normalize(i.x0.xyz), v = normalize(-i.x1.xyz);
    float3 nx = ddx(n), ny = ddy(n);
    float nv = max(dot(n, v), 1e-3);
    float3 up = normalize(float3(c[96].y, c[97].y, c[98].y)), side = normalize(cross(up, v) + 1e-4);
    float3 l = normalize(up + 0.6 * v + 0.5 * side), r = reflect(-v, n);

    float2 size;
    xscreen.GetDimensions(size.x, size.y);
    float2 bent = i.pos.xy + float2(-n.x, n.y) * 0.03 * size.y;
    float3 behind = xscreen.SampleLevel(xscreen_smp, bent / size, 0).rgb;
    float3 beer = )" beer R"(;
    float3 body = lerp(r0.rgb, lerp(behind * 0.95, beer, liquid), see);

    float a2 = min(0.0006 + 2 * max(dot(nx, nx), dot(ny, ny)), 0.2);
    float3 h = normalize(l + v);
    float nl = saturate(dot(n, l)), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float dd = nh * nh * (a2 - 1) + 1, k = sqrt(a2) * 0.5;
    float vis = 0.25 / ((nl * (1 - k) + k) * (nv * (1 - k) + k));
    float spec = min(a2 / (3.14159 * dd * dd) * vis, 64) * (0.04 + 0.96 * pow(1 - vh, 5)) * nl;

    float f = 0.04 + 0.96 * pow(1 - nv, 5);
    float3 shine = tex3.Sample(smp3, GlassMap(r, i.x0.w) + texsize[3].zw).rgb * 8;
    r0.rgb = body * (1 - f) + shine * f + spec * 0.6;
})"
// the lager: beer where the hue is saturated, the empty glass above the foam all but clear, the beer nearly so, in
// its own hue; the stout: beer all below the head, what is behind dimmed to a dark ruby, as stout held to the light
constexpr const char* kPsLager = GLASS_PINT("smoothstep(0.35, 0.6, sat)", "(1 - foam) * lerp(0.93, 0.88, liquid)",
                                            "behind * pow(tint, 0.4 + 0.5 / (0.3 + nv)) * hi + r0.rgb * 0.15");
constexpr const char* kPsStout = GLASS_PINT("1 - foam", "(1 - foam) * 0.7",
                                            "behind * float3(0.45, 0.2, 0.08) + r0.rgb * 0.3");
// the spirits: clear but for their labels and the metal band; the whisky between rows 36 and 108 of its texture, in
// its amber (its painted reflections, paler, would otherwise read as glass)
constexpr const char* kPsClearSpirit = GLASS_PINT(
    "0", "0.9 * (1 - max(GlassOval(uv, float4(52, 20, 82, 50)), GlassIn(uv, float4(0, 102, 127, 127))))", "behind");
constexpr const char* kPsWhisky = GLASS_PINT(
    "smoothstep(34, 40, GlassRow(uv)) * (1 - smoothstep(106, 110, GlassRow(uv)))",
    "lerp(0.9, 0.85, liquid) * (1 - max(max(GlassIn(uv, float4(52, 18, 76, 54)), "
    "GlassIn(uv, float4(0, 110, 127, 127))), max(GlassIn(uv, float4(0, 13, 12, 62)), "
    "GlassIn(uv, float4(116, 13, 127, 62)))))",
    "behind * pow(float3(1, 0.67, 0.16), 0.4 + 0.5 / (0.3 + nv)) * 0.8 + r0.rgb * 0.15");
#undef GLASS_PINT

}  // namespace

void glass::Init() {
    gpu::g_program_shading.push_back({.match = kMatchRigid, .on = &g_shading, .vs = kVsRigid, .ps = kPs,
                                      .functions = kFunctionsRigid, .screen = true, .textures = kBottles,
                                      .texture_stage = 1});
    gpu::g_program_shading.push_back({.match = kMatchSkinned, .on = &g_shading, .vs = kVsSkinned, .ps = kPs,
                                      .functions = kFunctionsSkinned, .screen = true, .textures = kBottles,
                                      .texture_stage = 1});
    gpu::g_program_shading.push_back({.match = kMatchPint, .on = &g_shading, .vs = kVsRigid, .ps = kPsLager,
                                      .functions = kFunctionsPint, .screen = true, .textures = kLager,
                                      .texture_stage = 1});
    gpu::g_program_shading.push_back({.match = kMatchPint, .on = &g_shading, .vs = kVsRigid, .ps = kPsStout,
                                      .functions = kFunctionsPint, .screen = true, .textures = kStout,
                                      .texture_stage = 1});
    gpu::g_program_shading.push_back({.match = kMatchPint, .on = &g_shading, .vs = kVsRigid, .ps = kPsClearSpirit,
                                      .functions = kFunctionsPint, .screen = true, .textures = kClearSpirit,
                                      .texture_stage = 1});
    gpu::g_program_shading.push_back({.match = kMatchPint, .on = &g_shading, .vs = kVsRigid, .ps = kPsWhisky,
                                      .functions = kFunctionsPint, .screen = true, .textures = kWhisky,
                                      .texture_stage = 1});
}
