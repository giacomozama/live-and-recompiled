#include "common/fur.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#endif

#include "core/log.h"
#include "gpu/shaders.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

bool fur::g_shading = false;
bool fur::g_strands = false;
fur::ModelLook fur::g_model_looks[fur::kModelLooks];
uint32_t fur::g_extra_shells = fur::kExtraShells;

namespace {

// The fur programs (four, 1B4CFC40E338AEDB and its variants) offset the skinned position along the normal by
// c85.x * v5.x: c85.x is the shell's offset, 0 on the skin pass under the fur, and v5.x the fur's length there. They
// light in view space: R2 is the unit normal to the end, c83 the key light's direction from the light. oPos is
// c3..c6 (a perspective projection) divided by w, then scaled by c-38 and moved by c-37; the shading undoes that for
// the view space position. Registers in the HLSL are hardware order, D3D's + 96.
constexpr const char* kMatch = "mac_mul(((c[181]).xxxx), ((i.v5).xxxx))";

// Of them the little girl squirrel's two scale the color by the vertex color, R9 * v0, as no other furry
// character's do: the shading left patches on her face, so her shells keep the game's own look and only the depth split.
// XBE_FUR_SHADE_ALL=1 shades hers too, to compare on a character that may share her programs
constexpr const char* kGirlSkip = "mac_mul(((R9).xyzw), ((i.v0).xxxx))";

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

// the shells, over the color the game lit them with, as a volume of fur the way Perlin's hypertexture lights one: the
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
// program's sum at the pixel, each blob coming in across its radius about the plane it clips at, as per-pixel
// lighting's do (a hard step there left a solid edge across a cheek), taking at most 40% of the key light's share
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
            float4 dx = c[146 + 4 * g] - sx, dy = c[147 + 4 * g] - sy, st = c[149 + 4 * g];
            float4 above = smoothstep(-1, 1, (sz - c[148 + 4 * g]) / max(sqrt(abs(st)), 1e-4));
            blobs += dot(above * st, 1 / max(dx * dx + dy * dy, 1e-6));
        }
        float lit = saturate(1 - 0.5 * blobs);  // the knee per-pixel lighting takes (lighting.cpp LightShadow)
        r0.rgb *= 1 - 0.4 * (1 - lit * lit) * i.x1.w;
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
// Conker's in the Heist suit (three of the four pieces of each shell; the fourth is the body's program): the head's
// program, its normal in R6 and length in v9, with the body's c85 for its offset
constexpr const char* kC85V9Match = "mac_mul(((c[181]).xxxx), ((i.v9).xxxx))";
// the weasels' (Heist and elsewhere): the body's program, shells offset by c94.x times the length in v5; a fur map on
// stage 2 (64x128, white over the furry pieces), the masks on 3
constexpr const char* kC94Match = "mac_mul(((c[190]).xxxx), ((i.v5).xxxx))";
// the Tediz Demolisher's in multiplayer (its character select model, characters::g_menu_models; F3 capture 003050,
// 2026-11-10): the skinned position (R10) moved along the normal (R2) by c85.x alone, no length per vertex, so the coat
// is even; the mask on stage 2, 8 of 16 shells. Per-pixel lighting took its draws before (one of its lit shapes)
constexpr const char* kTediMatch = "(mac_mul(((c[181]).xxxx), ((R2).xyzz)) + ((R10).xyzz))";
const bool kAlways = true;
// fur::TagShells' types for the chipmunks (ChipmunkTypes)
// from F3 captures 2026-11-09: the female bartender 398, Berri 114, the little girl 124, the two Sneekers (one each
// texture, teams likely) 445 and 461. The grey squirrels sharing their programs: 88 (no helmet), 181, 299
// Berri in the Heist's suit is 151, a type of her own
constexpr uint32_t kChipmunkTypes[] = {398, 114, 124, 445, 461, 151};
constexpr size_t kChipmunkCount = std::size(kChipmunkTypes);
// the weasels drawn by programs other than theirs (kC94Match, FUR_MAP_STRANDS): the Heist's 134, Berri's program and
// no fur map, its hands and feet of length 0
constexpr uint32_t kWeaselTypes[] = {134};
// drawn as the game draws them, super fur or not (furOff): the female bartender and the little girl. Strands on their
// soft fur maps looked bad (2026-11-09); Berri and the Sneekers keep the chipmunks' look
constexpr uint32_t kConsoleTypes[] = {398, 124};
// the models with a piece of hair among their shells (fur::HairOf), told from their other pieces drawn by the same
// program by the shell's offset per layer (c85.x over its layer + 1), at most most on the hair: Van Helsing's (276,
// F3 capture 192613: the hair 0.179, by the body's program; his tail, the same program and tag, 0.596; body and head
// by Berri's)
struct HairPiece {
    uint32_t model;
    float most;
};
constexpr HairPiece kHairPieces[] = {{276, 0.3f}};

