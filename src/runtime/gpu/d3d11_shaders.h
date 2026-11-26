#pragma once

namespace gpu::d3d11 {

inline constexpr const char* kCopyColor = R"(
Texture2D<float4> source : register(t0);
SamplerState filter : register(s0);
cbuffer Copy : register(b0) { float4 window; };
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return source.SampleLevel(filter, window.xy + window.zw * uv, 0);
}
)";

inline constexpr const char* kCopyDepthStencil = R"(
#ifdef COPY_MULTISAMPLED
Texture2DMS<float> depth : register(t0);
Texture2DMS<uint4> stencil : register(t1);
#else
Texture2D<float> depth : register(t0);
Texture2D<uint4> stencil : register(t1);
#endif
cbuffer Copy : register(b0) { uint bit; };
uint2 SourcePixel(float2 uv) {
    uint width, height;
#ifdef COPY_MULTISAMPLED
    uint samples;
    depth.GetDimensions(width, height, samples);
#else
    depth.GetDimensions(width, height);
#endif
    uint2 size = uint2(width, height);
    return min(uint2(saturate(uv) * size), size - 1);
}
float copy_depth(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Depth {
#ifdef COPY_MULTISAMPLED
    return depth.Load(SourcePixel(uv), 0);
#else
    return depth.Load(int3(SourcePixel(uv), 0));
#endif
}
void copy_stencil(float4 position : SV_Position, float2 uv : TEXCOORD0) {
#ifdef COPY_MULTISAMPLED
    if (!(stencil.Load(SourcePixel(uv), 0).g & bit)) discard;
#else
    if (!(stencil.Load(int3(SourcePixel(uv), 0)).g & bit)) discard;
#endif
}
)";
}  // namespace gpu::d3d11
