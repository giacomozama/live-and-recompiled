#pragma once

// AMD FidelityFX Super Resolution 1.0 (FSR 1.0)
// EASU (Edge-Adaptive Spatial Upsampling) & RCAS (Robust Contrast-Adaptive Sharpening)
// Adapted for HLSL 5.0 / Vulkan SPIR-V 1.6
// Based on ffx_fsr1.h from https://github.com/GPUOpen-Effects/FidelityFX-FSR
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// MIT License; full text in THIRD_PARTY.txt

namespace gpu {

inline const char* kFsrHlsl = R"FSR(
cbuffer B : register(b1) {
    float4 con0;
    float4 con1;
    float4 con2;
    float4 con3;
};

Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);

struct O {
    float4 p : SV_Position;
    float2 uv : TEXCOORD0;
};

O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// -----------------------------------------------------------------------------
// EASU (Edge-Adaptive Spatial Upsampling)
// -----------------------------------------------------------------------------

void FsrEasuTap(
    inout float3 aC, inout float aW,
    float2 off,
    float2 dir, float2 len,
    float lob, float clp,
    float3 c)
{
    float2 v = float2(dot(off, dir), dot(off, float2(-dir.y, dir.x))) * len;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSet(
    inout float2 dir, inout float len,
    float w,
    float lA, float lB, float lC, float lD, float lE)
{
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / (lenX + 1e-5), 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / (lenY + 1e-5), 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

float4 easu(O i) : SV_Target {
    float2 ip = i.uv * con3.zw - 0.5;

    float2 pp = ip * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;

    float2 p0 = fp * con1.xy + con1.zw;
    float2 p1 = p0 + con2.xy;
    float2 p2 = p0 + con2.zw;
    float2 p3 = p0 + con3.xy;

    float4 off = float4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;

    float3 bC = img.SampleLevel(ls, p0 + off.xw, 0).rgb; float bL = bC.g + 0.5 * (bC.r + bC.b);
    float3 cC = img.SampleLevel(ls, p0 + off.yw, 0).rgb; float cL = cC.g + 0.5 * (cC.r + cC.b);
    float3 iC = img.SampleLevel(ls, p1 + off.xw, 0).rgb; float iL = iC.g + 0.5 * (iC.r + iC.b);
    float3 jC = img.SampleLevel(ls, p1 + off.yw, 0).rgb; float jL = jC.g + 0.5 * (jC.r + jC.b);
    float3 fC = img.SampleLevel(ls, p1 + off.yz, 0).rgb; float fL = fC.g + 0.5 * (fC.r + fC.b);
    float3 eC = img.SampleLevel(ls, p1 + off.xz, 0).rgb; float eL = eC.g + 0.5 * (eC.r + eC.b);
    float3 kC = img.SampleLevel(ls, p2 + off.xw, 0).rgb; float kL = kC.g + 0.5 * (kC.r + kC.b);
    float3 lC = img.SampleLevel(ls, p2 + off.yw, 0).rgb; float lL = lC.g + 0.5 * (lC.r + lC.b);
    float3 hC = img.SampleLevel(ls, p2 + off.yz, 0).rgb; float hL = hC.g + 0.5 * (hC.r + hC.b);
    float3 gC = img.SampleLevel(ls, p2 + off.xz, 0).rgb; float gL = gC.g + 0.5 * (gC.r + gC.b);
    float3 oC = img.SampleLevel(ls, p3 + off.yz, 0).rgb; float oL = oC.g + 0.5 * (oC.r + oC.b);
    float3 nC = img.SampleLevel(ls, p3 + off.xz, 0).rgb; float nL = nC.g + 0.5 * (nC.r + nC.b);

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;

    FsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len,         pp.x  * (1.0 - pp.y), cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, (1.0 - pp.x) *        pp.y , fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len,         pp.x  *        pp.y , gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1.0 / 32768.0));
    dir = zro ? float2(1.0, 0.0) : (dir * dirR);

    len = len * 0.5;
    len *= len;

    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);

    float lob = 0.5 - 0.29 * len;
    float clp = 1.0 / lob;

    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;

    FsrEasuTap(aC, aW, float2( 0, -1) - pp, dir, len2, lob, clp, bC);
    FsrEasuTap(aC, aW, float2( 1, -1) - pp, dir, len2, lob, clp, cC);
    FsrEasuTap(aC, aW, float2(-1,  1) - pp, dir, len2, lob, clp, iC);
    FsrEasuTap(aC, aW, float2( 0,  1) - pp, dir, len2, lob, clp, jC);
    FsrEasuTap(aC, aW, float2( 0,  0) - pp, dir, len2, lob, clp, fC);
    FsrEasuTap(aC, aW, float2(-1,  0) - pp, dir, len2, lob, clp, eC);
    FsrEasuTap(aC, aW, float2( 1,  1) - pp, dir, len2, lob, clp, kC);
    FsrEasuTap(aC, aW, float2( 2,  1) - pp, dir, len2, lob, clp, lC);
    FsrEasuTap(aC, aW, float2( 2,  0) - pp, dir, len2, lob, clp, hC);
    FsrEasuTap(aC, aW, float2( 1,  0) - pp, dir, len2, lob, clp, gC);
    FsrEasuTap(aC, aW, float2( 1,  2) - pp, dir, len2, lob, clp, oC);
    FsrEasuTap(aC, aW, float2( 0,  2) - pp, dir, len2, lob, clp, nC);

    float3 min4 = min(min(fC, gC), min(jC, kC));
    float3 max4 = max(max(fC, gC), max(jC, kC));
    float3 rgb = min(max4, max(min4, aC / max(aW, 1e-5)));
    return float4(rgb, 1.0);
}

// -----------------------------------------------------------------------------
// RCAS (Robust Contrast-Adaptive Sharpening)
// -----------------------------------------------------------------------------

float4 rcas(O i) : SV_Target {
    float2 texel = con0.xy;
    float sharpness = con0.z;
    float gamma_on = con0.w;

    float3 b = img.SampleLevel(ls, i.uv + float2( 0, -1) * texel, 0).rgb;
    float3 d = img.SampleLevel(ls, i.uv + float2(-1,  0) * texel, 0).rgb;
    float3 e = img.SampleLevel(ls, i.uv, 0).rgb;
    float3 f = img.SampleLevel(ls, i.uv + float2( 1,  0) * texel, 0).rgb;
    float3 h = img.SampleLevel(ls, i.uv + float2( 0,  1) * texel, 0).rgb;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    float2 peakC = float2(1.0, -4.0);
    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);
    float3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);

    float3 lobeRGB = max(-hitMin, hitMax);
    // RCAS maximum negative lobe limit: 0.25 - 1/16 = 0.1875
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * sharpness;

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 rgb = (lobe * (b + d + f + h) + e) * rcpL;

    if (gamma_on > 0.0) {
        rgb = float3(lut.SampleLevel(ps, float2(rgb.r, 0.5), 0).r,
                     lut.SampleLevel(ps, float2(rgb.g, 0.5), 0).g,
                     lut.SampleLevel(ps, float2(rgb.b, 0.5), 0).b);
    }

    return float4(rgb, 1.0);
}
)FSR";

}  // namespace gpu