// fur_strands, shown as Super fur. The game's masks (a 64x64 DXT3 per shell, which alpha is a stack of some 55 fat round pegs to a tile, and
// which grey climbs from 0.70 at the roots to 0.91 at the tips) can't be thinner or longer than they were drawn, and
// the shells stand straight along the normal. Here the mask is worked out for the height of the shell, so the shells
// the renderer adds between the game's are as good as its own:
//
// Tufts are a Voronoi of the mask's coordinates. A strand's root is where a point of the shell came from when the
// tuft's strands gather toward its middle as they rise, each tuft leaning its own way; the tuft is longest there and
// shorter toward its edge, so it ends in a point. The strands are cones on a jittered grid of those roots, of random
// lengths, and past what the screen resolves they give way to a solid coat as tall as the strands' denser half.
//
// Both the head's program and the body's light the same way and have the same blob registers; they differ in the
// register their normal ends in and the constants of their length, FUR_* below.
//
// Each kind of fur its own look (fur::Look), three float4 of xs each, FurLook picks one into furA..furC: furA droop,
// tufting, thickness, taper; furB strands, tufts across a tile, root shade, gap shade; furC sheen, glow, length, fill.
// The weasels' program is FUR_MAP_STRANDS; the chipmunks share their programs with the grey squirrels and are told
// apart by who the game draws (fur::TagShells), their types in xs[12] and xs[13]; the weasels drawn by other
// programs (the Heist's) by theirs in xs[14]; those left the game's own fur by theirs in xs[9]; a hair piece left the
// game's own fur (fur::HairOf) by xs[11]
constexpr const char* kStrandFunctions = R"(
static float4 furA, furB, furC;
static bool furOff;  // the game's own fur: its masks, color and light (the types in xs[9])
void FurLook(int k) {
    furA = xs[3 * k];
    furB = xs[3 * k + 1];
    furC = xs[3 * k + 2];
}
int FurLookOf() {
#ifdef FUR_MAP_STRANDS
    return 1;
#else
    float4 tag = c[2];  // fur::kTagRegister: type, its byte at 0xA1, level, 1 once tagged
    bool chipmunk = tag.w == 1 && (any(xs[12] == tag.x) || any(xs[13] == tag.x));
    bool weasel = tag.w == 1 && any(xs[14] == tag.x);
    bool off = tag.w == 1 && any(xs[9] == tag.x);
    return off ? -1 : weasel ? 1 : chipmunk ? 2 : 0;
#endif
}
// hairy: the program a model's hair is drawn by (the body's, kMatch); its other programs' pieces may have the same
// offset per layer (Van Helsing's body and head, by Berri's). The hair keeps the game's own fur when its look asks
// (xs[11]: x the least xc.x of the hair piece, which the shell's offset per layer sets; z 1 when it asks)
void FurPick(bool hairy) {
    int k = FurLookOf();
    if (hairy && xs[11].z > 0 && xc.x > xs[11].x) k = -1;
    furOff = k < 0;
    FurLook(max(k, 0));
}
float2 FurHash(float2 p) {
    float3 a = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
    a += dot(a, a.yzx + 33.33);
    return frac((a.xx + a.yz) * a.zy);
}
float FurStrands(float2 uv, float h, float du, out float along, out float gap, out float seed, out float across) {
    along = 1;
    seed = 0.5;
    across = 0;
    float2 cu = uv * furB.y, base = floor(cu), center = 0, nearest = 0;
    float best = 9;
    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
        float2 id = base + float2(x, y), at = id + 0.15 + 0.7 * FurHash(id), v = cu - at;
        float d = dot(v, v);
        if (d < best) {
            best = d;
            center = at;
            nearest = id;
        }
    }
    float2 tuft = FurHash(nearest + 17.3);  // the winner's alone: unrolled, a hash in the branch ran for all nine
    float reach = lerp(0.85, 1, tuft.x), rise = saturate(h / reach);
    float pull = min(furA.y * rise * sqrt(rise), 0.85);
    float2 root = (cu - center - (tuft.yx - 0.5) * 1.1 * h * h) / (1 - pull);
    float dc = length(root);
    gap = saturate(dc / 0.75);
    gap = lerp(gap, 0.6, saturate(du * furB.y * 3 - 0.5));  // tufts under a few pixels: their crevices would crawl
    float top = reach * (1 - 0.3 * smoothstep(0.15, 0.85, dc));
    float per = furB.x / furB.y;
    float2 su = (root + center) * per, sb = floor(su);
    float blur = max(du * furB.x / (1 - pull), 1e-3), cover = 0;
    float toward = saturate(blur / 0.7 - 0.7);  // how far the strands have given way to the coat below
    // the strands' own coverage, where some can reach (no strand is taller than its tuft's top) and the screen still
    // shows them. The four cells nearest the point, not nine: a cell's root lies 0.2 to 0.8 across it, so one in the
    // far row is 0.7 off or more, which only a strand's thick lower part spans, where the near ones cover it anyway
    [branch] if (h < top && toward < 1) {
        float2 sb0 = sb - (float2)(su - sb < 0.5);
        [unroll] for (int y = 0; y <= 1; y++) [unroll] for (int x = 0; x <= 1; x++) {
            float2 id = sb0 + float2(x, y), r = FurHash(id), q = FurHash(id + 9.1);
            float len = top * lerp(0.72, 1, q.x);
            if (h >= len) continue;
            float t = h / len;
            float rad = furA.z * lerp(0.8, 1.2, q.y) * pow(1 - t, furA.w);
            float d = length(su - id - 0.2 - 0.6 * r);
            float c = saturate((rad - d) / blur + 0.5);
            if (c > cover) {
                cover = c;
                along = t;
                seed = q.y;
                across = saturate(d / max(rad, 1e-3));
            }
        }
    }
    // past what the screen resolves (a small or distant character), the strands' coverage: solid low down, thinning
    // over the top two thirds by each tuft's own height and a coarser clump's, so tufts keep their points and the edge
    // stays ragged. The plain average (part covered at every height, a third of the strands') left each of some 40
    // shells see-through, a hazy halo; a solid coat cut at one height made a distant tail a solid lump
    float rough = FurHash(floor(su * 0.34) + 41.7).x - 0.5;  // clumps of a few strands, still a pixel or so wide
    float far = 1 - smoothstep(0.3, 0.95, h / top + 0.25 * rough);
    // which strand a pixel lands on changes with the slightest move once a strand is a pixel or two across: its root's
    // darkness, edge and tint go to their averages first, before the strands give way, or they flicker as shadows
    float even = saturate(blur * 2.5 - 0.5);
    along = lerp(along, saturate(h / top), even);
    across = lerp(across, 0.5, even);
    seed = lerp(seed, 0.5, even);
    return lerp(cover, far, toward);
}
)";

