#include "common/lighting.h"

#include <deque>
#include <string>

#include "gpu/shaders.h"

bool lighting::g_per_pixel = true;

namespace {

// The lit programs (69 shapes in the captures: Conker, the other characters, skinned and rigid props) work out two
// colours from the unit view space normal in a register of their own (R0, R2, R5, R6 or R10 so far) and the view space
// position: the room's ambient, a quadratic in the normal per channel, (n, 1) M (n, 1) with M c166..c169 for red,
// c170..c173 green and c174..c177 blue; and the key light, c180 times max(N.L, 0) toward c179 negated, less the blob
// shadows: groups of four blobs (c50..c53, and c54..c61 for the programs that take three groups), each strength / d^2
// from the position placed by c63..c65 and clipped where it lies above the blob. Most put the ambient in oD0, some
// times a scale from an input (an occlusion baked into the mesh), and the key light in oD1; a few put the sum of the
// two in oD0. The colours are then interpolated across each triangle, which the long ones of a tail or an arm show:
// the key light's edge and the blob shadows crawl along them as they move. The shading hands on the normal and the
// position instead and works both out again at each pixel. The program's own oD0 gives the ambient's scale. Registers
// are hardware order, D3D's + 96.
constexpr const char* kAmbientOf = "dot(float4(((R%d).xyzw).xyz, 1), ((c[166]).xyzw))";
constexpr const char* kKeyInD1 = "((c[180]).xyzz));\n        oD1.xyz = mr.xyz;";
constexpr const char* kKeyInD0 = "((c[180]).xyzz)) + ((";
constexpr const char* kThreeGroups = "c[157]";
constexpr const char* kOneGroup = "c[149]";
constexpr const char* kPlaced = "c[159]";
constexpr int kRegisters = 12;

constexpr const char* kFunctions = R"(
float3 LightAmbient(float3 n) {
    float4 q = float4(n, 1);
    return float3(dot(q, float4(dot(q, c[166]), dot(q, c[167]), dot(q, c[168]), dot(q, c[169]))),
                  dot(q, float4(dot(q, c[170]), dot(q, c[171]), dot(q, c[172]), dot(q, c[173]))),
                  dot(q, float4(dot(q, c[174]), dot(q, c[175]), dot(q, c[176]), dot(q, c[177]))));
}
float LightBlobs(float3 p) {
    float4 q = float4(p, 1);
    float sx = dot(q, c[159]), sy = dot(q, c[160]), sz = dot(q, c[161]), b = 0;
    [unroll] for (int g = 0; g < LIGHT_GROUPS; g++) {
        float4 dx = c[146 + 4 * g] - sx, dy = c[147 + 4 * g] - sy;
        b += dot((float4)(sz >= c[148 + 4 * g]) * c[149 + 4 * g], 1 / max(dx * dx + dy * dy, 1e-6));
    }
    return b;
}
float3 LightKey(float3 n, float3 p) { return c[180].xyz * max(dot(n, -c[179].xyz), 0) * (1 - LightBlobs(p)); }
// the scale the program put on the ambient a to make d
float LightScale(float3 d, float3 a) { return clamp(dot(d, a) / max(dot(a, a), 1e-8), 0, 4); }
)";

// x0: the normal and the ambient's scale; x1: the position. oPos undone for it as the fur's shading does
constexpr const char* kVs = R"(    {
        float w = oPos.w, z = -w;
        float2 clip = (oPos.xy - c[59].xy) * w / c[58].xy;
        float3 p = float3((clip.x - c[99].z * z - c[99].w) / c[99].x, (clip.y - c[100].z * z - c[100].w) / c[100].y, z);
        float3 n = R%d.xyz;
        o.x0 = float4(n, LightScale(oD0.rgb%s, LightAmbient(n)));
        o.x1 = float4(p, 0);
    })";

// the colours again at the pixel: the back faces of a two sided draw keep the back colours the program left them
constexpr const char* kPs = R"({
    float nl = length(i.x0.xyz);
    if ((!PS_TWO_SIDED || front) && nl > 1e-6) {
        float3 n = i.x0.xyz / nl, a = i.x0.w * LightAmbient(n), k = LightKey(n, i.x1.xyz);
%s
    }
})";
constexpr const char* kPsKeyInD1 = "        v0.rgb = saturate(a);\n        if (misc.w != 0) v1.rgb = saturate(k);";
constexpr const char* kPsKeyInD0 = "        v0.rgb = saturate(a + k);";

// the entries' text, kept for the pointers they hand the renderer
std::deque<std::string> g_text;

const char* Keep(std::string s) { return g_text.emplace_back(std::move(s)).c_str(); }

std::string Format(const char* fmt, int r, const char* s) {
    char buf[1024];
    snprintf(buf, sizeof(buf), fmt, r, s);
    return buf;
}

}  // namespace

// The three groups' shapes go first: their programs read c[149] too. The view the shading undoes oPos by is the
// ambient occlusion's too.
void lighting::Init() {
    gpu::g_view_constants = {.viewport = 58, .projection = 99};
    struct Shape {
        int groups;
        bool key_in_d0;
    };
    for (Shape sh : {Shape{3, true}, Shape{3, false}, Shape{1, false}}) {
        const char* functions = Keep("static const int LIGHT_GROUPS = " + std::to_string(sh.groups) + ";\n" + kFunctions);
        std::string ps = kPs;
        ps.replace(ps.find("%s"), 2, sh.key_in_d0 ? kPsKeyInD0 : kPsKeyInD1);
        const char* pre = Keep(ps);
        for (int r = 0; r < kRegisters; r++) {
            gpu::g_program_shading.push_back({
                .match = Keep(Format(kAmbientOf, r, "")),
                .on = &g_per_pixel,
                .vs = Keep(Format(kVs, r, sh.key_in_d0 ? " - LightKey(n, p)" : "")),
                .functions = functions,
                .also = {sh.key_in_d0 ? kKeyInD0 : kKeyInD1, sh.groups == 3 ? kThreeGroups : kOneGroup, kPlaced},
                .pre = pre,
                .plain = true,
            });
        }
    }
}
