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
inline constexpr char kDepthBytes[] = R"(
Texture2D<float> depth : register(t0);
// X24_TYPELESS_G8_UINT exposes stencil in green, not the scalar red channel.
Texture2D<uint2> stencil : register(t1);
cbuffer Order : register(b0) { uint4 order; };
float4 main(float4 position:SV_Position):SV_Target {
    int3 p=int3(int2(position.xy),0);
    uint z=uint(round(saturate(depth.Load(p))*16777215.0));
    uint packed=(z<<8)|(stencil.Load(p).y&255);
    float4 b=float4(packed&255,(packed>>8)&255,(packed>>16)&255,(packed>>24)&255)/255.0;
    return float4(b[order.x],b[order.y],b[order.z],b[order.w]);
}
)";
}  // namespace gpu::d3d11