// As kVs, and the shell hangs: its position moves along the surface, toward the view's down (the camera seldom rolls),
// by its height times its offset, so a strand curves. Only across the view, not in depth: hanging back from the eye
// put an outer shell at about the depth of the one under it, whose opaque strands write depth, and the depth test
// then let it through or not pixel by pixel, a speckle that flickered on a face tilted toward the camera. The skin
// stays as the program drew it, bit for bit
constexpr const char* kStrandVs = R"(
#ifndef FUR_HAIRY
#define FUR_HAIRY 0
#endif
    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float x = (clip.x - c[99].z * z - c[99].w) / c[99].x, y = (clip.y - c[100].z * z - c[100].w) / c[100].y;
        o.d1.xyz = saturate(R1.y * c[180].xyz);
        float key = dot(o.d1.xyz, 1), sum = key + dot(o.d0.xyz, 1);
        float3 n = normalize(FUR_NORMAL.xyz), v = float3(x, y, z);
        float hh = saturate(FUR_TOP * xc.x);
        FurPick(FUR_HAIRY);
        // bare skin (a vertex of length 0: the weasels' hands and feet, Berri's face) grown to the look's fill of half
        // the longest, out along the normal as the program's own offset: in depth too, outward, so a full projection
        float len = FUR_LENGTH, grow = hh > 0 && !furOff ? max(0.5 * furC.w - len, 0) : 0;
        if (grow > 0) {
            v += n * FUR_TOP * grow;
            len += grow;
        }
        bool hangs = hh > 0 && furA.x > 0 && !furOff;
        if (hangs) {
            float3 g = float3(0, -1, 0);
            float3 hang = (g - n * dot(g, n)) * FUR_TOP * len * hh * furA.x;
            v.xy += hang.xy;  // across the view only: each shell keeps the game's depth, in front of the one under it
        }
        float pw = -v.z;
        if ((hangs || grow > 0) && pw > 1e-3) {
            float px = (c[99].x * v.x + c[99].z * v.z + c[99].w) / pw * c[58].x + c[59].x;
            float py = (c[100].y * v.y + c[100].z * v.z + c[100].w) / pw * c[58].y + c[59].y;
            if (grow > 0) {
                float4 at = float4(v, 1);
                float cw = magclamp(dot(at, c[102])), pz = dot(at, c[101]) / dot(at, c[102]) * c[58].z + c[59].z;
                o.pos = float4(float2(px * 2 / surf.x - 1, 1 - py * 2 / surf.y) * cw, pz * surf.z * cw, cw);
            } else {
                o.pos.xy = float2(px * 2 / surf.x - 1, 1 - py * 2 / surf.y) * o.pos.w;  // z and w the program's own
            }
        }
        o.x0 = float4(n, hh);
        o.x1 = float4(v, sum > 1e-4 ? key / sum : 0);
    })";

// The mask on stage 2 (64 square; a 4x4 white there is the game's own skin pass or no fur), or on stage 3 with a fur
// map on stage 2, whose blue the game multiplies in: the weasels' white where the coat is, the chipmunks' and grey
// squirrels' soft patches (face, legs; a stitched soldier's worn in places). The strands take the mask's place, shortened by the
// look's length and where the map is under half (its soft edges and worn spots short fuzz rather than a cut), and the
// map still multiplies in. Grey as the game's. furH is the height along the strands, for the lighting
constexpr const char* kStrandPre = R"(
#ifndef FUR_HAIRY
#define FUR_HAIRY 0
#endif
    float furAlong = 1, furGap = 0, furSeed = 0.5, furAcross = 0, furCover = 1, furH = i.x0.w;
    FurLook(0);
    furOff = false;
#if PS_PROGRAMMABLE && STAGE2_MODE == 1
    {
        bool two = texsize[2].x == 64, three = false;
#if STAGE3_MODE == 1
        three = texsize[3].x == 64;
#endif
        FurPick(FUR_HAIRY);
        float2 fuv = three ? tc3.xy : tc2.xy;
        float fdu = max(length(ddx(fuv)), length(ddy(fuv)));  // outside the branch: derivatives need the whole quad
        if ((two || three) && i.x0.w > 0 && !furOff) {
#if STAGE3_MODE == 1
            if (three) t2.b = lerp(t2.b, 1, furC.w);  // bare skin filled in as far as the look asks, the game's alpha too
#endif
            float reach = furC.z * (three ? saturate(t2.b * 2) : 1);
            furH = i.x0.w / max(reach, 1e-3);
            furCover = furH < 1 ? FurStrands(fuv, furH, fdu, furAlong, furGap, furSeed, furAcross) : 0;
            float4 strands = float4((float3)lerp(0.70, 0.91, saturate(furH)), furCover);
#if STAGE3_MODE == 1
            if (three) t3 = strands; else
#endif
            t2 = strands;
        }
    }
