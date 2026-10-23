#include "common/grass.h"

#include "gpu/shaders.h"

bool grass::g_wake = true;

namespace {

// The wind program (0x6B89A4) bends each blade away from the player as it passes: c100 the player's position, c101
// its step since the frame before times 20000 (0x1A9440, w 1), s = (1 - v)^2 / d^2 (R5.x, d the blade point's distance
// from c100), the point R2 + s * c101 in homogeneous terms, (p + s k) / (1 + s), which runs off toward k itself as s
// grows. A tip that comes close to a fast rider (the bull at a charge: 37 a frame, k some 730000 away) is thrown
// across the screen, a long thin triangle down to its root. R3 still holds the point before the push less c100, so
// the push is taken again here and its length eased toward wake (40 world units, about a blade's height); a push the
// easing changes by under a thousandth (under some 10) is left as the program drew it, bit for bit.
constexpr const char* kMatch = "(mac_mul(((c[101]).xyzw), ((R5).xxxx)) + ((R2).xyzw))";

constexpr const char* kVs = R"(    o.x0 = 0;
    o.x1 = 0;
    {
        const float wake = 40;
        float3 base = R3.xyz + c[100].xyz;
        float s = R5.x, kw = c[101].w;
        float3 to = c[101].xyz - base * kw;
        float far = length(to), len = s > 0 ? far / max(1 / s + kw, 1e-30) : 0;
        if (!(len < 1e30)) len = 1e30;
        float eased = len / pow(1 + pow(len / wake, 4), 0.25);
        if (far > 0 && eased < len * 0.999) {
            float4 w = float4(base + to * (eased / far), 1);
            float4 pos = float4(dot(w, c[136]), dot(w, c[137]), dot(w, c[138]), dot(w, c[139]));
            float4 p = float4(mac_mul(pos, c[58]).xyz * ilu_rcc(pos.w).x + c[59].xyz, magclamp(pos.w));
            o.pos = float4(float3(p.x * 2 / surf.x - 1, 1 - p.y * 2 / surf.y, p.z * surf.z) * p.w, p.w);
        }
    })";

}  // namespace

void grass::Init() { gpu::g_program_shading.push_back({.match = kMatch, .on = &g_wake, .vs = kVs}); }