#endif
)";

// The light that reaches into the coat as in kPs, and on top: the roots and the crevices between tufts dark and more
// saturated (the light that gets there has crossed the fur's own color); each strand a little different, darker at its
// edge; two highlights as hair has (Scheuermann's shifted Kajiya-Kay), the first toward the root in the light's color,
// the second toward the tip, wider and in the fur's; and a light behind the fur coming through its edge, the thin
// tips strongest, in the light's strength and the fur's hue, not its lit color, which is dim on that side.
constexpr const char* kStrandPs = R"({
    float h = saturate(furH);
    // a pixel no strand covers fails the alpha test (10 or more) whatever its color: most of an outer shell's
    [branch] if (furCover * 255 >= 1) {
    {
        float4 p = float4(i.x1.xyz, 1);
        float sx = dot(p, c[159]), sy = dot(p, c[160]), sz = dot(p, c[161]), blobs = 0;
        [unroll] for (int g = 0; g < 3; g++) {
            float4 dx = c[146 + 4 * g] - sx, dy = c[147 + 4 * g] - sy, st = c[149 + 4 * g];
            float4 above = smoothstep(-1, 1, (sz - c[148 + 4 * g]) / max(sqrt(abs(st)), 1e-4));
            blobs += dot(above * st, 1 / max(dx * dx + dy * dy, 1e-6));
        }
        float lit = saturate(1 - 0.5 * blobs);  // the knee per-pixel lighting takes (lighting.cpp LightShadow)
        r0.rgb *= 1 - 0.4 * (1 - lit * lit) * i.x1.w;
    }
    if (h > 0 && furOff) {
        r0.a = 1 - pow(saturate(1 - r0.a), xs[10].x);  // the shells added between the game's: together as its one
    } else if (h > 0) {
        float3 n = normalize(i.x0.xyz), e = normalize(-i.x1.xyz), l = normalize(-c[179].xyz);
        float nl = dot(n, l), wrap = saturate(nl * 0.5 + 0.5);
        float thick = min((1 - h) / max(wrap, 0.4), 1.5);
        float3 trans = lerp(exp(-float3(0.14, 0.22, 0.32) * thick), 1, 0.4);
        float3 g = float3(0, -1, 0), t = normalize(n + 2 * h * furA.x * (g - n * dot(g, n)));
        float3 hv = normalize(l + e);
        float s1 = dot(normalize(t - 0.1 * n), hv), s2 = dot(normalize(t + 0.14 * n), hv);
        float first = pow(saturate(1 - s1 * s1), 60), second = pow(saturate(1 - s2 * s2), 14);
        float deep = saturate(max(1.7 * h, furAlong * furAlong));
        float ao = lerp(1 - furB.z, 1, deep) * (1 - furB.w * furGap * furGap * saturate(1 - 1.5 * h));
        float3 albedo = r0.rgb;
        float3 hue = albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 1e-3);
        r0.rgb = pow(max(albedo, 1e-4), lerp(1.3, 1, ao)) * lerp(trans, 1, deep * 0.6) * ao;
        r0.rgb *= (1 + (furSeed - 0.5) * 0.18) * (1 - 0.15 * furAcross * furAcross);
        float tips = h * sqrt(furAlong);
        float nv = 1 - saturate(dot(n, e)), rim = nv * nv * nv;
        float behind = pow(saturate(0.5 - 0.5 * dot(l, e)), 2) * (1 - 0.6 * saturate(nl));
        r0.rgb += hue * c[180].xyz * sqrt(h) * furC.y * behind * (0.25 + 1.5 * nv * nv);
        r0.rgb += albedo * tips * furC.y * 0.4 * rim * wrap;
        r0.rgb += tips * wrap * furC.x * c[180].xyz * (0.22 * first + 0.3 * hue * second);
    }
    }
})";

// the object types ([object+0x68]) drawn with the chipmunks' look: Berri, the female bartender, the warboss's little
// girl and multiplayer's SHC Sneeker. XBE_FUR_CHIPMUNK (types, comma separated, 0x for hex) in their place, to try
std::vector<float> ChipmunkTypes() {
    std::vector<float> out(std::begin(kChipmunkTypes), std::begin(kChipmunkTypes) + kChipmunkCount);
    if (const char* e = std::getenv("XBE_FUR_CHIPMUNK")) {
        out.clear();
        for (const char* p = e; *p && out.size() < 8;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 0);
            if (end == p) break;
            out.push_back(float(v));
            p = *end ? end + 1 : end;
        }
    }
    if (out.size() > 8) out.resize(8);
    return out;
}

// the types super fur's draws were tagged with since boot (SeenModels), and whether drawn by the weasels' program
// (FUR_MAP_STRANDS, whatever the type: the Weasels look). Render thread writes, the F1 menu reads
constexpr int kSeenMax = 128;
std::atomic<uint32_t> g_seen[kSeenMax];
std::atomic<bool> g_seen_weasel[kSeenMax];
std::atomic<bool> g_seen_drawn[kSeenMax];  // drawn this session, not only remembered from the settings
std::atomic<int> g_seen_count{0};

// XBE_FUR_SURVEY=1: each model's first draw with super fur this session logged, its type and name, to fill
// kFurryModels from
bool Survey() {
    static const bool on = [] {
        const char* e = std::getenv("XBE_FUR_SURVEY");
        return e && *e && *e != '0';
    }();
    return on;
}

void See(uint32_t type, bool weasel, bool drawn) {
    int n = std::min(g_seen_count.load(std::memory_order_acquire), kSeenMax), i = 0;
    while (i < n && g_seen[i].load(std::memory_order_relaxed) != type) i++;
    if (i == n) {
        if (n >= kSeenMax) return;
        g_seen[n].store(type, std::memory_order_relaxed);
        g_seen_weasel[n].store(false, std::memory_order_relaxed);
        g_seen_drawn[n].store(false, std::memory_order_relaxed);
        g_seen_count.store(n + 1, std::memory_order_release);
    }
    if (weasel) g_seen_weasel[i].store(true, std::memory_order_relaxed);
    if (drawn && !g_seen_drawn[i].exchange(true, std::memory_order_relaxed) && Survey())
        LOGR("fur survey: model %u \"%s\"%s", type, fur::ModelName(type).c_str(), weasel ? ", the weasels' program" : "");
}

void PutLook(float (*xs)[4], int k, const fur::Look& l) {
    const auto pc = [](uint32_t v) { return float(v) / 100.0f; };
    float* a = xs[3 * k];
    float* b = xs[3 * k + 1];
    float* c = xs[3 * k + 2];
    a[0] = pc(l.droop);
    a[1] = pc(l.tufting);
    a[2] = pc(l.thickness) * 1.15f;  // a cell's corner to its middle, some jitter out
    a[3] = pc(l.taper);
    b[0] = float(l.strands);
    b[1] = float(l.tufts);
    b[2] = pc(std::min(l.root_shade, fur::kMaxShade));
    b[3] = pc(std::min(l.gap_shade, fur::kMaxShade));
    c[0] = pc(l.sheen);
    c[1] = pc(l.back_glow);
    c[2] = pc(l.length);
    c[3] = pc(l.fill);
}

// each draw's: the three kinds' default looks and the types that pick them, or, for a model with a look of its own
// (the type in the draw's tag, fur::kTagRegister), that look in all three places, so whichever the shader picks is the
// model's, and the game's own fur for it alone when it asks for that
void Fill(float (*xs)[4], const float* tag, bool weasel) {
    for (int k = 0; k < fur::kLooks; k++) PutLook(xs, k, fur::kLookDefaults[k]);
    static const std::vector<float> chipmunks = ChipmunkTypes();
    for (int i = 0; i < 8; i++) xs[12 + i / 4][i % 4] = i < int(chipmunks.size()) ? chipmunks[size_t(i)] : -1.0f;
    for (int i = 0; i < 4; i++) xs[14][i] = i < int(std::size(kWeaselTypes)) ? float(kWeaselTypes[i]) : -1.0f;
    for (int i = 0; i < 4; i++) xs[9][i] = i < int(std::size(kConsoleTypes)) ? float(kConsoleTypes[i]) : -1.0f;
    xs[10][0] = 1.0f / float(1 + fur::g_extra_shells);
    for (int i = 0; i < 4; i++) xs[11][i] = 0;
    if (!tag || tag[3] != 1.0f || tag[0] < 0) return;
    const uint32_t type = uint32_t(tag[0]);
    See(type, weasel, true);
    for (const HairPiece& hp : kHairPieces) {
        if (fur::Canonical(type) != hp.model) continue;
        const fur::ModelLook* hair = fur::FindModelLook(fur::HairOf(hp.model));
        if (!hair || !hair->game) continue;
        xs[11][0] = 1.0f / (16.0f * hp.most);  // xc.x is (layer + 1) / (16 offset), 1 / (16 the piece's offset per layer)
        xs[11][2] = 1.0f;
    }
    if (const fur::ModelLook* m = fur::FindModelLook(fur::Canonical(type))) {  // its class's, in multiplayer
        const fur::ModelLook own = *m;  // the menu may change it meanwhile: one copy, whole or not
        for (int k = 0; k < fur::kLooks; k++) PutLook(xs, k, own.look);
        for (int i = 0; i < 4; i++) xs[9][i] = -1.0f;
        if (own.game) xs[9][0] = float(type);
    }
}

void FillStrands(float (*xs)[4], const float* tag) { Fill(xs, tag, false); }
void FillWeaselStrands(float (*xs)[4], const float* tag) { Fill(xs, tag, true); }

float DepthAlpha() {
    const char* e = std::getenv("XBE_FUR_DEPTH_ALPHA");
    return e ? float(std::atof(e)) : kDepthAlpha;
}

}  // namespace

void fur::Init() {
    const float depth_alpha = DepthAlpha();
    const char* all = std::getenv("XBE_FUR_SHADE_ALL");
    const char* skip = all && *all && *all != '0' ? nullptr : kGirlSkip;
    static const std::string body_vs = std::string("#define FUR_HAIRY 1\n#define FUR_NORMAL R2\n#define FUR_TOP c[181].x\n#define FUR_LENGTH i.v5.x\n") + kStrandVs,
                             head_vs = std::string("#define FUR_NORMAL R6\n#define FUR_TOP c[186].x\n#define FUR_LENGTH i.v9.x\n") + kStrandVs,
                             c90_vs = std::string("#define FUR_NORMAL R2\n#define FUR_TOP c[186].x\n#define FUR_LENGTH i.v5.x\n") + kStrandVs,
                             c85v9_vs = std::string("#define FUR_NORMAL R6\n#define FUR_TOP c[181].x\n#define FUR_LENGTH i.v9.x\n") + kStrandVs,
                             tedi_vs = std::string("#define FUR_NORMAL R2\n#define FUR_TOP c[181].x\n#define FUR_LENGTH 1.0\n") + kStrandVs,
                             c94_vs = std::string("#define FUR_MAP_STRANDS 1\n#define FUR_NORMAL R2\n#define FUR_TOP c[190].x\n#define FUR_LENGTH i.v5.x\n") + kStrandVs,
                             c94_pre = std::string("#define FUR_MAP_STRANDS 1\n") + kStrandPre + "\n#undef FUR_MAP_STRANDS\n",
                             body_pre = std::string("#define FUR_HAIRY 1\n") + kStrandPre + "\n#undef FUR_HAIRY\n";
    const auto strands = [&](const char* match, const std::string& vs, int top, const char* without, const char* pre = kStrandPre,
                             bool weasel = false) {  // the weasels' program
        gpu::g_program_shading.push_back({.match = match, .on = &g_strands, .vs = vs.c_str(), .ps = kStrandPs, .top = top,
                                          .soft = true, .fill = weasel ? FillWeaselStrands : FillStrands,
                                          .fill_register = int(kTagRegister), .functions = kStrandFunctions,
                                          .depth_alpha = depth_alpha, .without = without, .pre = pre,
                                          .between_live = &g_extra_shells, .top_layers = 16});
    };
    strands(kMatch, body_vs, 85 + 96, nullptr, body_pre.c_str());  // the girl's shape too (the officer's): the patches were fur_shading's
    strands(kHeadMatch, head_vs, 90 + 96, nullptr);
    strands(kC90Match, c90_vs, 90 + 96, nullptr);  // Berri's, the officer's head and the soldiers' pieces
    strands(kC94Match, c94_vs, 94 + 96, nullptr, c94_pre.c_str(), true);
    strands(kC85V9Match, c85v9_vs, 85 + 96, nullptr);
    strands(kTediMatch, tedi_vs, 85 + 96, nullptr);
    gpu::g_program_shading.push_back({.match = kMatch, .on = &g_shading, .vs = kVs, .ps = kPs, .top = 85 + 96,
                                      .between = 1, .soft = true, .depth_alpha = depth_alpha, .without = skip});
    gpu::g_program_shading.push_back({.match = kMatch, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 85 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kHeadMatch, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 90 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kC90Match, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 90 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kC85V9Match, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 85 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kC94Match, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 94 + 96, .depth_alpha = depth_alpha});
    gpu::g_program_shading.push_back({.match = kTediMatch, .on = &kAlways, .vs = "    o.x0 = 0;\n    o.x1 = 0;",
                                      .top = 85 + 96, .depth_alpha = depth_alpha});
}

fur::ModelLook* fur::FindModelLook(uint32_t type) {
    if (type == kNoModel) return nullptr;
    for (ModelLook& m : g_model_looks)
        if (m.type == type) return &m;
    return nullptr;
}

int fur::KindOf(uint32_t type) {
    const int n = std::min(g_seen_count.load(std::memory_order_acquire), kSeenMax);
    for (int i = 0; i < n; i++)
        if (g_seen[i].load(std::memory_order_relaxed) == type && g_seen_weasel[i].load(std::memory_order_relaxed))
            return kWeasel;
    if (std::find(std::begin(kWeaselTypes), std::end(kWeaselTypes), type) != std::end(kWeaselTypes)) return kWeasel;
    static const std::vector<float> chipmunks = ChipmunkTypes();
    const uint32_t group = Canonical(type);  // a multiplayer class's: the Sneekers' chipmunk types are two of its models
    for (float c : chipmunks)
        if (uint32_t(c) == type || Canonical(uint32_t(c)) == group) return kChipmunk;
    return kCoat;
}

bool fur::GameFurOf(uint32_t type) {
    return std::find(std::begin(kConsoleTypes), std::end(kConsoleTypes), type) != std::end(kConsoleTypes);
}

fur::ModelLook* fur::ModelLookOf(uint32_t type) {
    if (type == kNoModel) return nullptr;
    if (ModelLook* m = FindModelLook(type)) return m;
    for (ModelLook& m : g_model_looks)
        if (m.type == kNoModel) {
            m.look = DefaultLook(type);
            m.game = GameFurOf(type);
            std::atomic_thread_fence(std::memory_order_release);  // the look whole before the renderer finds it
            m.type = type;
            return &m;
        }
    return nullptr;
}

std::vector<uint32_t> fur::SeenModels() {
    std::vector<uint32_t> out;
    const int n = std::min(g_seen_count.load(std::memory_order_acquire), kSeenMax);
    for (int i = 0; i < n; i++) out.push_back(g_seen[i].load(std::memory_order_relaxed));
    return out;
}

namespace {

// the story's furry models by id (the game's model table, 0x6A58F0): those the soak test (LT + RT + Black on the main
// menu, every level back to back) drew with super fur under XBE_FUR_SURVEY, 2026-11-10, and a few it loaded but
// didn't show, kin of those it did (marked). Multiplayer's by kFurryPrefixes, named as their packages load: every
// squirrel class and the furry Tediz ones (FurryTedi) at each of their four models, one entry a class (fur::Canonical)
constexpr uint32_t kFurryModels[] = {
    0,    // Conker
    7,    // QueenBee
    13,   // Haystack
    16,   // RockMonster
    17,   // WeaselMinion1, the weasels' program
    20,   // WeaselMinion2, the weasels' program
    34,   // RatBrown
    59,   // Weasel (not drawn)
    69,   // TerminatorHaystack
    88,   // CombatSquirrel
    91,   // CombatSquirrelrodent
    109,  // BeeKing
    114,  // Berri
    124,  // WarBossDoll
    130,  // EnvironmentSuit
    134,  // WeaselSuit (not drawn in the soak; the Heist's weasel in F3 captures)
    135,  // CombatGeneral
    145,  // RockMonsterBabe
    150,  // ConkerMatrix
    151,  // BeriMatrix
    152,  // WeaselSwat
    153,  // WeaselScientist (not drawn)
    154,  // ConkerBat
    162,  // BatBoss
    164,  // BatBossStoker
    167,  // ConkerTied (not drawn)
    179,  // RatBrownStitched (not drawn)
    181,  // Barman
    183,  // WeaselBass (not drawn)
    184,  // WeaselClarinet
    185,  // WeaselDrum (not drawn)
    273,  // ConkerWar
    276,  // ConkerHelsing
    299,  // MainFrontend\CombatSquirrelMultiWWIISharpshooter
    321,  // CombatSquirrelShot
    398,  // BerriBarmaid
};
constexpr const char* kFurryPrefixes[] = {
    "Multi\\CombatSquirrel", "MultiFrontend\\CombatSquirrel", "MultiLOD\\CombatSquirrel", "MultiLOD2\\CombatSquirrel",
};
// the furry Tediz classes, at each of their four models: the WWII grunt and every Future one (CombatTedizMulti<X>,
// not WWII); the other WWII Tediz have no fur
bool FurryTedi(const std::string& name) {
    static const char* const kFolders[] = {"Multi\\", "MultiFrontend\\", "MultiLOD\\", "MultiLOD2\\"};
    for (const char* f : kFolders) {
        const std::string head = std::string(f) + "CombatTedizMulti";
        if (name.rfind(head, 0) != 0) continue;
        const std::string cls = name.substr(head.size());
        return !cls.empty() && (cls == "WWIIGrunt" || cls.rfind("WWII", 0) != 0);
    }
    return false;
}

std::atomic<bool> g_image{false};  // the game's image is in memory, its model table with it (fur::ImageLoaded)

// the table's name, "" when it has none
std::string TableName(uint32_t type) {
    std::string out;
    if (type >= 0x205 || !g_image.load(std::memory_order_acquire)) return out;
    const uint32_t name = kernel::R32(0x6A58F0 + 4 * type);
    if (name < 0x10000) return out;
    for (uint32_t i = 0; i < 80; i++) {
        const char c = kernel::G<char>(name + i);
        if (!c) break;
        out += c;
    }
    return out;
}

// a multiplayer class's models (Multi\, MultiFrontend\, MultiLOD\, MultiLOD2\ and the frontend's MainFrontend\
// copies, which spell some classes in another case): its name after the folder, lowercase; "" for any other model
std::string ClassOf(const std::string& name) {
    static const char* const kFolders[] = {"Multi\\", "MultiFrontend\\", "MultiLOD\\", "MultiLOD2\\", "MainFrontend\\"};
    for (const char* f : kFolders)
        if (name.rfind(f, 0) == 0) {
            std::string c = name.substr(strlen(f));
            if (c.rfind("Combat", 0) != 0) return "";
            for (char& ch : c) ch = char(std::tolower(uint8_t(ch)));
            return c;
        }
    return "";
}

// each table entry's Canonical + 1, 0 until worked out (once its name is in). Any thread
constexpr uint32_t kTable = 0x205;
std::atomic<uint32_t> g_canonical[kTable];

}  // namespace

// Conker's models that share his look, Conker's own (0): all but Van Helsing's (276)
constexpr uint32_t kConkers[] = {150 /* ConkerMatrix */, 154 /* ConkerBat */, 167 /* ConkerTied */, 273 /* ConkerWar */};

uint32_t fur::Canonical(uint32_t type) {
    if (std::find(std::begin(kConkers), std::end(kConkers), type) != std::end(kConkers)) return 0;
    if (type >= kTable) return type;
    if (const uint32_t c = g_canonical[type].load(std::memory_order_relaxed)) return c - 1;
    const std::string name = TableName(type);
    if (name.empty()) return type;  // not named yet: asked again later
    const std::string cls = ClassOf(name);
    uint32_t best = type;
    bool full = name.rfind("Multi\\", 0) == 0;
    if (!cls.empty()) {
        for (uint32_t i = 0; i < kTable && !full; i++) {
            const std::string other = TableName(i);
            if (i == type || ClassOf(other) != cls) continue;
            if (other.rfind("Multi\\", 0) == 0) best = i, full = true;  // the class's full model, when it has one
            else best = std::min(best, i);
        }
    }
    // a class's full model not named yet (named as packages are registered): asked again later, or its models
    // could settle on different ones
    if (cls.empty() || full) g_canonical[type].store(best + 1, std::memory_order_relaxed);
    return best;
}

void fur::ImageLoaded() { g_image.store(true, std::memory_order_release); }

std::vector<uint32_t> fur::KnownModels() {
    // the table again every two seconds at most: multiplayer's names come in as their packages are registered
    static std::vector<uint32_t> known;
    static std::chrono::steady_clock::time_point at{};
    const auto now = std::chrono::steady_clock::now();
    if (now - at < std::chrono::seconds(2) && !known.empty()) return known;
    at = now;
    known.clear();
    for (uint32_t type = 0; type < 0x205; type++) {
        const std::string name = TableName(type);
        if (name.empty()) continue;
        const bool listed = std::find(std::begin(kFurryModels), std::end(kFurryModels), type) != std::end(kFurryModels) ||
                            std::any_of(std::begin(kFurryPrefixes), std::end(kFurryPrefixes),
                                        [&](const char* p) { return name.rfind(p, 0) == 0; }) ||
                            FurryTedi(name);
        const uint32_t group = Canonical(type);
        if (listed && std::find(known.begin(), known.end(), group) == known.end()) known.push_back(group);
    }
    for (const HairPiece& hp : kHairPieces)
        if (std::find(known.begin(), known.end(), hp.model) != known.end()) known.push_back(HairOf(hp.model));
    return known;
}

bool fur::Furless(uint32_t type) {
    const std::string name = TableName(type);
    return ClassOf(name).rfind("combattedizmulti", 0) == 0 && !FurryTedi(name);
}

std::string fur::ModelName(uint32_t type) {
    if (type == 0) return "Conker (all but Van Helsing)";
    if (IsHair(type)) {
        const std::string name = TableName(type & ~kHairBit);
        return (name.empty() ? "Model" : name) + " hair (" + std::to_string(type & ~kHairBit) + ")";
    }
    const std::string name = TableName(type);
    if (!ClassOf(name).empty()) return name.substr(name.find('\\') + 1) + " (multiplayer)";
    return (name.empty() ? "Model" : name) + " (" + std::to_string(type) + ")";
}

#if __has_include(<toml++/toml.hpp>)

namespace {

// a Look's settings keys, as fur_strand_* without the prefix
struct LookKey {
    const char* key;
    uint32_t fur::Look::*field;
};
constexpr LookKey kLookKeys[] = {
    {"strands", &fur::Look::strands},       {"tufts", &fur::Look::tufts},
    {"tufting", &fur::Look::tufting},       {"droop", &fur::Look::droop},
    {"thickness", &fur::Look::thickness},   {"taper", &fur::Look::taper},
    {"root_shade", &fur::Look::root_shade}, {"gap_shade", &fur::Look::gap_shade},
    {"sheen", &fur::Look::sheen},           {"back_glow", &fur::Look::back_glow},
    {"length", &fur::Look::length},         {"fill", &fur::Look::fill},
};

}  // namespace

// [fur_models.<type>]: a model's look; keys left out keep its defaults
void fur::Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    if (const toml::array* seen = t["fur_seen"].as_array())
        for (const auto& v : *seen)
            if (const auto type = v.value<int64_t>(); type && *type >= 0 && *type < 0x10000) See(uint32_t(*type), false, false);
    if (const toml::array* seen = t["fur_seen_weasel"].as_array())
        for (const auto& v : *seen)
            if (const auto type = v.value<int64_t>(); type && *type >= 0 && *type < 0x10000) See(uint32_t(*type), true, false);
    const toml::table* models = t["fur_models"].as_table();
    if (!models) return;
    for (const auto& [key, node] : *models) {
        const toml::table* m = node.as_table();
        char* end = nullptr;
        const unsigned long type = std::strtoul(std::string(key.str()).c_str(), &end, 10);
        if (!m || end == nullptr || *end || type >= 0x10000 || key.str().empty()) continue;
        ModelLook* own = ModelLookOf(uint32_t(type));
        if (!own) break;
        own->game = (*m)["game"].value_or(own->game);
        for (const LookKey& k : kLookKeys) own->look.*k.field = (*m)[k.key].value_or(own->look.*k.field);
        own->look.root_shade = std::min(own->look.root_shade, kMaxShade);
        own->look.gap_shade = std::min(own->look.gap_shade, kMaxShade);
    }
}

void fur::Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    toml::table models;
    for (const ModelLook& m : g_model_looks) {
        if (m.type == fur::kNoModel || (m.look == DefaultLook(m.type) && m.game == GameFurOf(m.type))) continue;
        toml::table one;
        one.insert_or_assign("game", m.game);
        for (const LookKey& k : kLookKeys) one.insert_or_assign(k.key, int64_t(m.look.*k.field));
        models.insert_or_assign(std::to_string(m.type), std::move(one));
    }
    t.insert_or_assign("fur_models", std::move(models));
    // the models super fur has drawn, remembered for the F1 menu's list
    toml::array seen, weasel;
    const int n = std::min(g_seen_count.load(std::memory_order_acquire), kSeenMax);
    for (int i = 0; i < n; i++) {
        const uint32_t type = g_seen[i].load(std::memory_order_relaxed);
        seen.push_back(int64_t(type));
        if (g_seen_weasel[i].load(std::memory_order_relaxed)) weasel.push_back(int64_t(type));
    }
    t.insert_or_assign("fur_seen", std::move(seen));
    t.insert_or_assign("fur_seen_weasel", std::move(weasel));
}

#else  // no toml++ (the headless harness): the defaults only

void fur::Load(const void*) {}
void fur::Save(void*) {}

#endif

void fur::TagShells(uint32_t object) {
    if (!object) return;
    const float type = float(kernel::R32(object + 0x68));
    const float byte = float(kernel::G<uint8_t>(object + 0xA1));
    const float level = float(kernel::R32(0x7FA1F8));
    d3d::SetVertexConstant(kTagRegister, {type, byte, level, 1.0f});
}
