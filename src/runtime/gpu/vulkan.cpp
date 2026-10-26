// The title's Direct3D 8 calls drawn with Vulkan, on HLSL which glslang compiles to SPIR-V.
// Resources move between image layouts; a render pass begins
// lazily on the draw or clear that needs it and ends at the next barrier or copy.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define VOLK_IMPLEMENTATION
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "gpu/dxgi_formats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#ifndef IMGUI_IMPL_VULKAN_USE_VOLK
#define IMGUI_IMPL_VULKAN_USE_VOLK
#endif
#include <backends/imgui_impl_vulkan.h>
#ifdef _WIN32
#include <backends/imgui_impl_win32.h>
#endif

#include <smmintrin.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/interp.h"
#include "gpu/shader_record.h"
#include "gpu/shaders.h"
#include "gpu/smaa_hlsl.h"
#include "gpu/smaa_tex.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

// a and b the same, byte for byte, as memcmp says; inline, 16 bytes at a time. Sizes past what the compiler inlines
// (a sampler's description, a draw's bindings) were a libc call each, some 17 a draw: 4% of a cutscene's render thread
template <typename T>
bool SameBytes(const T& a, const T& b) {
    static_assert(std::is_trivially_copyable_v<T>);
    const uint8_t* x = reinterpret_cast<const uint8_t*>(&a);
    const uint8_t* y = reinterpret_cast<const uint8_t*>(&b);
    __m128i diff = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= sizeof(T); i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
    for (; i + 4 <= sizeof(T); i += 4) {
        uint32_t u, v;
        std::memcpy(&u, x + i, 4);
        std::memcpy(&v, y + i, 4);
        if (u != v) return false;
    }
    return i == sizeof(T) || std::memcmp(x + i, y + i, sizeof(T) - i) == 0;
}

constexpr int kFrames = 3;
constexpr uint32_t kMaxGen = 4;  // app::Settings::frame_generation's most: pictures per frame
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint64_t kCbAlign = 256;     // constants in the ring: past every device's minUniformBufferOffsetAlignment
constexpr uint64_t kPassCbBytes = 256;  // a full screen pass's constants
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;
constexpr size_t kSamplersKept = 2048;  // distinct stage samplers before the cache starts over

// the descriptor set every pipeline takes, pushed per draw: HLSL's b0 b1, t0 to t4 and s0 to s4 (CompileSpirv shifts
// the registers to these); t4 and s4 are a ProgramShading's field, the dummy elsewhere
constexpr uint32_t kBindCb = 0;
constexpr uint32_t kBindTex = 2;
constexpr uint32_t kBindSampler = 7;
constexpr uint32_t kBindViews = kBindSampler - kBindTex;
constexpr uint32_t kBindings = 12;

// layouts by use, a depth image's attachment layout aside
constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kSrc = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
constexpr VkImageLayout kDst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// the effects over the frame's 3D picture (app::Settings) past SMAA (kSmaaHeadHlsl): grade's chromatic aberration and
// film grain. px: 1 / size, size; opts: aberration, grain (0 to 1), the frame's grain seed
const char* kPostHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 opts; };
Texture2D img : register(t0);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float hash(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float4 grade(O i) : SV_Target {
    float4 c = img.SampleLevel(ls, i.uv, 0);
    // red and blue pulled apart along the radius, up to 0.6% of the frame at the corners
    float2 d = (i.uv - 0.5) * opts.x * 0.012;
    c.r = img.SampleLevel(ls, i.uv + d, 0).r;
    c.b = img.SampleLevel(ls, i.uv - d, 0).b;
    // grain: noise per pixel, two hashes summed so it gathers around 0, new each frame, strongest in the shadows
    float2 g = floor(i.p.xy) + opts.z * float2(37.13, 17.71);
    float n = 0.5 * (hash(g) + hash(g + float2(113.7, 271.3)));
    float l = dot(c.rgb, float3(0.299, 0.587, 0.114));
    c.rgb = saturate(c.rgb + (n - 0.5) * opts.y * 0.3 * (1 - 0.6 * l));
    return c;
}
)";

// SMAA (gpu/smaa_hlsl.h) in its custom shading language mode, a source per pass (SmaaHlsl). No texture is handed to a
// function: SMAA's texture arguments become unused uints and its sampling macros read the global of the argument's
// name, which the pass declares.
const char* kSmaaHeadHlsl = R"(
cbuffer B : register(b1) { float4 px; float4 opts; };
SamplerState ls : register(s0);
SamplerState ps : register(s1);
#define SMAA_CUSTOM_SL
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS px
#define SMAATexture2D(tex) uint tex##_unused
#define SMAATexturePass2D(tex) 0u
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(ls, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(ps, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(ls, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(ls, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(ps, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(ls, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// the passes, each computing the offsets SMAA's vertex shaders would hand it
const char* kSmaaTailHlsl = R"(
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
#ifdef SMAA_EDGES
float4 edges(O i) : SV_Target {
    float4 off[3];
    SMAAEdgeDetectionVS(i.uv, off);
    return float4(SMAALumaEdgeDetectionPS(i.uv, off, 0u), 0, 0);
}
#endif
#ifdef SMAA_WEIGHTS
float4 weights(O i) : SV_Target {
    float4 off[3];
    float2 pix;
    SMAABlendingWeightCalculationVS(i.uv, pix, off);
    return SMAABlendingWeightCalculationPS(i.uv, pix, off, 0u, 0u, 0u, float4(0, 0, 0, 0));
}
#endif
#ifdef SMAA_BLEND
float4 blend(O i) : SV_Target {
    float4 off;
    SMAANeighborhoodBlendingVS(i.uv, off);
    return SMAANeighborhoodBlendingPS(i.uv, off, 0u, 0u);
}
#endif
)";

// SMAA's pass 0 edges (colorTex at t0), 1 blending weights (edgesTex, areaTex, searchTex at t0 to t2), 2 neighbourhood
// blending (colorTex, blendTex at t0 and t1); the names a pass doesn't read, its functions' all the same, sit at t3
const char* SmaaHlsl(int pass) {
    static const std::array<std::string, 3> src = [] {
        const char* names[] = {"colorTex", "edgesTex", "areaTex", "searchTex", "blendTex", "tex", "predicationTex",
                               "depthTex", "velocityTex", "currentColorTex", "previousColorTex"};
        const std::vector<std::string> used[3] = {{"colorTex"}, {"edgesTex", "areaTex", "searchTex"},
                                                  {"colorTex", "blendTex"}};
        const char* defines[3] = {"#define SMAA_EDGES\n", "#define SMAA_WEIGHTS\n", "#define SMAA_BLEND\n"};
        std::array<std::string, 3> out;
        for (size_t p = 0; p < 3; p++) {
            std::string g = defines[p];
            for (const char* n : names) {
                auto at = std::find(used[p].begin(), used[p].end(), n);
                size_t reg = at == used[p].end() ? 3 : size_t(at - used[p].begin());
                g += "Texture2D " + std::string(n) + " : register(t" + std::to_string(reg) + ");\n";
            }
            out[p] = g + kSmaaHeadHlsl + kSmaaHlsl + kSmaaTailHlsl;
        }
        return out;
    }();
    return src[size_t(pass)].c_str();
}

// a depth buffer's depth and stencil read pixel for pixel, the first sample of each when MS is defined: depth writes
// the depth, stencilbit keeps the pixels whose stencil has bit opts.x. A stencil view reads the stencil in its first
// channel (D3D12's in its second).
const char* kDepthStencilCopyHlsl = R"(
cbuffer B : register(b1) { uint4 opts; };
#ifdef MS
Texture2DMS<float> img : register(t0);
Texture2DMS<uint> sten : register(t1);
float depthat(int2 p) { return img.Load(p, 0); }
uint stencilat(int2 p) { return sten.Load(p, 0); }
#else
Texture2D<float> img : register(t0);
Texture2D<uint> sten : register(t1);
float depthat(int2 p) { return img.Load(int3(p, 0)); }
uint stencilat(int2 p) { return sten.Load(int3(p, 0)); }
#endif
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float depth(O i) : SV_Depth { return depthat(int2(i.p.xy)); }
void stencilbit(O i) { if (((stencilat(int2(i.p.xy)) >> opts.x) & 1) == 0) discard; }
)";

// a D24S8 depth buffer as the console's memory holds it, read through a 32 bit colour format:
// R G B A take the bytes order picks
const char* kDepthBytesHlsl = R"(
cbuffer B : register(b1) { float4 order; };
Texture2D<float> img : register(t0);
Texture2D<uint> sten : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float depthbyte(O i, uint k) {
    uint z = (uint)round(saturate(img.Load(int3(i.p.xy, 0))) * 16777215.0);
    uint d = z << 8 | (sten.Load(int3(i.p.xy, 0)) & 255);
    return ((d >> (k * 8)) & 255) / 255.0;
}
float4 psmain(O i) : SV_Target {
    return float4(depthbyte(i, (uint)order.x), depthbyte(i, (uint)order.y), depthbyte(i, (uint)order.z), depthbyte(i, (uint)order.w));
}
)";

// D3DBLEND, the console uses GL values
VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

// D3DBLENDOP, GL values
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

// D3DSTENCILOP, GL values
VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// the DXGI formats the pipeline key and Assemble name, as Vulkan's
VkFormat VkFormatOf(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// a colour target's format as the pipeline key names it, D3D12's way, so shaders.rec serves both renderers
uint32_t DxgiOf(VkFormat f) { return f == kBgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN; }

bool HasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
bool IsDepth(VkFormat f) { return HasStencil(f) || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM; }
bool IsBlock(VkFormat f) {
    return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC2_UNORM_BLOCK || f == VK_FORMAT_BC3_UNORM_BLOCK;
}
// bytes per texel, or per 4x4 block
uint32_t TexelBytes(VkFormat f) {
    if (f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) return 8;
    if (IsBlock(f)) return 16;
    return 4;
}

// what the resources' destructors release through: one backend at a time
struct Handles {
    VkDevice dev = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;
} g_vk;

struct Image : std::enable_shared_from_this<Image> {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;              // null: the swap chain's
    VkImageView view = VK_NULL_HANDLE;          // every aspect: a colour image sampled, any image as an attachment
    VkImageView depth_view = VK_NULL_HANDLE;    // a depth and stencil image's depth, sampled
    VkImageView stencil_view = VK_NULL_HANDLE;  // and its stencil
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t w = 1, h = 1, d = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ~Image() {
        for (VkImageView v : {view, depth_view, stencil_view}) {
            if (v) vkDestroyImageView(g_vk.dev, v, nullptr);
        }
        if (alloc) vmaDestroyImage(g_vk.vma, image, alloc);
    }
    // the view a shader samples
    VkImageView Sampled() const { return depth_view ? depth_view : view; }
};
using ImageRef = std::shared_ptr<Image>;

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0;
    ~Buffer() {
        if (alloc) vmaDestroyBuffer(g_vk.vma, buf, alloc);
    }
};
using BufferRef = std::shared_ptr<Buffer>;

// Vulkan objects released once the GPU is past the frame that last used them
struct Deferred {
    std::function<void()> release;
    ~Deferred() {
        if (release) release();
    }
};

struct ImageSpec {
    VkFormat fmt = kBgra;
    uint32_t w = 1, h = 1, d = 1, levels = 1, layers = 1;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
};

// a console surface the renderer draws into, ext.h's Target without the extension's parts
struct Target {
    ImageRef res;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    ImageRef fb;                  // copy read by draws that sample the target they render to
    uint64_t fb_draw = 0;         // the draw fb was copied for, which its other stages reading the target share
    // a depth target read as a 32 bit colour texture: its bytes as colour, R the top depth byte (DepthBytes)
    ImageRef zb;
    VkFormat fmt = kBgra;
    // multisampling (app::Settings::msaa): draws and clears go to ms, res takes them back before it is read
    ImageRef ms;
    bool ms_ahead = false;   // ms drawn since res last took it
    bool res_ahead = false;  // res written since ms last took it
};

struct Tex {
    ImageRef res;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t whole = 0;  // every byte hashed, as sig is at its last upload
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
};

// what a VkSampler is made from, hashed for the cache
struct SamplerDesc {
    uint32_t min = 0, mag = 0, mip = 0;  // VkFilter, VkFilter, VkSamplerMipmapMode
    uint32_t u = 0, v = 0, w = 0;        // VkSamplerAddressMode
    float bias = 0, min_lod = 0, max_lod = 0;
    uint32_t aniso = 1;    // 1: off
    uint32_t compare = 0;  // VkCompareOp + 1, 0: no comparison
    float border[4] = {};
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    VkFormat rtv = kBgra;     // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    ImageRef into;  // a copy from an earlier call to draw over again
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    VkImageView view[4] = {};    // null: nothing bound, the stage reads a dummy of its kind
    Image* res[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};            // stage samples a render target
    uint32_t sampled = 0xF;     // bit per stage the pixel shader reads: the others are bound nothing
    float k[4] = {1, 1, 1, 1};  // texels per render scale texel
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    bool exact[4] = {};         // stage samples a target where it lies, without the half texel back
};

using Spirv = std::vector<uint32_t>;
using SpirvRef = std::shared_ptr<const Spirv>;

constexpr size_t kConstRegs = 192;  // State::vsc, the cbuffer c
using ConstReads = std::bitset<kConstRegs>;

// a draw's shaders, and the SPIR-V Compile returned
struct DrawShaders : ShaderChoice {
    const Spirv* vs_code = nullptr;
    const Spirv* ps_code = nullptr;
    ConstReads reads;  // the registers of c the two read
};

// the registers of c an HLSL source reads: every one when it indexes c by anything but a number
ConstReads ReadsOf(const std::string& src) {
    ConstReads r;
    for (size_t p = src.find("c["); p != std::string::npos; p = src.find("c[", p + 2)) {
        if (p > 0 && (std::isalnum(static_cast<unsigned char>(src[p - 1])) || src[p - 1] == '_')) continue;  // pc[, tc[
        size_t q = p + 2;
        size_t n = 0;
        while (q < src.size() && std::isdigit(static_cast<unsigned char>(src[q])) && n < kConstRegs)
            n = n * 10 + size_t(src[q++] - '0');
        if (q == p + 2 || q >= src.size() || src[q] != ']') return r.set();
        if (n < kConstRegs) r.set(n);  // the declaration's c[192] is no read
    }
    return r;
}

// a full screen pass's pipeline past its shaders
struct FullScreenDesc {
    VkFormat color = VK_FORMAT_UNDEFINED;  // none: depth only
    bool depth = false;                    // a depth attachment
    uint32_t samples = 1;
    int blend = 0;        // 1 premultiplied over the target, which keeps its alpha; 2 by the blend constant
    int ds = 0;           // 1 depth written, always passing; 2 stencil bit stencil_bit set where the shader keeps
    int stencil_bit = 0;
};

EShLanguage StageOf(const char* profile) {
    switch (profile[0]) {
        case 'v': return EShLangVertex;
        case 'g': return EShLangGeometry;
        case 'h': return EShLangTessControl;
        case 'd': return EShLangTessEvaluation;
        default: return EShLangFragment;
    }
}

// the HLSL to SPIR-V, any thread: registers bN, tN, sN at bindings kBindCb + N, kBindTex + N, kBindSampler + N; a
// vertex input at its register's location (VK_LOC), what the stages hand on in declaration order
bool CompileSpirv(const std::string& src, const char* entry, const char* profile, Spirv& out, std::string& err) {
    EShLanguage stage = StageOf(profile);
    glslang::TShader sh(stage);
    const char* text = src.c_str();
    int len = int(src.size());
    sh.setStringsWithLengths(&text, &len, 1);
    sh.setEntryPoint("main");  // the SPIR-V's, which every pipeline names; entry is the HLSL function
    sh.setSourceEntryPoint(entry);
    sh.setPreamble("#define VK_LOC(n) [[vk::location(n)]]\n");
    sh.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    sh.setHlslIoMapping(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindCb);
    sh.setShiftBinding(glslang::EResTexture, kBindTex);
    sh.setShiftBinding(glslang::EResSampler, kBindSampler);
    auto msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, msgs)) {
        err = sh.getInfoLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msgs) || !prog.mapIO()) {
        err = prog.getInfoLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;  // the driver optimizes; glslang is built without SPIRV-Tools
    out.clear();
    glslang::GlslangToSpv(*prog.getIntermediate(stage), out, &opt);
    if (out.empty()) err = "no SPIR-V";
    return !out.empty();
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d,
                                            void*) {
    static std::atomic<int> n{0};
    if (n++ >= 1000) return VK_FALSE;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) LOGE("vulkan: %s", d->pMessage);
    else LOGW("vulkan: %s", d->pMessage);
    return VK_FALSE;
}

// the window's client area
VkExtent2D WindowExtent() {
#ifdef _WIN32
    RECT rc;
    if (!GetClientRect(static_cast<HWND>(app::Window()), &rc)) return {0, 0};
    return {uint32_t(std::max<LONG>(rc.right - rc.left, 0)), uint32_t(std::max<LONG>(rc.bottom - rc.top, 0))};
#else
    VkExtent2D e{};
    app::PixelSize(e.width, e.height);
    return e;
#endif
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    if (a.topology == Topology::Patches) return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// the pipeline's topology class; the draw sets the topology itself
VkPrimitiveTopology TopologyClass(uint32_t topo) {
    switch (topo) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case uint32_t(Topology::Patches): return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

SamplerDesc StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    SamplerDesc sd;
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.min = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mag = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mip = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        // D3D12's anisotropic filter is linear in every direction
        sd.min = sd.mag = VK_FILTER_LINEAR;
        sd.mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sd.aniso = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap and border are read as clamp
    auto unit = [&](uint32_t m) { return clamp && (m == kTaddressWrap || m == kTaddressBorder) ? kTaddressClamp : m; };
    sd.u = Address(unit(t[kTssAddressU]));
    sd.v = Address(unit(t[kTssAddressV]));
    sd.w = Address(t[kTssAddressW]);
    sd.bias = Bits(t[kTssMipLodBias]);
    sd.min_lod = float(t[kTssMaxMipLevel]);
    sd.max_lod = mip ? VK_LOD_CLAMP_NONE : sd.min_lod;
    Color(t[kTssBorderColor], sd.border);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.min = sd.mag = lin(min) || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sd.compare = swap[CmpIndex(s.rs[kRsShadowFunc])] + 1u;
        sd.aniso = 1;
    }
    return sd;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Changed(bool any) override { same_state_ = !any; }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    bool Due(std::chrono::steady_clock::time_point& at) override {
        if (held_.empty()) return false;
        at = held_.front().at;
        return true;
    }
    void Show() override;
    uint32_t Pictures() override { return cap_ || !swap_ || minimised_ ? 1 : swap_gen_; }
    void Keep(const Surface& front) override;
    void Between(const Surface& front, uint32_t i) override;
    void Replaying(bool on) override {
        FlushRun();
        replay_ = on;
        pass_serial_ = skip_to_ = 0;
        kept_run_next_ = 0;
    }
    // XBE_INTERP_RECORD: the pictures shown, kept at the console's size w x h, and F4's saving of them
    void RecordPictures(const std::vector<Image*>& sources, const std::vector<std::string>& notes, uint32_t w, uint32_t h);
    void SaveRecorded();
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override {
        FlushRun();
        Submit(true);
    }
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    void Note(const char* text) override {
        if (cap_) fprintf(cap_, "     note %s\n", text);
    }
    // done once the commands being recorded, the next fence value, have run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;      // the frame's draws
        // its pictures into the swap chain, each after its image is acquired: one, or frame generation's
        VkCommandBuffer present[kMaxGen] = {};
        VkSemaphore acquired[kMaxGen] = {};
        BufferRef upload;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<std::shared_ptr<void>> garbage;
    };
    struct Alloc {
        VkBuffer buf;
        uint64_t off;
        uint8_t* cpu;
    };
    // the attachments a render pass draws into
    struct Attach {
        Image* color = nullptr;
        Image* depth = nullptr;
        bool operator==(const Attach&) const = default;
    };
    // what a draw's descriptors hold: b0 and b1, t0 to t3, s0 to s3
    struct Bindings {
        VkDescriptorBufferInfo cb[2] = {};
        VkImageView view[kBindViews] = {};  // left null: the dummy 2D image, and blit_[0]
        VkSampler smp[kBindViews] = {};
    };

    Alloc Upload(uint64_t size, uint64_t align = kCbAlign);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    void Retire(std::shared_ptr<void> p) { CurrentFrame().garbage.push_back(std::move(p)); }
    ImageRef NewImage(const ImageSpec& s);
    BufferRef NewBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback);
    // the commands' order: a layout change, a full memory barrier, the device's writes to the host
    void Transition(Image& im, VkImageLayout want);
    void Barrier();
    void HostBarrier();
    void ClearWhole(Image& im);
    // render passes: SetTargets names the attachments, EnsureRendering begins a pass on them when none is
    void SetTargets(Image* color, Image* depth) { desired_ = {color, depth}; }
    void EnsureRendering();
    void EndRendering();
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(Image& im, uint32_t w, uint32_t h);
    void Push(const Bindings& b);
    void PushRaw(const Bindings& b);
    bool FastDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, bool patches);
    void KeepDraw(const Assembled& a, const DrawShaders& sh, const DrawTextures& stages, VkPipeline pso,
                  const Bindings& b, GuestExtent size);
    void BeginCommands(VkCommandBuffer cmd);
    void Begin();
    // the recorded commands to the GPU; wait: until they ran, then recording goes on in the same frame
    void Submit(bool wait, VkSemaphore wait_sem = VK_NULL_HANDLE, VkSemaphore signal_sem = VK_NULL_HANDLE);
    void WaitTimeline(uint64_t value);
    uint64_t Completed() const;
    void Blit(VkImageView src, const float uvrect[4], const BlitOptions& o);
    void DrawFullScreen(VkPipeline pso, const Alloc& cb, std::array<VkImageView, 4> views);
    Image* Feedback(Target& t);
    Image* DepthBytes(Target& z, uint32_t format);
    void Pass(const char* hlsl, const char* entry, VkFormat rtv, Image& target, uint32_t w, uint32_t h,
              const VkImageView src[3], const float p[8], float blend);
    VkPipeline EnsurePassPipeline(const char* hlsl, const char* entry, VkFormat rtv, float blend);
    // the effects over the 3D picture (kPostHlsl): once a frame, at the first screen space draw onto the frame-sized
    // target after a 3D one, else at Present
    bool PostWanted() const;
    bool ScreenSpaceDraw(const State& s, const Assembled& a);
    bool ReadsTarget(const State& s);
    void TrackPost(const State& s, bool flat);
    void ApplyPost(Target& t);
    bool EnsureSmaaTextures();
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    VkPipeline EnsureDepthCopyPipeline();
    void DrawDepthCopy(Target& src, Target& dst, VkPipeline pso);
    // depth and stencil from src into the depth attachment dst: from_ms reads the first samples, samples is dst's
    VkPipeline EnsureDepthStencilCopyPipeline(bool from_ms, uint32_t samples, int bit);
    void CopyDepthStencil(Image& src, bool from_ms, Image& dst, uint32_t samples);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a, bool patches);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    Image* BindTexture(const State& s, int stage, DrawTextures& stages);
    Image* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    Image* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                            DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    VkPipeline EnsurePipeline(const PsoKey& k, const DrawShaders& sh);
    VkPipeline FindPipeline(const PsoKey& k, const DrawShaders& sh);
    // any thread: a draw's pipeline, a point list's expanded by gs, patches tessellated by hs and ds
    VkPipeline CreateDrawPipeline(const PsoKey& k, const Spirv& vs, const Spirv& ps, const Spirv* gs,
                                  const Spirv* hs = nullptr, const Spirv* dms = nullptr) const;
    VkPipeline CreateFullScreenPipeline(const Spirv& vs, const Spirv& ps, const FullScreenDesc& d) const;
    VkPipeline CreatePipeline(VkGraphicsPipelineCreateInfo& ci, const Spirv* const code[],
                              const VkShaderStageFlagBits stages[], uint32_t n) const;
    float PointScale() const;  // host pixels per console pixel of the bound target
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c, Bindings& b);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, const DrawTextures& stages, Bindings& b);
    VkSampler EnsureSampler(const SamplerDesc& sd);
    VkSampler FindSampler(const SamplerDesc& sd);
    VkSampler MakeSampler(const SamplerDesc& sd);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    // a run of fast draws (FastDraw) in one state, drawn as one: whether a draw's geometry can join it, its joining,
    // and the run drawn (anything else that records ends it first)
    bool Mergeable(const Assembled& a) const;
    void JoinRun(const Assembled& a);
    void FlushRun();
    // a frame drawn again: a run the frame drew starting at this draw, drawn from its geometry still in the ring
    bool ReuseRun(const State& s);
    void BindGeometry(const Assembled& a);
    void IssueGeometry(const Assembled& a);
    bool DrawBetweenShells(const State& s, const Assembled& a, uint32_t shading, const Bindings& b, VkPipeline pso,
                           VkPipeline zpso);
    void UploadVertexBuffers(const Assembled& a);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the SPIR-V and sets hash
    const Spirv* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // the shaders and pipelines shaders.rec lists, made on threads of their own while the title runs
    void StartWarmUp();
    void WarmUp(std::vector<shader_record::Entry> entries, shader_record::Programs programs);
    struct WarmShader {
        uint64_t hash;
        SpirvRef code;
    };
    void WarmShaders(const std::vector<const shader_record::Entry*>& list, const shader_record::Programs& programs,
                     std::unordered_map<uint64_t, WarmShader>& made);
    size_t WarmPipelines(const std::vector<const shader_record::Entry*>& list,
                         const std::unordered_map<uint64_t, WarmShader>& made);
    // Blit's pipeline
    VkPipeline EnsureBlitPipeline(VkFormat rtv, bool over = false, uint32_t samples = 1);
    // Init, in order, each callee right after its caller
    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreateLayout();
    bool CreateFrames();
    bool CreateDummies();
    bool CreateGammaLut();
    void CreateBlitSamplers();
    bool InitUi();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp();
    void FinishFront(Target& t);
    bool KeepPicture(const Surface& front, uint32_t slot);
    bool AcquireImage(uint32_t& bi, VkSemaphore acquired);
    void DrawFrame(VkImageView picture, uint32_t bi);
    void BlitFrame(VkImageView src);
    void BuildUi();
    void DrawUi();
    void QueuePresent(uint32_t bi);
    void ShowHeld();
    void ReleaseOldTextures();
    void ResetFrame();
    // pictures shown: a frame's own, and those frame generation adds
    void CountFps(uint32_t pictures);
    void ApplySettings();
    void RemakeTargets(float scale, uint32_t samples);
    ImageRef Shrink(Image& src, VkImageView view, uint32_t w, uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    void CapAround(const State& s);
    // an image CapSave shrinks to w x h and writes, colour times mul
    struct CapImage {
        Image* res;
        VkImageView view;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t);
    // multisampling: a frame-sized target's samples, and the copies that keep them and res the same picture
    uint32_t WantSamples() const;
    bool SamplesSupported(VkFormat f, VkImageUsageFlags usage, uint32_t samples) const;
    void CreateSampledTarget(Target& t);
    void ResolveSamples(Target& t);
    void SpreadSamples(Target& t);
    void WriteRes(Target& t, bool whole);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ImageRef CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<VkClearRect>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, Image& src, Target& dst_t, const CopyList& l);
    void CopyByRegion(const Target& src_t, Image& src, Target& dst_t, const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        Image* res;
        ImageRef down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    Image* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, Image& src);

    VkInstance inst_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator vma_ = nullptr;
    // device features past the required ones
    bool aniso_ = false;
    bool non_solid_ = false;
    bool tess_ = false;  // ProgramShading::tess
    bool precise_ = false;
    bool custom_border_ = false;
    bool dynamic_rendering_ext_ = false;
    uint32_t custom_border_max_ = 0;
    uint32_t custom_borders_ = 0;
    VkFormat depth_fmt_ = VK_FORMAT_D24_UNORM_S8_UINT;  // targets'
    VkFormat shadow_fmt_ = VK_FORMAT_D32_SFLOAT;        // depth textures the title fills itself
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = kBgra;
    bool swap_vsync_ = true;
    uint32_t swap_gen_ = 1;    // the frame generation it has images for
    bool swap_stale_ = false;  // out of date or suboptimal: made again before the next frame
    bool minimised_ = false;
    std::vector<ImageRef> bb_;
    std::vector<VkSemaphore> present_sem_;  // per swap chain image: its picture is drawn
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ = false;
    Attach desired_;
    Attach active_;
    bool rendering_ = false;
    std::unordered_map<uint64_t, SpirvRef> blobs_;
    struct ShaderSource {
        uint64_t hash;  // the source's, blobs_'s key
        uint64_t id;    // shader_record's
        ConstReads reads;
    };
    std::unordered_map<uint64_t, ShaderSource> shader_src_;  // vertex and pixel program keys
    // the constants last uploaded, and where; good until Begin
    std::array<std::array<float, 4>, kConstRegs> last_vsc_{};
    DrawConstants last_c_{};
    Alloc last_vc_{}, last_dc_{};
    bool last_vc_ok_ = false, last_dc_ok_ = false;
    // what Draw last set on cmd_, which a draw setting the same leaves as it is; anything else that sets these
    // forgets it (ForgetDrawCmd)
    struct DrawCmd {
        VkPipeline pso = VK_NULL_HANDLE;
        bool bound = false;
        Bindings b;
        bool view = false;
        VkViewport vp{};
        VkRect2D sc{};
        bool fixed = false;
        float bf[4] = {};
        uint32_t sref = 0;
        int topo = -1;
    };
    DrawCmd drawn_;
    // the lookups the draws before made, which a run of draws in one state repeats thousands of times a frame; the
    // maps they front never drop an entry (samplers_ does, and forgets these)
    struct LastShader {
        uint64_t key = 0;
        const ShaderSource* src = nullptr;
        const Spirv* code = nullptr;
    };
    LastShader last_vs_, last_ps_;
    PsoKey last_pso_key_{};
    VkPipeline last_pso_ = VK_NULL_HANDLE;
    bool last_pso_ok_ = false;
    struct LastSampler {
        SamplerDesc sd;
        VkSampler smp = VK_NULL_HANDLE;
    };
    std::array<LastSampler, 4> last_smp_{};
    uint32_t last_smp_next_ = 0;
    void ForgetDrawCmd() { drawn_ = {}; }
    // the last draw that went the whole way, which a draw in the same state (Changed) takes up again: its shaders,
    // textures, pipeline and constants, past the vertices it assembles itself
    struct KeptDraw {
        bool ok = false;
        bool surface = false;  // blends and tests depth without writing it: a draw like it may be a g_surface_depth one
        ConstReads reads;
        bool quad = false;
        DrawTextures stages{};
        VkPipeline pso = VK_NULL_HANDLE;
        Bindings b;
        GuestExtent size{};
        Target* rt = nullptr;  // bound as it drew
        Target* ds = nullptr;
        bool ms = false;
        uint16_t regs = 0;
        uint64_t decode = 0;
        Topology topology = Topology::Triangles;
        bool strip = false, pretransformed = false;
        std::array<VertexElement, 16> elements{};
    };
    KeptDraw kept_;
    bool same_state_ = false;  // Changed: nothing the kept draw drew with has changed since
    std::unordered_map<uint64_t, VkPipeline> psos_;
    shader_record::Recorder record_;
    // WarmUp's results by source hash and pipeline key, taken by Compile and EnsurePipeline as they first need them
    struct Warm {
        std::mutex lock;
        std::unordered_map<uint64_t, SpirvRef> blobs;
        std::unordered_map<uint64_t, VkPipeline> psos;
        std::atomic<bool> stop{false};
        std::thread thread;
    } warm_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkSampler blit_[4] = {};  // linear at s0, point at s1 to s3, all clamped
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, VkPipeline> blit_psos_;
    // stages with nothing bound read these, by kind
    ImageRef dummy_2d_, dummy_3d_, dummy_cube_, dummy_depth_;
    // ProgramShading::field's image per g_program_shading entry, and the version of the field it holds
    struct FieldImage {
        ImageRef im;
        uint64_t version = 0;
    };
    std::vector<FieldImage> fields_;
    VkImageView FieldView(uint32_t shading);
    // ProgramShading::screen: a copy of the draw's colour target as it stands, the one its stages reading the target
    // share; null for the dummy without one
    VkImageView ScreenView();
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    bool cap_after_ = false;  // the last draw matched XBE_CAPTURE_VS
    uint32_t cap_more_ = 0;   // frames still to capture after this one (XBE_CAPTURE_FRAMES)
    uint32_t cap_seq_ = 0;    // the frame's number in its run of captures
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    std::array<std::array<float, 4>, 192> cap_vsc_{};  // the vertex constants at the last captured draw
    struct CapTex {
        ImageRef res;
        VkImageView view;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ImageRef lut_;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    bool bound_ms_ = false;  // the bound attachments are the targets' multisampled ones
    uint32_t msaa_ = 1;      // samples of the targets made with them
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        BufferRef rb[kFrames];
        ImageRef down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                    // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};  // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    bool queries_failed_ = false;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    uint32_t frame_draws_ = 0, frame_opaque_ = 0, frame_counted_ = 0;  // for the optional per-frame summary
    // the effects over the 3D picture: the frame-sized target 3D was drawn to this frame (0 for none), whether they
    // were drawn, the copies they read, and each vertex program's PositionWSource by hash
    uint32_t post_rt_ = 0;
    bool post_done_ = false;
    // XBE_POST_TRACE: what the picture's draws did to the effects, logged in FinishFront when it changes
    struct PostTrace {
        uint32_t solid = 0, other = 0, other_rt = 0, ow = 0, oh = 0;  // 3D draws on the frame's size, on others
        uint32_t flat_elsewhere = 0, flat_reads = 0, no_target = 0;
        const char* applied = nullptr;  // where ApplyPost was called
        const char* outcome = nullptr;  // how it ended
    } post_trace_;
    std::string post_trace_key_;
    uint32_t post_trace_lines_ = 0;
    void PostTraceEnd(const Target& front);
    ImageRef post_tmp_[4];
    ImageRef smaa_area_, smaa_search_;  // SMAA's lookups (gpu/smaa_tex.h)
    std::unordered_map<uint64_t, int> pos_w_;
    uint64_t presents_ = 0;
    uint64_t draw_serial_ = 0;  // draws so far, Target::fb_draw
    State sprite_;              // a fixed function point sprite's state as the console draws it (SpriteStage)
    double fps_ = 0;
    // frame generation (app::Settings::frame_generation): the frame's pictures (Keep, Between), its own last, and a
    // bit for each taken this frame; whether the draws are the frame's again (Replaying); the pictures past a frame's
    // first, held until their time (Due, Show); the time between frames, smoothed, which the pictures share out
    std::vector<ImageRef> gen_pics_;
    uint32_t gen_taken_ = 0;
    bool replay_ = false;
    struct Run {
        uint32_t first = 0, last = 0;  // pass_serial_ of its first and last draws
        uint32_t draws = 0, vertices = 0, buffers = 0;
        int topo = 0;
        std::array<uint32_t, 16> strides{};
        std::array<std::vector<uint8_t>, 16> data;
        std::vector<uint32_t> indices;
        // a run of one buffer: its vertices straight in the ring (data unused), the ring's last allocation as it
        // grows. Through data, a cutscene's 25 MB of vertices a frame were copied twice
        bool direct = false;
        Alloc vb{};
        uint64_t bytes = 0;  // the vertices', without the slack after them
    };
    Run run_;
    bool GrowRun(const VertexBuffer& b);
    // the frame's runs as drawn, for its drawing again: their draws, their geometry in the ring, the stage bindings
    // they had; good until the ring starts over (a waiting Submit) or the frame ends
    struct KeptRun {
        uint32_t first = 0, last = 0, buffers = 0, count = 0;
        int topo = 0;
        VkBuffer bufs[16];
        VkDeviceSize offs[16], sizes[16], strides[16];
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ioff = 0;
        VkImageView view[kBindViews];
        VkSampler smp[kBindViews];
    };
    std::vector<KeptRun> kept_runs_;
    size_t kept_run_next_ = 0;
    uint32_t pass_serial_ = 0;  // Draw calls in this pass: the frame's, or a drawing again
    uint32_t skip_to_ = 0;      // drawing again: the last draw of a reused run
    struct Recorded {
        ImageRef im;
        uint32_t w = 0, h = 0;
        std::string note;
    };
    std::vector<Recorded> recorded_;  // a ring, the next written at recorded_next_
    uint64_t recorded_next_ = 0;
    struct Held {
        uint32_t bi;
        std::chrono::steady_clock::time_point at;
    };
    std::deque<Held> held_;
    std::chrono::steady_clock::time_point gen_last_{};
    double gen_period_ = 1.0 / 60;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    static std::once_flag glslang_once;
    std::call_once(glslang_once, [] { glslang::InitializeProcess(); });
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    if (!CreateSwapChain() || !CreateLayout() || !CreateFrames()) return false;
    BeginCommands(frames_[0].cmd);
    Begin();
    if (!CreateDummies() || !CreateGammaLut()) return false;
    CreateBlitSamplers();
    if (!InitUi()) return false;
    LoadSettings();
    LOGI("vulkan: %ux%u guest, %ux%u window, render scale %.3f, %ux msaa", width, height, bb_w_, bb_h_, scale_, msaa_);
    StartWarmUp();
    return true;
}

VulkanBackend::~VulkanBackend() {
    warm_.stop = true;
    if (warm_.thread.joinable()) warm_.thread.join();
    if (dev_) vkDeviceWaitIdle(dev_);
    if (ui_) ImGui_ImplVulkan_Shutdown();
    if (cap_) fclose(cap_);
    // every image and buffer before the allocator
    targets_.clear();
    textures_.clear();
    late_.clear();
    cap_tex_.clear();
    bb_.clear();
    dummy_2d_.reset();
    dummy_3d_.reset();
    dummy_cube_.reset();
    dummy_depth_.reset();
    lut_.reset();
    for (auto& f : frames_) {
        f.garbage.clear();
        f.upload.reset();
        for (VkSemaphore a : f.acquired) {
            if (a) vkDestroySemaphore(dev_, a, nullptr);
        }
        if (f.pool) vkDestroyCommandPool(dev_, f.pool, nullptr);
    }
    for (auto* m : {&psos_, &blit_psos_, &warm_.psos}) {
        for (auto& [key, p] : *m) {
            if (p) vkDestroyPipeline(dev_, p, nullptr);
        }
    }
    for (auto& [key, s] : samplers_) vkDestroySampler(dev_, s, nullptr);
    for (VkSampler s : blit_) {
        if (s) vkDestroySampler(dev_, s, nullptr);
    }
    for (VkSemaphore s : present_sem_) vkDestroySemaphore(dev_, s, nullptr);
    if (query_pool_) vkDestroyQueryPool(dev_, query_pool_, nullptr);
    if (timeline_) vkDestroySemaphore(dev_, timeline_, nullptr);
    if (ui_pool_) vkDestroyDescriptorPool(dev_, ui_pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
    if (swap_) vkDestroySwapchainKHR(dev_, swap_, nullptr);
    if (vma_) vmaDestroyAllocator(vma_);
    if (dev_) vkDestroyDevice(dev_, nullptr);
    if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(inst_, messenger_, nullptr);
    if (inst_) vkDestroyInstance(inst_, nullptr);
    g_vk = {};
}

// XBE_VULKAN_DEBUG: the validation layer, its messages in the log
bool VulkanBackend::CreateInstance() {
    if (volkInitialize() != VK_SUCCESS) {
        LOGE("vulkan: no Vulkan loader");
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        LOGE("vulkan: the loader is older than Vulkan 1.3");
        return false;
    }
#ifdef _WIN32
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#else
    std::vector<const char*> exts = app::VulkanExtensions();  // the SDL window's surface ones
#endif
    bool debug = std::getenv("XBE_VULKAN_DEBUG") != nullptr;
    const char* validation = "VK_LAYER_KHRONOS_validation";
    if (debug) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = app::kGame.log;
    ai.pEngineName = "xbe_recomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = debug ? 1 : 0;
    ci.ppEnabledLayerNames = &validation;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst_);
    if (r != VK_SUCCESS && debug) {
        LOGW("vulkan: no validation layer (%d), going on without it", int(r));
        debug = false;
        exts.pop_back();
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, nullptr, &inst_);
    }
    if (r != VK_SUCCESS) {
        LOGE("vulkan: no instance (%d)", int(r));
        return false;
    }
    volkLoadInstance(inst_);
    if (debug) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = DebugMessage;
        vkCreateDebugUtilsMessengerEXT(inst_, &mi, nullptr, &messenger_);
        LOGI("vulkan: validation on");
    }
    return true;
}

bool VulkanBackend::CreateSurface() {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = static_cast<HWND>(app::Window());
    if (vkCreateWin32SurfaceKHR(inst_, &si, nullptr, &surface_) != VK_SUCCESS) {
        LOGE("vulkan: no surface for the window");
        return false;
    }
    return true;
#else
    if (!app::VulkanSurface(inst_, &surface_)) {
        LOGE("vulkan: no surface for the window");
        return false;
    }
    return true;
#endif
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) {
        if (!std::strcmp(e.extensionName, name)) return true;
    }
    return false;
}

// a Vulkan 1.3 GPU that presents to the window and has what the renderer needs, a discrete one first
bool VulkanBackend::PickDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst_, &n, devs.data());
    int best_score = 0;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, exts.data());
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        if (p.apiVersion >= VK_API_VERSION_1_3) vkGetPhysicalDeviceFeatures2(d, &f);
        const char* lacks = nullptr;
        if (p.apiVersion < VK_API_VERSION_1_3) lacks = "Vulkan 1.3";
        else if (!HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) lacks = "a swap chain";
        else if (!HasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) lacks = "push descriptors";
        else if (!f.features.geometryShader) lacks = "geometry shaders";
        else if (!f.features.textureCompressionBC) lacks = "BC textures";
        else if (!f12.timelineSemaphore || !f12.hostQueryReset) lacks = "timeline semaphores or host query resets";
        else if (!f13.dynamicRendering) lacks = "dynamic rendering";
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn && family < 0; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = int(i);
        }
        if (!lacks && family < 0) lacks = "a queue that draws and presents to the window";
        if (lacks) {
            LOGI("vulkan: %s left out, no %s", p.deviceName, lacks);
            continue;
        }
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score <= best_score) continue;
        best_score = score;
        phys_ = d;
        props_ = p;
        qfam_ = uint32_t(family);
        aniso_ = f.features.samplerAnisotropy;
        non_solid_ = f.features.fillModeNonSolid;
        tess_ = f.features.tessellationShader;
        precise_ = f.features.occlusionQueryPrecise;
        custom_border_ = HasExtension(exts, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        dynamic_rendering_ext_ = HasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }
    if (!phys_) {
        LOGE("vulkan: no GPU the renderer can use");
        return false;
    }
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    LOGR("Vulkan device: %s (%s %s, Vulkan %u.%u.%u)", props_.deviceName, p12.driverName, p12.driverInfo,
         VK_API_VERSION_MAJOR(props_.apiVersion), VK_API_VERSION_MINOR(props_.apiVersion),
         VK_API_VERSION_PATCH(props_.apiVersion));
    // targets' depth: D24S8 like the console where the GPU has it (AMD's don't)
    auto supports = [&](VkFormat f, VkFormatFeatureFlags want) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
        return (fp.optimalTilingFeatures & want) == want;
    };
    VkFormatFeatureFlags target = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    depth_fmt_ = supports(VK_FORMAT_D24_UNORM_S8_UINT, target) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!supports(depth_fmt_, target)) {
        LOGE("vulkan: no depth and stencil format to draw into");
        return false;
    }
    // depth textures sampled with a comparison need a depth format
    VkFormatFeatureFlags shadow = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    shadow_fmt_ = supports(VK_FORMAT_D32_SFLOAT, shadow) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
    if (depth_fmt_ != VK_FORMAT_D24_UNORM_S8_UINT) LOGI("vulkan: targets' depth is 32 bit float, no D24S8");
    return true;
}

bool VulkanBackend::CreateDevice() {
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, custom_border_ ? &border_have : nullptr};
    vkGetPhysicalDeviceFeatures2(phys_, &have);
    custom_border_ = custom_border_ && border_have.customBorderColors && border_have.customBorderColorWithoutFormat;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    border.customBorderColors = VK_TRUE;
    border.customBorderColorWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, custom_border_ ? &border : nullptr};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    f.features.geometryShader = VK_TRUE;
    f.features.textureCompressionBC = VK_TRUE;
    f.features.samplerAnisotropy = aniso_;
    f.features.fillModeNonSolid = non_solid_;
    f.features.tessellationShader = tess_;
    f.features.occlusionQueryPrecise = precise_;
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (custom_border_) exts.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    // ImGui looks its functions up by the extension's names
    if (dynamic_rendering_ext_) exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    if (VkResult r = vkCreateDevice(phys_, &ci, nullptr, &dev_); r != VK_SUCCESS) {
        LOGE("vulkan: no device (%d)", int(r));
        return false;
    }
    volkLoadDevice(dev_);
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    if (custom_border_) {
        VkPhysicalDeviceCustomBorderColorPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &bp};
        vkGetPhysicalDeviceProperties2(phys_, &p2);
        custom_border_max_ = bp.maxCustomBorderColorSamplers;
    }
    VmaVulkanFunctions fn{};
    fn.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = phys_;
    ai.device = dev_;
    ai.instance = inst_;
    ai.pVulkanFunctions = &fn;
    if (vmaCreateAllocator(&ai, &vma_) != VK_SUCCESS) {
        LOGE("vulkan: no memory allocator");
        return false;
    }
    g_vk = {dev_, vma_};
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    return vkCreateSemaphore(dev_, &si, nullptr, &timeline_) == VK_SUCCESS;
}

// the swap chain at the window's size, or none while it has no size; the old one is idle
bool VulkanBackend::CreateSwapChain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return false;
    VkExtent2D ext = caps.currentExtent.width == UINT32_MAX ? WindowExtent() : caps.currentExtent;
    swap_vsync_ = app::Get().vsync;
    swap_gen_ = std::clamp<uint32_t>(app::Get().frame_generation, 1, kMaxGen);
    swap_stale_ = false;
    minimised_ = !ext.width || !ext.height;
    if (minimised_) return true;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR fmt = formats[0];
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
            return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) {
            fmt = *it;
            break;
        }
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    // without vsync the frame tears in as soon as it is done, as D3D12's tearing does
    if (!swap_vsync_) mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                             : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : mode;
    // a frame's pictures past the first are held acquired while the next is drawn: an image more for each
    uint32_t count = std::max<uint32_t>(caps.minImageCount, kFrames) + swap_gen_ - 1;
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = {std::clamp(ext.width, caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) {
        ci.compositeAlpha = VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (VkResult r = vkCreateSwapchainKHR(dev_, &ci, nullptr, &sc); r != VK_SUCCESS) {
        LOGE("vulkan: no swap chain (%d)", int(r));
        return false;
    }
    bb_.clear();
    for (VkSemaphore s : present_sem_) vkDestroySemaphore(dev_, s, nullptr);
    present_sem_.clear();
    if (swap_) vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = sc;
    swap_fmt_ = fmt.format;
    bb_w_ = ci.imageExtent.width;
    bb_h_ = ci.imageExtent.height;
    vkGetSwapchainImagesKHR(dev_, swap_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(dev_, swap_, &n, images.data());
    for (VkImage image : images) {
        auto im = std::make_shared<Image>();
        im->image = image;
        im->format = swap_fmt_;
        im->w = bb_w_;
        im->h = bb_h_;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &im->view);
        bb_.push_back(std::move(im));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(dev_, &si, nullptr, &s);
        present_sem_.push_back(s);
    }
    return true;
}

// one push descriptor set for every pipeline: b0 b1, t0 to t4, s0 to s4
bool VulkanBackend::CreateLayout() {
    VkDescriptorSetLayoutBinding b[kBindings] = {};
    for (uint32_t i = 0; i < kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < kBindTex       ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < kBindSampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = kBindings;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &dsl_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl_;
    return vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_) == VK_SUCCESS;
}

constexpr VkBufferUsageFlags kUploadUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

bool VulkanBackend::CreateFrames() {
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = qfam_;
        if (vkCreateCommandPool(dev_, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1 + kMaxGen;
        VkCommandBuffer cbs[1 + kMaxGen];
        if (vkAllocateCommandBuffers(dev_, &ai, cbs) != VK_SUCCESS) return false;
        f.cmd = cbs[0];
        std::copy(cbs + 1, cbs + 1 + kMaxGen, f.present);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& a : f.acquired) {
            if (vkCreateSemaphore(dev_, &si, nullptr, &a) != VK_SUCCESS) return false;
        }
        f.upload = NewBuffer(kUploadSize, kUploadUsage, false);
        if (!f.upload) return false;
    }
    return true;
}

// 1 x 1 images of each kind a stage declares, cleared, for stages with nothing bound
bool VulkanBackend::CreateDummies() {
    ImageSpec sp;
    dummy_2d_ = NewImage(sp);
    sp.type = VK_IMAGE_VIEW_TYPE_3D;
    dummy_3d_ = NewImage(sp);
    sp.type = VK_IMAGE_VIEW_TYPE_CUBE;
    sp.layers = 6;
    dummy_cube_ = NewImage(sp);
    ImageSpec ds;
    ds.fmt = depth_fmt_;
    ds.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    dummy_depth_ = NewImage(ds);
    for (ImageRef* im : {&dummy_2d_, &dummy_3d_, &dummy_cube_, &dummy_depth_}) {
        if (!*im) return false;
        ClearWhole(**im);
        Transition(**im, kRead);
    }
    return true;
}

// gamma ramp as a 256 x 1 lookup
bool VulkanBackend::CreateGammaLut() {
    ImageSpec sp;
    sp.w = 256;
    lut_ = NewImage(sp);
    if (!lut_) return false;
    ClearWhole(*lut_);
    Transition(*lut_, kRead);
    return true;
}

// four clamp samplers: linear at s0, point at s1 to s3
void VulkanBackend::CreateBlitSamplers() {
    SamplerDesc sd;
    sd.u = sd.v = sd.w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sd.max_lod = VK_LOD_CLAMP_NONE;
    for (int i = 0; i < 4; i++) {
        sd.min = sd.mag = i == 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sd.mip = i == 0 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        blit_[i] = MakeSampler(sd);
    }
}

bool VulkanBackend::InitUi() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = inst_;
    ii.PhysicalDevice = phys_;
    ii.Device = dev_;
    ii.QueueFamily = qfam_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    // its vertex buffers go round per draw: one per picture, kMaxGen a frame with frame generation, kFrames in flight
    ii.ImageCount = std::max<uint32_t>(uint32_t(bb_.size()), kFrames * kMaxGen + 1);
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_fmt_;
    ui_ = ImGui_ImplVulkan_Init(&ii);
    if (!ui_) LOGE("vulkan: the overlay could not start");
    return ui_;
}

void VulkanBackend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    msaa_ = WantSamples();
}

ImageRef VulkanBackend::NewImage(const ImageSpec& s) {
    auto im = std::make_shared<Image>();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = s.type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = s.type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = s.fmt;
    ci.extent = {s.w, s.h, s.d};
    ci.mipLevels = s.levels;
    ci.arrayLayers = s.layers;
    ci.samples = s.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = s.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(vma_, &ci, &ai, &im->image, &im->alloc, nullptr) != VK_SUCCESS) {
        LOG_FIRST(8, "vulkan: no memory for a %ux%u image of format %d", s.w, s.h, int(s.fmt));
        return nullptr;
    }
    im->format = s.fmt;
    im->w = s.w;
    im->h = s.h;
    im->d = s.d;
    im->samples = s.samples;
    bool depth = IsDepth(s.fmt);
    bool stencil = HasStencil(s.fmt);
    im->aspect = depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                       : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->image;
    vi.viewType = s.type;
    vi.format = s.fmt;
    vi.subresourceRange = {im->aspect, 0, s.levels, 0, s.layers};
    vkCreateImageView(dev_, &vi, nullptr, &im->view);
    // a shader samples one aspect of a depth and stencil image at a time
    if (stencil && (s.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCreateImageView(dev_, &vi, nullptr, &im->depth_view);
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCreateImageView(dev_, &vi, nullptr, &im->stencil_view);
    }
    return im;
}

// mapped host memory: the GPU reads it where the frame's upload ring is, readback writes it back
BufferRef VulkanBackend::NewBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
    auto b = std::make_shared<Buffer>();
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    if (!readback) ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma_, &ci, &ai, &b->buf, &b->alloc, &info) != VK_SUCCESS) {
        LOG_FIRST(8, "vulkan: no memory for a %llu byte buffer", (unsigned long long)size);
        return nullptr;
    }
    b->map = static_cast<uint8_t*>(info.pMappedData);
    b->size = size;
    return b;
}

void VulkanBackend::BeginCommands(VkCommandBuffer cmd) {
    cmd_ = cmd;
    ForgetDrawCmd();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
}

// a new command buffer's state: nothing bound, the dynamic states a pipeline may read set
void VulkanBackend::Begin() {
    // a new command buffer: the ring may have started over
    last_vc_ok_ = last_dc_ok_ = false;
    ForgetDrawCmd();
    float zero[4] = {};
    vkCmdSetBlendConstants(cmd_, zero);
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    bound_rt_ = bound_ds_ = nullptr;
    desired_ = active_ = {};
    rendering_ = false;
}

void VulkanBackend::Submit(bool wait, VkSemaphore wait_sem, VkSemaphore signal_sem) {
    EndRendering();
    vkEndCommandBuffer(cmd_);
    uint64_t value = ++fence_value_;
    VkSemaphore signals[2] = {timeline_, signal_sem};
    uint64_t values[2] = {value, 0};
    uint64_t wait_value = 0;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = wait_sem ? 1 : 0;
    ts.pWaitSemaphoreValues = &wait_value;
    ts.signalSemaphoreValueCount = signal_sem ? 2 : 1;
    ts.pSignalSemaphoreValues = values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
    si.waitSemaphoreCount = wait_sem ? 1 : 0;
    si.pWaitSemaphores = &wait_sem;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = signal_sem ? 2 : 1;
    si.pSignalSemaphores = signals;
    if (VkResult r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE); r != VK_SUCCESS) {
        LOG_FIRST(4, "vulkan: submit failed (%d)", int(r));
    }
    Frame& f = CurrentFrame();
    f.fence = value;
    if (wait) {
        WaitTimeline(value);
        vkResetCommandPool(dev_, f.pool, 0);
        f.garbage.clear();
        f.used = 0;
        kept_runs_.clear();  // their geometry was in the ring
        BeginCommands(f.cmd);
        Begin();
    }
}

void VulkanBackend::WaitTimeline(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    vkWaitSemaphores(dev_, &wi, UINT64_MAX);
}

uint64_t VulkanBackend::Completed() const {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev_, timeline_, &v);
    return v;
}

// the viewport D3D12 would have: y down, the bottom edge first and the height negative
void VulkanBackend::SetViewport(float x, float y, float w, float h) {
    ForgetDrawCmd();
    VkViewport vp{x, y + h, w, -h, 0, 1};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    ForgetDrawCmd();
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    VkRect2D r{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    vkCmdSetScissor(cmd_, 0, 1, &r);
}

void VulkanBackend::SetFullViewport(uint32_t w, uint32_t h) {
    SetViewport(0, 0, float(w), float(h));
    SetScissor(0, 0, int32_t(w), int32_t(h));
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void VulkanBackend::SetBlitTarget(Image& im, uint32_t w, uint32_t h) {
    SetTargets(&im, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload->buf, o, f.upload->map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    BufferRef b = NewBuffer(size, kUploadUsage, false);
    if (!b) {
        // nothing to put it in: the ring's start takes it, and draws already recorded there read it
        LOG_FIRST(4, "vulkan: upload of %llu bytes overwrites the ring", (unsigned long long)size);
        return {f.upload->buf, 0, f.upload->map};
    }
    Retire(b);
    return {b->buf, 0, b->map};
}

void VulkanBackend::Transition(Image& im, VkImageLayout want) {
    if (im.layout == want) return;
    EndRendering();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    im.layout = want;
}

// everything recorded before done before anything after: between passes on the same image, and around copies
void VulkanBackend::Barrier() {
    EndRendering();
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0,
                         nullptr, 0, nullptr);
}

// copies into readback buffers seen by the CPU once the fence is past them
void VulkanBackend::HostBarrier() {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m, 0, nullptr, 0,
                         nullptr);
}

// colour to 0, depth to 1 and stencil to 0, as D3D12 clears a new target
void VulkanBackend::ClearWhole(Image& im) {
    Transition(im, kDst);
    Barrier();
    VkImageSubresourceRange r{im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue v{1, 0};
        vkCmdClearDepthStencilImage(cmd_, im.image, kDst, &v, 1, &r);
    } else {
        VkClearColorValue c{};
        vkCmdClearColorImage(cmd_, im.image, kDst, &c, 1, &r);
    }
}

void VulkanBackend::EnsureRendering() {
    if (rendering_ && active_ == desired_) return;
    EndRendering();
    Image* c = desired_.color;
    Image* z = desired_.depth;
    if (!c && !z) return;
    if (c) Transition(*c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (z) Transition(*z, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    Barrier();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo za{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    for (VkRenderingAttachmentInfo* a : {&ca, &za}) {
        a->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    uint32_t w = c ? c->w : z->w;
    uint32_t h = c ? c->h : z->h;
    if (c) {
        ca.imageView = c->view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (z) {
        za.imageView = z->view;
        za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ri.pDepthAttachment = &za;
        if (z->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &za;
        w = std::min(w, z->w);
        h = std::min(h, z->h);
    }
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    vkCmdBeginRendering(cmd_, &ri);
    rendering_ = true;
    active_ = desired_;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    FlushRun();
    vkCmdEndRendering(cmd_);
    rendering_ = false;
}

void VulkanBackend::Push(const Bindings& b) {
    ForgetDrawCmd();
    PushRaw(b);
}

void VulkanBackend::PushRaw(const Bindings& b) {
    VkDescriptorImageInfo ii[kBindViews];
    VkDescriptorImageInfo si[kBindViews];
    VkWriteDescriptorSet w[kBindings] = {};
    for (uint32_t i = 0; i < kBindings; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (uint32_t i = 0; i < 2; i++) {
        w[kBindCb + i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[kBindCb + i].pBufferInfo = &b.cb[i];
    }
    for (uint32_t i = 0; i < kBindViews; i++) {
        ii[i] = {VK_NULL_HANDLE, b.view[i] ? b.view[i] : dummy_2d_->view, kRead};
        w[kBindTex + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[kBindTex + i].pImageInfo = &ii[i];
        si[i] = {b.smp[i] ? b.smp[i] : blit_[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[kBindSampler + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[kBindSampler + i].pImageInfo = &si[i];
    }
    vkCmdPushDescriptorSetKHR(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, kBindings, w);
}

// a resized target gives back its resources once the GPU is past them
void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    for (ImageRef* r : {&it->second.res, &it->second.fb, &it->second.zb, &it->second.ms}) {
        if (*r) Retire(*r);
    }
    targets_.erase(it);
}

// the image of a new target at render scale, cleared
bool VulkanBackend::CreateTargetResource(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    ImageSpec sp;
    sp.fmt = t.depth ? depth_fmt_ : t.fmt;
    sp.w = t.w;
    sp.h = t.h;
    sp.usage = (t.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    t.res = NewImage(sp);
    if (!t.res) return false;
    ClearWhole(*t.res);
    return true;
}

// msaa samples, 1 when off
uint32_t VulkanBackend::WantSamples() const {
    uint32_t n = app::Get().msaa;
    return n == 2 || n == 4 || n == 8 ? n : 1;
}

bool VulkanBackend::SamplesSupported(VkFormat f, VkImageUsageFlags usage, uint32_t samples) const {
    VkImageFormatProperties p{};
    if (vkGetPhysicalDeviceImageFormatProperties(phys_, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &p) !=
        VK_SUCCESS) {
        return false;
    }
    return (p.sampleCounts & samples) != 0;
}

// a target the size of the title's frame gets msaa_ samples, cleared; none when the format can't
void VulkanBackend::CreateSampledTarget(Target& t) {
    if (msaa_ < 2 || t.gw != guest_w_ || t.gh != guest_h_) return;
    ImageSpec sp;
    sp.fmt = t.depth ? depth_fmt_ : t.fmt;
    sp.w = t.w;
    sp.h = t.h;
    sp.samples = VkSampleCountFlagBits(msaa_);
    sp.usage = (t.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!SamplesSupported(sp.fmt, sp.usage, msaa_)) {
        LOG_FIRST(4, "vulkan: %ux multisampling of format %d unsupported", msaa_, int(sp.fmt));
        return;
    }
    t.ms = NewImage(sp);
    if (t.ms) ClearWhole(*t.ms);
}

// res takes what was drawn to the samples since: colour averaged, depth and stencil the first sample's
void VulkanBackend::ResolveSamples(Target& t) {
    if (!t.ms_ahead) return;
    t.ms_ahead = false;
    if (t.depth) {
        Transition(*t.ms, kRead);
        CopyDepthStencil(*t.ms, true, *t.res, 1);
        return;
    }
    Transition(*t.ms, kSrc);
    Transition(*t.res, kDst);
    Barrier();
    VkImageResolve r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    vkCmdResolveImage(cmd_, t.ms->image, kSrc, t.res->image, kDst, 1, &r);
}

// the samples take what was written to res since, every sample of a pixel the same
void VulkanBackend::SpreadSamples(Target& t) {
    if (!t.res_ahead) return;
    t.res_ahead = false;
    Transition(*t.res, kRead);
    if (t.depth) {
        CopyDepthStencil(*t.res, false, *t.ms, msaa_);
        return;
    }
    SetBlitTarget(*t.ms, t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(t.res->view, uv, {.alpha = true, .rtv = t.fmt, .samples = msaa_});
}

// before res is written other than by a draw: whole, nothing drawn to the samples stays; they take res before the next
void VulkanBackend::WriteRes(Target& t, bool whole) {
    if (!t.ms) return;
    if (whole) t.ms_ahead = false;
    else ResolveSamples(t);
    t.res_ahead = true;
}

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t VulkanBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

VulkanBackend::TargetSlot VulkanBackend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t)) return nullptr;
    g_perf.targets++;
    CreateSampledTarget(t);
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

bool VulkanBackend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    // multisampled when every target has samples, else into res
    bool ms = (!rt || rt->ms) && (!ds || ds->ms);
    for (Target* t : {rt, ds}) {
        if (!t) continue;
        if (ms) {
            SpreadSamples(*t);
            t->ms_ahead = true;
        } else {
            WriteRes(*t, false);
        }
        t->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_ || ms != bound_ms_) {
        Image* color = rt ? (ms ? rt->ms.get() : rt->res.get()) : nullptr;
        Image* depth = ds ? (ms ? ds->ms.get() : ds->res.get()) : nullptr;
        SetTargets(color, depth);
        bound_rt_ = rt;
        bound_ds_ = ds;
        bound_ms_ = ms;
    }
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
VkPipeline VulkanBackend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso;
    uint64_t h;
    const Spirv* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    const Spirv* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    pso = CreateFullScreenPipeline(*vs, *ps, {.depth = true, .ds = 1});
    return pso;
}

// src's depth drawn into dst's depth buffer
void VulkanBackend::DrawDepthCopy(Target& src, Target& dst, VkPipeline pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    ResolveSamples(src);
    WriteRes(dst, true);
    Transition(*src.res, kRead);
    SetTargets(nullptr, dst.res.get());
    SetFullViewport(dst.w, dst.h);
    DrawFullScreen(pso, Upload(kPassCbBytes), {src.res->Sampled(), VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE});
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// CopyDepthStencil's pipelines, kept in blit_psos_: bit < 0 writes depth, else that stencil bit; null when they fail
VkPipeline VulkanBackend::EnsureDepthStencilCopyPipeline(bool from_ms, uint32_t samples, int bit) {
    uint64_t seed = uint64_t(from_ms ? 1 : 0) | uint64_t(samples) << 1 | uint64_t(bit + 1) << 8;
    auto& pso = blit_psos_[Hash(kDepthStencilCopyHlsl, std::strlen(kDepthStencilCopyHlsl), seed)];
    if (pso) return pso;
    std::string src = std::string(from_ms ? "#define MS 1\n" : "") + kDepthStencilCopyHlsl;
    uint64_t h;
    const Spirv* vs = Compile(src, "vs", "vs_5_0", h);
    const Spirv* ps = Compile(src, bit < 0 ? "depth" : "stencilbit", "ps_5_0", h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    // the reference has every bit set: a pixel the shader keeps gets this one
    pso = CreateFullScreenPipeline(*vs, *ps,
                                   {.depth = true, .samples = samples, .ds = bit < 0 ? 1 : 2, .stencil_bit = bit});
    return pso;
}

// src's depth and stencil drawn over the depth attachment dst of the same size; src readable by shaders. No stencil
// export: the stencil is cleared, then each bit drawn on its own.
void VulkanBackend::CopyDepthStencil(Image& src, bool from_ms, Image& dst, uint32_t samples) {
    constexpr int kStencilBits = 8;
    VkPipeline depth = EnsureDepthStencilCopyPipeline(from_ms, samples, -1);
    if (!depth) return;
    std::array<VkImageView, 4> views = {src.depth_view, src.stencil_view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    SetTargets(nullptr, &dst);
    SetFullViewport(dst.w, dst.h);
    EnsureRendering();
    VkClearAttachment ca{VK_IMAGE_ASPECT_STENCIL_BIT};
    ca.clearValue.depthStencil = {1, 0};
    VkClearRect cr{{{0, 0}, {dst.w, dst.h}}, 0, 1};
    vkCmdClearAttachments(cmd_, 1, &ca, 1, &cr);
    DrawFullScreen(depth, Upload(kPassCbBytes), views);
    ForgetDrawCmd();
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    for (int bit = 0; bit < kStencilBits; bit++) {
        VkPipeline pso = EnsureDepthStencilCopyPipeline(from_ms, samples, bit);
        if (!pso) break;
        Alloc cb = Upload(kPassCbBytes);
        uint32_t c[4] = {uint32_t(bit), 0, 0, 0};
        std::memcpy(cb.cpu, c, sizeof(c));
        DrawFullScreen(pso, cb, views);
    }
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    VkPipeline pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(si->second, di->second, pso);
    return true;
}

// BGRA pixels into a new w x h texture, left readable by shaders
ImageRef VulkanBackend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    ImageRef tex = NewImage({.w = w, .h = h});
    if (!tex) return nullptr;
    uint32_t row = w * 4;
    Alloc a = Upload(uint64_t(row) * h, 16);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], row);
    Transition(*tex, kDst);
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, a.buf, tex->image, kDst, 1, &r);
    Transition(*tex, kRead);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void VulkanBackend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ImageRef tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    WriteRes(t, !over);
    SetBlitTarget(*t.res, t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(tex->view, uv, {.alpha = true, .rtv = t.fmt, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// the full screen triangle: cb at b0 and b1, views at t0 to t3 (a 2D dummy for none), the blit samplers
void VulkanBackend::DrawFullScreen(VkPipeline pso, const Alloc& cb, std::array<VkImageView, 4> views) {
    if (!pso) return;
    EnsureRendering();
    ForgetDrawCmd();
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    Bindings b;
    b.cb[0] = b.cb[1] = {cb.buf, cb.off, kPassCbBytes};
    for (int i = 0; i < 4; i++) {
        b.view[i] = views[size_t(i)] ? views[size_t(i)] : dummy_2d_->view;
        b.smp[i] = blit_[i];
    }
    Push(b);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
}

void VulkanBackend::Blit(VkImageView src, const float uvrect[4], const BlitOptions& o) {
    Alloc cb = Upload(kPassCbBytes);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb, {src, lut_->view, VK_NULL_HANDLE, VK_NULL_HANDLE});
}

const Spirv* VulkanBackend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    hash = SourceHash(src, entry, profile);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.get();
    {
        std::lock_guard<std::mutex> lock(warm_.lock);
        auto w = warm_.blobs.find(hash);
        if (w != warm_.blobs.end()) {
            const Spirv* code = (blobs_[hash] = std::move(w->second)).get();
            warm_.blobs.erase(w);
            return code;
        }
    }
    Spirv code;
    std::string err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    auto t = std::chrono::steady_clock::now();
    bool ok = CompileSpirv(src, entry, profile, code, err);
    // a compile runs on the frame that first needs the shader: log the ones long enough to be seen as a hitch
    if (double ms = SinceMs(t); ms > 20) LOGR("vulkan: %s compile took %.0f ms (%zu bytes of source)", profile, ms, src.size());
    if (!ok) {
        LOGE("vulkan: %s compile failed: %s", profile, err.c_str());
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    auto ref = std::make_shared<const Spirv>(std::move(code));
    blobs_[hash] = ref;
    return ref.get();
}

VkPipeline VulkanBackend::CreatePipeline(VkGraphicsPipelineCreateInfo& ci, const Spirv* const code[],
                                         const VkShaderStageFlagBits stages[], uint32_t n) const {
    VkShaderModule modules[4] = {};
    VkPipelineShaderStageCreateInfo si[4] = {};
    bool made = true;
    for (uint32_t i = 0; i < n; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code[i]->size() * sizeof(uint32_t);
        mi.pCode = code[i]->data();
        made = made && vkCreateShaderModule(dev_, &mi, nullptr, &modules[i]) == VK_SUCCESS;
        si[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si[i].stage = stages[i];
        si[i].module = modules[i];
        si[i].pName = "main";
    }
    VkPipeline p = VK_NULL_HANDLE;
    ci.stageCount = n;
    ci.pStages = si;
    ci.layout = layout_;
    if (made && vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    for (VkShaderModule m : modules) {
        if (m) vkDestroyShaderModule(dev_, m, nullptr);
    }
    return p;
}

// a full screen pass's pipeline: solid, no culling, one colour target or a depth one
VkPipeline VulkanBackend::CreateFullScreenPipeline(const Spirv& vs, const Spirv& ps, const FullScreenDesc& d) const {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(d.samples);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (d.ds == 1) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    } else if (d.ds == 2) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF,
                    1u << d.stencil_bit, 0};
        ds.back = ds.front;
    }
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (d.blend == 1) {
        // premultiplied colour, the target keeps its alpha
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (d.blend == 2) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.color ? 1 : 0;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d.color ? 1 : 0;
    ri.pColorAttachmentFormats = &d.color;
    ri.depthAttachmentFormat = d.depth ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = d.depth && HasStencil(depth_fmt_) ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    const Spirv* code[3] = {&vs, &ps, nullptr};
    const VkShaderStageFlagBits stages[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             VK_SHADER_STAGE_GEOMETRY_BIT};
    return CreatePipeline(ci, code, stages, 2);
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples - 1) << 33];
    if (pso) return pso;
    uint64_t h;
    const Spirv* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    const Spirv* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    pso = CreateFullScreenPipeline(*vs, *ps, {.color = rtv, .samples = samples, .blend = over ? 1 : 0});
    return pso;
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
VkPipeline VulkanBackend::EnsurePassPipeline(const char* hlsl, const char* entry, VkFormat rtv, float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso;
    uint64_t h;
    const Spirv* vs = Compile(hlsl, "vs", "vs_5_0", h);
    const Spirv* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return VK_NULL_HANDLE;
    pso = CreateFullScreenPipeline(*vs, *ps, {.color = rtv, .blend = blend >= 0 ? 2 : 0});
    return pso;
}

// a full screen pass into target: src are t0 to t2, p is b1, blend >= 0 blends by that factor
void VulkanBackend::Pass(const char* hlsl, const char* entry, VkFormat rtv, Image& target, uint32_t w, uint32_t h,
                         const VkImageView src[3], const float p[8], float blend) {
    VkPipeline pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    Alloc cb = Upload(kPassCbBytes);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    SetTargets(&target, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        ForgetDrawCmd();
        vkCmdSetBlendConstants(cmd_, f);
    }
    DrawFullScreen(pso, cb, {src[0], src[1], src[2], VK_NULL_HANDLE});
    bound_rt_ = bound_ds_ = nullptr;
}

// the depth target's bytes as colour, drawn again each time a draw reads them; null when there is no memory for it
Image* VulkanBackend::DepthBytes(Target& z, uint32_t format) {
    if (!z.zb) {
        z.zb = NewImage({.w = z.w, .h = z.h,
                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
        if (!z.zb) return nullptr;
    }
    uint32_t at = cap_i_;
    if (cap_) fprintf(cap_, "%04u depth bytes %08X format %X\n", cap_i_++, z.data, format);
    ResolveSamples(z);
    Transition(*z.res, kRead);
    const VkImageView src[3] = {z.res->depth_view, z.res->stencil_view, VK_NULL_HANDLE};
    float p[8] = {};
    DepthByteOrder(format, p);
    Pass(kDepthBytesHlsl, "psmain", kBgra, *z.zb, z.w, z.h, src, p, -1);
    Transition(*z.zb, kRead);
    // the bytes as the draw reads them, at the target's own size (alpha kept: the 32 bit BMP holds it)
    if (cap_) {
        char name[48];
        snprintf(name, sizeof(name), "%04u_zbytes_%08X_%X", at, z.data, format);
        CapSave({.res = z.zb.get(), .view = z.zb->view, .w = z.w, .h = z.h}, name);
    }
    return z.zb.get();
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(Target& t) {
    if (!t.fb) {
        t.fb = NewImage({.fmt = t.fmt, .w = t.w, .h = t.h});
        if (!t.fb) return nullptr;
    }
    ResolveSamples(t);
    Transition(*t.res, kSrc);
    Transition(*t.fb, kDst);
    Barrier();
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cmd_, t.res->image, kSrc, t.fb->image, kDst, 1, &r);
    Transition(*t.fb, kRead);
    return t.fb.get();
}

VkPipeline VulkanBackend::CreateDrawPipeline(const PsoKey& k, const Spirv& vs, const Spirv& ps, const Spirv* gs,
                                             const Spirv* hs, const Spirv* dms) const {
    // vertex attributes at their registers' locations, each buffer's stride set as it is bound
    VkVertexInputAttributeDescription attr[16];
    VkVertexInputBindingDescription bind[16];
    uint32_t na = 0;
    uint32_t nb = 0;
    uint32_t seen = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        uint32_t buffer = (e >> 8) & 0xFF;
        attr[na++] = {r, buffer, VkFormatOf(e & 0xFF), e >> 16};
        if (!((seen >> buffer) & 1)) {
            seen |= 1u << buffer;
            bind[nb++] = {buffer, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = TopologyClass(k.topo);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    // a point is drawn as its whole console pixel by the geometry shader, solid, front facing and never culled
    bool points = k.topo == uint32_t(Topology::Points);
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe && non_solid_ && !points ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull && !points ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw && !points ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zbias || k.zslope ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(k.samples, 1u));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = Cmp(k.zfunc);
    ds.stencilTestEnable = k.sten;
    ds.front = {StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.back = ds.front;
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = k.blend;
    ba.srcColorBlendFactor = Blend(k.src, false);
    ba.dstColorBlendFactor = Blend(k.dst, false);
    ba.colorBlendOp = BlendOp(k.op);
    ba.srcAlphaBlendFactor = Blend(k.src, true);
    ba.dstAlphaBlendFactor = Blend(k.dst, true);
    ba.alphaBlendOp = BlendOp(k.op);
    ba.colorWriteMask = k.wmask;  // RGBA bits, as KeyBlend packs them
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &ba;
    // the strides only with buffers to bind: a draw without vertex inputs binds none to set them
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn)) - (nb ? 0 : 1);
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormatOf(k.rtfmt);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = k.has_ds ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = k.has_ds && HasStencil(depth_fmt_) ? depth_fmt_ : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    bool patches = k.topo == uint32_t(Topology::Patches);
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = 3;
    if (patches) {
        if (!hs || !dms) return VK_NULL_HANDLE;
        ci.pTessellationState = &ts;
        const Spirv* code[4] = {&vs, &ps, hs, dms};
        const VkShaderStageFlagBits stages[4] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
        return CreatePipeline(ci, code, stages, 4);
    }
    const Spirv* code[3] = {&vs, &ps, gs};
    const VkShaderStageFlagBits stages[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             VK_SHADER_STAGE_GEOMETRY_BIT};
    return CreatePipeline(ci, code, stages, points && gs ? 3 : 2);
}

float VulkanBackend::PointScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, const DrawShaders& sh) {
    if (last_pso_ok_ && std::memcmp(&k, &last_pso_key_, sizeof(k)) == 0) return last_pso_;
    last_pso_ = FindPipeline(k, sh);
    last_pso_key_ = k;
    last_pso_ok_ = true;
    return last_pso_;
}

VkPipeline VulkanBackend::FindPipeline(const PsoKey& k, const DrawShaders& sh) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second;
    PipelineEntry e = MakePipelineEntry(k, sh);
    record_.Add(shader_record::kPipeline, &e, sizeof(e));
    VkPipeline pso = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(warm_.lock);
        auto w = warm_.psos.find(h);
        if (w != warm_.psos.end()) {
            pso = w->second;
            warm_.psos.erase(w);
        }
    }
    if (!pso) {
        uint64_t gh;
        const Spirv* gs = k.topo == uint32_t(Topology::Points) ? Compile(PointHlsl(), "main", "gs_5_0", gh) : nullptr;
        // a point list needs the geometry shader that sizes its points: without it there is no point size
        if (k.topo == uint32_t(Topology::Points) && !gs) {
            psos_[h] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        const Spirv *hs = nullptr, *ds = nullptr;
        if (k.topo == uint32_t(Topology::Patches)) {
            std::string tess = TessHlsl(sh.shading);
            hs = Compile(tess, "hs", "hs_5_0", gh);
            ds = Compile(tess, "ds", "ds_5_0", gh);
        }
        g_perf.psos++;
        auto t = std::chrono::steady_clock::now();
        pso = CreateDrawPipeline(k, *sh.vs_code, *sh.ps_code, gs, hs, ds);
        if (!pso) LOGE("vulkan: pipeline creation failed");
        if (double ms = SinceMs(t); ms > 20) LOGR("vulkan: pipeline creation took %.0f ms", ms);
    }
    psos_[h] = pso;
    return pso;
}

void VulkanBackend::StartWarmUp() {
    // a second instance on this PC shares the files: it reads them only
    record_.Open(app::ExeDir(), kRecordLayout, app::Instance() <= 1);
    std::vector<shader_record::Entry> entries = record_.TakeEntries();
    shader_record::Programs programs = record_.TakePrograms();
    if (!app::Get().shader_warmup || entries.empty()) return;
    warm_.thread = std::thread([this, e = std::move(entries), p = std::move(programs)]() mutable {
        WarmUp(std::move(e), std::move(p));
    });
}

void VulkanBackend::WarmUp(std::vector<shader_record::Entry> entries, shader_record::Programs programs) {
    auto t = std::chrono::steady_clock::now();
    std::vector<const shader_record::Entry*> shaders, pipelines;
    for (const auto& e : entries) (e.kind == shader_record::kPipeline ? pipelines : shaders).push_back(&e);
    std::unordered_map<uint64_t, WarmShader> made;  // by record id
    WarmShaders(shaders, programs, made);
    size_t psos = WarmPipelines(pipelines, made);
    LOGR("shaders: made %zu of %zu recorded shaders and %zu of %zu pipelines ahead in %.0f ms%s", made.size(),
         shaders.size(), psos, pipelines.size(), SinceMs(t), warm_.stop ? ", stopped" : "");
}

// vertex shaders of programs this machine has not run yet are left out: shader_programs.bin has no words for them
void VulkanBackend::WarmShaders(const std::vector<const shader_record::Entry*>& list,
                                const shader_record::Programs& programs,
                                std::unordered_map<uint64_t, WarmShader>& made) {
    std::mutex made_lock;
    ParallelFor(list.size(), warm_.stop, [&](size_t i) {
        const shader_record::Entry& e = *list[i];
        static const std::vector<uint32_t> none;
        std::string src;
        const char* profile = "vs_5_0";
        if (e.kind == shader_record::kVertex && e.bytes.size() == sizeof(shader_record::VertexEntry)) {
            shader_record::VertexEntry v;
            std::memcpy(&v, e.bytes.data(), sizeof(v));
            auto p = programs.find(v.program);
            if (v.program && p == programs.end()) return;
            src = VertexHlsl(v, v.program ? p->second : none);
        } else if (e.kind == shader_record::kPixel && e.bytes.size() == sizeof(PixelKey)) {
            PixelKey pk;
            std::memcpy(&pk, e.bytes.data(), sizeof(pk));
            src = PixelHlsl(pk);
            profile = "ps_5_0";
        } else {
            return;
        }
        Spirv code;
        std::string err;
        if (!CompileSpirv(src, "main", profile, code, err)) return;
        uint64_t hash = SourceHash(src, "main", profile);
        auto ref = std::make_shared<const Spirv>(std::move(code));
        {
            std::lock_guard<std::mutex> lock(warm_.lock);
            warm_.blobs.emplace(hash, ref);
        }
        std::lock_guard<std::mutex> lock(made_lock);
        made[e.id] = {hash, std::move(ref)};
    });
}

// the pipelines whose shaders WarmShaders made, how many were made
size_t VulkanBackend::WarmPipelines(const std::vector<const shader_record::Entry*>& list,
                                    const std::unordered_map<uint64_t, WarmShader>& made) {
    Spirv gs;
    std::string err;
    bool have_gs = CompileSpirv(PointHlsl(), "main", "gs_5_0", gs, err);
    std::atomic<size_t> n{0};
    ParallelFor(list.size(), warm_.stop, [&](size_t i) {
        const shader_record::Entry& e = *list[i];
        if (e.bytes.size() != sizeof(PipelineEntry)) return;
        PipelineEntry p;
        std::memcpy(&p, e.bytes.data(), sizeof(p));
        auto vs = made.find(p.vs_id);
        auto ps = made.find(p.ps_id);
        if (vs == made.end() || ps == made.end()) return;
        PsoKey k;
        std::memcpy(&k, &p.k, sizeof(k));  // padding included, as EnsurePipeline hashes it
        k.vs = vs->second.hash;
        k.ps = ps->second.hash;
        bool points = k.topo == uint32_t(Topology::Points);
        if (points && !have_gs) return;
        if (k.topo == uint32_t(Topology::Patches)) return;  // made as drawn: the record keeps no shading entry
        VkPipeline pso = CreateDrawPipeline(k, *vs->second.code, *ps->second.code, points ? &gs : nullptr);
        if (!pso) return;
        std::lock_guard<std::mutex> lock(warm_.lock);
        if (!warm_.psos.emplace(Hash(&k, sizeof(k)), pso).second) {
            vkDestroyPipeline(dev_, pso, nullptr);
            return;
        }
        n++;
    });
    return n;
}

Image* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                        DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    stages.rt[stage] = true;
    Target* rt = FindTarget(s.rt, false);
    // half a texel back only for a target read at its own size: a pass resizing it (Conker's depth of field shrinks
    // the frame twice and stretches it back) lines up as drawn, half a texel per pass leaving the blur pixels off
    stages.exact[stage] = rt && (t.gw != rt->gw || t.gh != rt->gh);
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    // left bound but never read (particles over a pass that read the frame): the same shader, no copy or resolve
    if (!((stages.sampled >> stage) & 1)) return nullptr;
    // the console reads the pixels it is about to overwrite: give the draw a copy, one for all its stages
    if (feedback) {
        if (!t.fb || t.fb_draw != draw_serial_) {
            t.fb_draw = draw_serial_;
            if (!Feedback(t)) return nullptr;
        }
        stages.view[stage] = t.fb->view;
        return t.fb.get();
    }
    ResolveSamples(t);
    Transition(*t.res, kRead);
    stages.view[stage] = t.res->Sampled();
    return t.res.get();
}

// false when the texture could not be made, its entry dropped
bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) Retire(tx.res);
    tx = Tex{};
    tx.d = d;
    // a DXT texture smaller than a block keeps its size: padded to 4x4, texcoords 0..1 would stretch over the
    // padding (the multiplayer menu's 2x2 scanline mask faded out halfway)
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    ImageSpec sp;
    switch (fi.host) {
        case Host::BC1: sp.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
        case Host::BC2: sp.fmt = VK_FORMAT_BC2_UNORM_BLOCK; break;
        case Host::BC3: sp.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case Host::R32F: sp.fmt = shadow_fmt_; break;
        default: sp.fmt = kBgra; break;
    }
    sp.w = w;
    sp.h = h;
    sp.d = d.depth > 1 ? d.depth : 1;
    sp.levels = tx.levels;
    sp.layers = d.cube ? 6 : 1;
    sp.type = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    tx.res = NewImage(sp);
    if (!tx.res) {
        textures_.erase(key);
        return false;
    }
    return true;
}

// one face's level: the decoded rows, cut or zero filled to the level's
void VulkanBackend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                       uint32_t pitch) {
    const Image& im = *tx.res;
    uint32_t w = std::max(1u, im.w >> l);
    uint32_t h = std::max(1u, im.h >> l);
    uint32_t slices = std::max(1u, im.d >> l);
    bool block = IsBlock(im.format);
    uint32_t rows = block ? (h + 3) / 4 : h;
    uint64_t row_size = uint64_t(block ? (w + 3) / 4 : w) * TexelBytes(im.format);
    Alloc a = Upload(row_size * rows * slices, 16);
    size_t src_rows = pitch ? px.size() / pitch / slices : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row_size;
            size_t n = y < src_rows ? std::min<size_t>(pitch, row_size) : 0;
            if (n) std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], n);
            std::memset(dst + n, 0, size_t(row_size - n));
        }
    }
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VkImageAspectFlags(im.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT), l, f, 1};
    r.imageExtent = {w, h, slices};
    vkCmdCopyBufferToImage(cmd_, a.buf, im.image, kDst, 1, &r);
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    Transition(*tx.res, kDst);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (OffsetSigned(d.format))
                for (uint8_t& b : px) b ^= 0x80;
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(*tx.res, kRead);
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
Image* VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                       DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes. A texture back after a frame or more
    // unbound has every byte hashed: the title may have loaded another in its place, unlocked, and a sample misses
    // one alike but for a corner (Berri's blink atlas, clean, kept the bloodied one's picture from a later level)
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        bool idle = tx.frame != presents_;
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t mark = DirtyEpoch(d.data) ^ palette.hash;
        uint64_t sig = MemSig(d.data, total) ^ mark;
        uint64_t whole = total > kSigWholeMax && idle && same ? Hash(p, total) ^ mark : 0;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || (whole && whole != tx.whole) || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            tx.whole = total > kSigWholeMax ? Hash(p, total) ^ mark : 0;
            UploadTexture(tx, d, p, palette.va ? palette.colours : nullptr);
        }
    }
    stages.view[stage] = tx.res->view;
    stages.levels[stage] = tx.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (OffsetSigned(d.format)) info.flags |= kTexOffset;
    return tx.res.get();
}

Image* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.view[stage] = VK_NULL_HANDLE;
    stages.levels[stage] = 1;
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.exact[stage] = false;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    // nor is a texture the title loaded over an old target's memory without a lock: Conker moves its environment map
    // targets about the heap, and a fur shell's A8L8 texture later laid where one had been drew that picture, black
    // dots in the fur. A target not drawn lately gives way to a texture it can't be, of another size or a format no
    // surface takes; its memory changing proves nothing, the environment maps go unredrawn for frames and still are
    auto surface_format = [](uint32_t f) {
        switch (f) {
            case 0x02: case 0x03: case 0x05: case 0x06: case 0x07:
            case 0x10: case 0x11: case 0x12: case 0x1C: case 0x1E: return true;
            default: return false;
        }
    };
    if (tit != targets_.end() && !tit->second.depth && TargetStale(d.data) &&
        (tit->second.gw != d.width || tit->second.gh != d.height || !surface_format(d.format))) {
        ReleaseTarget(tit);
        tit = targets_.end();
    }
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    // a depth buffer read through a 32 bit colour format: the GPU holds it, guest memory does not
    if (tit == targets_.end() && !fi.depth && !fi.compressed && fi.bits == 32 && !d.cube && d.depth == 1) {
        auto zit = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
        if (zit != targets_.end() && zit->second.res) {
            // depth is data, not a picture: half a texel back reads the neighbour's depth, and Conker's depth of
            // field left a sharp line of background along the right edges of what it keeps in focus
            stages.rt[stage] = true;
            stages.exact[stage] = true;
            info.kind = kTex2D;
            info.flags |= uint8_t(kTexDepthBytes | DepthByteLayout(d.format) << kTexDepthLayoutShift);
            if (!((stages.sampled >> stage) & 1)) return nullptr;
            if (Image* r = DepthBytes(zit->second, d.format)) {
                stages.view[stage] = r->view;
                return r;
            }
            stages.rt[stage] = false;
            info = {};
            info.kind = TextureKind(d);
        }
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

VkSampler VulkanBackend::MakeSampler(const SamplerDesc& sd) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = VkFilter(sd.mag);
    ci.minFilter = VkFilter(sd.min);
    ci.mipmapMode = VkSamplerMipmapMode(sd.mip);
    ci.addressModeU = VkSamplerAddressMode(sd.u);
    ci.addressModeV = VkSamplerAddressMode(sd.v);
    ci.addressModeW = VkSamplerAddressMode(sd.w);
    float max_bias = props_.limits.maxSamplerLodBias;
    ci.mipLodBias = std::clamp(sd.bias, -max_bias, max_bias);
    ci.anisotropyEnable = sd.aniso > 1 && aniso_;
    ci.maxAnisotropy = std::min(float(sd.aniso), props_.limits.maxSamplerAnisotropy);
    ci.compareEnable = sd.compare != 0;
    ci.compareOp = sd.compare ? VkCompareOp(sd.compare - 1) : VK_COMPARE_OP_NEVER;
    ci.minLod = sd.min_lod;
    ci.maxLod = std::max(sd.max_lod, sd.min_lod);
    // the border colour: Vulkan's three fixed ones when it is one of them, else a custom one where the GPU has them
    const float* b = sd.border;
    bool border = sd.u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || sd.v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                  sd.w == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    bool black = b[0] == 0 && b[1] == 0 && b[2] == 0;
    bool white = b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1;
    ci.borderColor = black && b[3] == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : white            ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                     : black            ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                     : b[0] + b[1] + b[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                 : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    bool fixed = (black && (b[3] == 0 || b[3] == 1)) || white;
    if (border && !fixed && custom_border_ && custom_borders_ < custom_border_max_) {
        std::memcpy(custom.customBorderColor.float32, b, sizeof(float) * 4);
        custom.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &custom;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_borders_++;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &ci, nullptr, &s);
    return s;
}

VkSampler VulkanBackend::EnsureSampler(const SamplerDesc& sd) {
    for (const LastSampler& l : last_smp_)
        if (l.smp && SameBytes(l.sd, sd)) return l.smp;
    VkSampler smp = FindSampler(sd);
    last_smp_[last_smp_next_++ % last_smp_.size()] = {sd, smp};
    return smp;
}

VkSampler VulkanBackend::FindSampler(const SamplerDesc& sd) {
    uint64_t key = Hash(&sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (samplers_.size() >= kSamplersKept) {
        // the cache starts over, the old ones go once the frames that bound them are done
        std::vector<VkSampler> old;
        for (auto& [k, s] : samplers_) old.push_back(s);
        auto release = std::make_shared<Deferred>();
        release->release = [dev = dev_, old] {
            for (VkSampler s : old) vkDestroySampler(dev, s, nullptr);
        };
        Retire(std::move(release));
        samplers_.clear();
        last_smp_ = {};
        custom_borders_ = 0;
    }
    VkSampler s = MakeSampler(sd);
    samplers_[key] = s;
    return s;
}

bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    auto perf_t = PerfClock::now();
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    stages.sampled = SampledStages(s);
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    g_perf.bind += PerfMs(perf_t);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

// sets sh.vh, sh.vid and sh.vs_code, compiling the shader on a cache miss
void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (last_vs_.src && last_vs_.key == sh.vkey) {
        sh.vs_code = last_vs_.code;
        sh.vh = last_vs_.src->hash;
        sh.vid = last_vs_.src->id;
        sh.reads |= last_vs_.src->reads;
        return;
    }
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second.hash].get();
        sh.vh = vit->second.hash;
        sh.vid = vit->second.id;
        sh.reads |= vit->second.reads;
        last_vs_ = {sh.vkey, &vit->second, sh.vs_code};
        return;
    }
    shader_record::VertexEntry v = MakeVertexEntry(s, a, sh);
    std::string src = VertexHlsl(v, DrawProgram(s, sh));
    sh.vs_code = Compile(src, "main", "vs_5_0", sh.vh);
    sh.vid = shader_record::Id(shader_record::kVertex, &v, sizeof(v));
    ConstReads reads = ReadsOf(src);
    sh.reads |= reads;
    shader_src_[sh.vkey] = {sh.vh, sh.vid, reads};
    if (!sh.vs_code) return;
    record_.Add(shader_record::kVertex, &v, sizeof(v));
    if (sh.prog) record_.AddProgram(s.vs->hash, s.vs->program);
}

// sets sh.ph, sh.pid and sh.ps_code, compiling the shader on a cache miss
void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    if (last_ps_.src && last_ps_.key == sh.pkey) {
        sh.ps_code = last_ps_.code;
        sh.ph = last_ps_.src->hash;
        sh.pid = last_ps_.src->id;
        sh.reads |= last_ps_.src->reads;
        return;
    }
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second.hash].get();
        sh.ph = pit->second.hash;
        sh.pid = pit->second.id;
        sh.reads |= pit->second.reads;
        last_ps_ = {sh.pkey, &pit->second, sh.ps_code};
        return;
    }
    std::string src = PixelHlsl(pk);
    sh.ps_code = Compile(src, "main", "ps_5_0", sh.ph);
    sh.pid = shader_record::Id(shader_record::kPixel, &pk, sizeof(pk));
    ConstReads reads = ReadsOf(src);
    sh.reads |= reads;
    shader_src_[sh.pkey ^ kPixelKeySalt] = {sh.ph, sh.pid, reads};
    if (sh.ps_code) record_.Add(shader_record::kPixel, &pk, sizeof(pk));
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? DxgiOf(bound_rt_->fmt) : 0;
    k.samples = bound_ms_ ? msaa_ : 1;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k,
                                    const DrawTextures& stages, GuestExtent size, DrawConstants& c, Bindings& b) {
    // the last draw's constants again when this one reads the same: a level drawn in thousands of small draws
    // changing a register they don't read would otherwise fill the ring and wait for the GPU every frame
    bool same_vc = last_vc_ok_;
    for (size_t i = 0; same_vc && i < kConstRegs; i++)
        same_vc = !sh.reads[i] || std::memcmp(&s.vsc[i], &last_vsc_[i], sizeof(s.vsc[i])) == 0;
    if (!same_vc) {
        last_vc_ = Upload(sizeof(s.vsc.v));
        std::memcpy(last_vc_.cpu, s.vsc.data(), sizeof(s.vsc.v));
        last_vsc_ = s.vsc.v;
        last_vc_ok_ = true;
    }
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) {
        c.surf[3] = PointScale();
        FillPointConstants(s, sh.prog, &s == &sprite_, c);
    }
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages.rt, stages.exact, stages.k, stages.wide, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (sh.shading) FillShadingConstants(s, sh.shading, c);
    if (sh.shading && g_program_shading[sh.shading - 1].field) b.view[4] = FieldView(sh.shading);
    if (sh.shading && g_program_shading[sh.shading - 1].screen) b.view[4] = ScreenView();
    if (!last_dc_ok_ || std::memcmp(&c, &last_c_, sizeof(c)) != 0) {
        last_dc_ = Upload(sizeof(DrawConstants));
        std::memcpy(last_dc_.cpu, &c, sizeof(c));
        last_c_ = c;
        last_dc_ok_ = true;
    }
    b.cb[0] = {last_vc_.buf, last_vc_.off, sizeof(s.vsc.v)};
    b.cb[1] = {last_dc_.buf, last_dc_.off, sizeof(DrawConstants)};
}

void VulkanBackend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        Image* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res->shared_from_this(), stages.view[i], d.data, d.width, d.height});
        }
    }
}

// the draw's line: its state, then its textures
void VulkanBackend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k,
                                const DrawConstants& c, const DrawTextures& stages) {
    CaptureDrawLine(cap_, cap_i_++, bound_ds_ ? s.zs.data : 0, s, prim, count, k, c);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants; then ends the draw's line
void VulkanBackend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    if (count <= kQuadVertices) CaptureInputs(cap_, s, a, c);
    fputc('\n', cap_);
}

// each shader once, as translated
void VulkanBackend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = OpenFile(cap_dir_ / name, "w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void VulkanBackend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    CaptureShader("vs", sh.vkey, VertexHlsl(MakeVertexEntry(s, a, sh), DrawProgram(s, sh)));
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

// a stage without a texture reads a dummy of its kind; the samplers read the views' levels
void VulkanBackend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, const DrawTextures& stages,
                                               Bindings& b) {
    bool clamp = UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) {
        const TexInfo& info = stages.info[i];
        VkImageView v = stages.res[i] ? stages.view[i] : VK_NULL_HANDLE;
        if (!v) {
            v = info.shadow             ? dummy_depth_->Sampled()
                : info.kind == kTexCube ? dummy_cube_->view
                : info.kind == kTex3D   ? dummy_3d_->view
                                        : dummy_2d_->view;
        }
        b.view[i] = v;
        b.smp[i] = EnsureSampler(StageSampler(s, i, stages, clamp, aniso));
    }
    if (drawn_.bound && SameBytes(b, drawn_.b)) return;
    FlushRun();
    PushRaw(b);
    drawn_.b = b;
    drawn_.bound = true;
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    float sc = W / gw;
    int32_t x0 = int32_t(s.viewport.x * sc);
    int32_t y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
    int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp<int32_t>(x0, 0, int32_t(W));
    y0 = std::clamp<int32_t>(y0, 0, int32_t(H));
    x1 = std::clamp<int32_t>(x1, 0, int32_t(W));
    y1 = std::clamp<int32_t>(y1, 0, int32_t(H));
    // as SetViewport and SetScissor make them
    VkViewport vp{0, H, W, -H, 0, 1};
    VkRect2D r{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    if (drawn_.view && std::memcmp(&vp, &drawn_.vp, sizeof(vp)) == 0 && std::memcmp(&r, &drawn_.sc, sizeof(r)) == 0)
        return;
    FlushRun();
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &r);
    drawn_.vp = vp;
    drawn_.sc = r;
    drawn_.view = true;
}

void VulkanBackend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (drawn_.fixed && std::memcmp(bf, drawn_.bf, sizeof(bf)) == 0 && ref == drawn_.sref) return;
    FlushRun();
    vkCmdSetBlendConstants(cmd_, bf);
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
    std::memcpy(drawn_.bf, bf, sizeof(bf));
    drawn_.sref = ref;
    drawn_.fixed = true;
}

// none without vertex inputs: that pipeline takes no strides (CreateDrawPipeline)
void VulkanBackend::UploadVertexBuffers(const Assembled& a) {
    if (!a.buffer_count || !a.regs) return;
    VkBuffer bufs[16];
    VkDeviceSize offs[16];
    VkDeviceSize sizes[16];
    VkDeviceSize strides[16];
    // past the vertices, zeros: a three component element is read as four (SHORT3, NORMSHORT3, PBYTE3), and the last
    // vertex's reads past its end
    constexpr uint64_t kSlack = 8;
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint64_t bytes = std::max<uint64_t>(b.bytes, 16) + kSlack;  // a binding is never empty
        Alloc vb = Upload(bytes, 16);
        if (b.bytes) std::memcpy(vb.cpu, b.src, b.bytes);
        std::memset(vb.cpu + b.bytes, 0, bytes - b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        sizes[i] = bytes;
        strides[i] = b.stride;
    }
    vkCmdBindVertexBuffers2(cmd_, 0, a.buffer_count, bufs, offs, sizes, strides);
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    BindGeometry(a);
    IssueGeometry(a);
}

// a list (not a strip, not patches) whose every binding holds the same number of whole vertices, the draw's own
bool VulkanBackend::Mergeable(const Assembled& a) const {
    if (!a.buffer_count || !a.regs || a.strip || a.topology == Topology::Patches) return false;
    uint32_t v = 0;
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        if (!b.stride || b.bytes % b.stride) return false;
        if (i && b.bytes / b.stride != v) return false;
        v = b.bytes / b.stride;
    }
    return v != 0;
}

// a direct run's vertices (Run::direct) grown by b's, in place: false when the ring took something else since
constexpr uint64_t kRunSlack = 8;  // as UploadVertexBuffers: a vertex's reads past its end
bool VulkanBackend::GrowRun(const VertexBuffer& b) {
    Frame& f = CurrentFrame();
    uint64_t had = std::max<uint64_t>(run_.bytes, 16) + kRunSlack;
    uint64_t now = std::max<uint64_t>(run_.bytes + b.bytes, 16) + kRunSlack;
    if (!run_.draws) {
        run_.vb = Upload(now, 16);
    } else {
        if (run_.vb.buf != f.upload->buf || f.used != run_.vb.off + had || run_.vb.off + now > kUploadSize) return false;
        f.used = run_.vb.off + now;
        g_perf.upload += now - had;
    }
    if (b.bytes) std::memcpy(run_.vb.cpu + run_.bytes, b.src, b.bytes);
    run_.bytes += b.bytes;
    std::memset(run_.vb.cpu + run_.bytes, 0, now - run_.bytes);
    return true;
}

void VulkanBackend::JoinRun(const Assembled& a) {
    constexpr uint32_t kRunVertices = 1u << 18;
    uint32_t v = a.buffers[0].bytes / a.buffers[0].stride;
    int topo = int(PrimitiveTopology(a));
    bool direct = a.buffer_count == 1;
    bool fits = run_.draws && run_.buffers == a.buffer_count && run_.topo == topo && run_.vertices + v <= kRunVertices;
    for (uint32_t i = 0; fits && i < a.buffer_count; i++) fits = run_.strides[i] == a.buffers[i].stride;
    // a direct run the ring moved on from starts again: the same draws, in one more
    if (fits && direct && !GrowRun(a.buffers[0])) fits = false;
    if (!fits) {
        FlushRun();
        run_.first = pass_serial_;
        run_.buffers = a.buffer_count;
        run_.topo = topo;
        for (uint32_t i = 0; i < a.buffer_count; i++) run_.strides[i] = a.buffers[i].stride;
        run_.direct = direct;
        run_.bytes = 0;
        if (direct) GrowRun(a.buffers[0]);
    }
    uint32_t base = run_.vertices;
    for (uint32_t i = 0; !direct && i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        run_.data[i].insert(run_.data[i].end(), b.src, b.src + b.bytes);
    }
    if (a.index_count) {
        for (uint32_t k = 0; k < a.index_count; k++) {
            uint32_t idx;
            if (a.index16) {
                uint16_t x;
                std::memcpy(&x, static_cast<const uint8_t*>(a.index_src) + k * 2, 2);
                idx = x;
            } else {
                std::memcpy(&idx, static_cast<const uint8_t*>(a.index_src) + k * 4, 4);
            }
            run_.indices.push_back(uint32_t(int64_t(idx) + a.base_vertex) + base);
        }
    } else {
        for (uint32_t k = 0; k < a.draw_count; k++) run_.indices.push_back(base + k);
    }
    run_.vertices += v;
    run_.draws++;
    run_.last = pass_serial_;
}

// the run's geometry in one draw, with the bindings its draws left
void VulkanBackend::FlushRun() {
    if (!run_.draws) return;
    run_.draws = 0;  // first: what this records must not come back here
    g_perf.runs++;
    VkBuffer bufs[16];
    VkDeviceSize offs[16], sizes[16], strides[16];
    for (uint32_t i = 0; run_.direct && i < 1; i++) {
        bufs[i] = run_.vb.buf;
        offs[i] = run_.vb.off;
        sizes[i] = std::max<uint64_t>(run_.bytes, 16) + kRunSlack;
        strides[i] = run_.strides[i];
    }
    for (uint32_t i = 0; !run_.direct && i < run_.buffers; i++) {
        const std::vector<uint8_t>& d = run_.data[i];
        uint64_t bytes = std::max<uint64_t>(d.size(), 16) + kRunSlack;
        Alloc vb = Upload(bytes, 16);
        std::memcpy(vb.cpu, d.data(), d.size());
        std::memset(vb.cpu + d.size(), 0, bytes - d.size());
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        sizes[i] = bytes;
        strides[i] = run_.strides[i];
        run_.data[i].clear();
    }
    vkCmdBindVertexBuffers2(cmd_, 0, run_.buffers, bufs, offs, sizes, strides);
    if (run_.topo != drawn_.topo) {
        vkCmdSetPrimitiveTopology(cmd_, VkPrimitiveTopology(run_.topo));
        drawn_.topo = run_.topo;
    }
    uint32_t ibytes = uint32_t(run_.indices.size() * 4);
    Alloc ib = Upload(ibytes, 4);
    std::memcpy(ib.cpu, run_.indices.data(), ibytes);
    vkCmdBindIndexBuffer(cmd_, ib.buf, ib.off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd_, uint32_t(run_.indices.size()), 1, 0, 0, 0);
    if (!replay_) {
        KeptRun& k = kept_runs_.emplace_back();
        k.first = run_.first;
        k.last = run_.last;
        k.buffers = run_.buffers;
        k.count = uint32_t(run_.indices.size());
        k.topo = run_.topo;
        std::copy(bufs, bufs + run_.buffers, k.bufs);
        std::copy(offs, offs + run_.buffers, k.offs);
        std::copy(sizes, sizes + run_.buffers, k.sizes);
        std::copy(strides, strides + run_.buffers, k.strides);
        k.ib = ib.buf;
        k.ioff = ib.off;
        std::copy(drawn_.b.view, drawn_.b.view + kBindViews, k.view);
        std::copy(drawn_.b.smp, drawn_.b.smp + kBindViews, k.smp);
    }
    run_.indices.clear();
    run_.vertices = 0;
}

// The draws of a run all followed one draw (gpu/interp.h: same state, same constants), so drawn again they are all
// blended alike and stay one run: its geometry, already in the ring, is drawn with the blended constants and the
// rest of its draws are skipped (skip_to_). The checks are FastDraw's but those on the geometry, which is the frame's
bool VulkanBackend::ReuseRun(const State& s) {
    while (kept_run_next_ < kept_runs_.size() && kept_runs_[kept_run_next_].first < pass_serial_) kept_run_next_++;
    if (kept_run_next_ >= kept_runs_.size() || kept_runs_[kept_run_next_].first != pass_serial_) return false;
    const KeptRun& r = kept_runs_[kept_run_next_];
    if (!last_vc_ok_ || !last_dc_ok_ || kept_.b.cb[0].buffer != last_vc_.buf || kept_.b.cb[0].offset != last_vc_.off ||
        kept_.b.cb[1].buffer != last_dc_.buf || kept_.b.cb[1].offset != last_dc_.off)
        return false;
    for (size_t i = 0; i < kConstRegs; i++)
        if (kept_.reads[i] && std::memcmp(&s.vsc[i], &last_vsc_[i], sizeof(s.vsc[i])) != 0) return false;
    if (CurrentFrame().used > kUploadSize - kUploadReserve) return false;
    if (bound_rt_ != kept_.rt || bound_ds_ != kept_.ds || bound_ms_ != kept_.ms || (!bound_rt_ && !bound_ds_))
        return false;
    FlushRun();
    EnsureRendering();
    if (kept_.pso != drawn_.pso) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, kept_.pso);
        drawn_.pso = kept_.pso;
    }
    Bindings b = kept_.b;
    std::copy(r.view, r.view + kBindViews, b.view);
    std::copy(r.smp, r.smp + kBindViews, b.smp);
    if (!drawn_.bound || !SameBytes(b, drawn_.b)) {
        PushRaw(b);
        drawn_.b = b;
        drawn_.bound = true;
    }
    SetViewportScissor(s, kept_.size.w);
    SetBlendFactorStencilRef(s);
    vkCmdBindVertexBuffers2(cmd_, 0, r.buffers, r.bufs, r.offs, r.sizes, r.strides);
    if (r.topo != drawn_.topo) {
        vkCmdSetPrimitiveTopology(cmd_, VkPrimitiveTopology(r.topo));
        drawn_.topo = r.topo;
    }
    vkCmdBindIndexBuffer(cmd_, r.ib, r.ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd_, r.count, 1, 0, 0, 0);
    skip_to_ = r.last;
    kept_run_next_++;
    g_perf.reused++;
    return true;
}

void VulkanBackend::BindGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    if (int topo = int(PrimitiveTopology(a)); topo != drawn_.topo) {
        vkCmdSetPrimitiveTopology(cmd_, VkPrimitiveTopology(topo));
        drawn_.topo = topo;
    }
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(cmd_, ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
}

// the bound geometry drawn again
void VulkanBackend::IssueGeometry(const Assembled& a) {
    if (a.index_count)
        vkCmdDrawIndexed(cmd_, a.index_count, 1, 0, a.base_vertex, 0);
    else
        vkCmdDraw(cmd_, a.draw_count, 1, 0, 0);
}

// ProgramShading::between: the shells between the draw before in the run and this one, its geometry bound and drawn
// with vertex constants of their own, each followed by its depth pass when there is one (zpso, then pso bound back),
// then the draw's own bindings pushed back; false, nothing done, without any
bool VulkanBackend::DrawBetweenShells(const State& s, const Assembled& a, uint32_t shading, const Bindings& b,
                                      VkPipeline pso, VkPipeline zpso) {
    const ProgramShading& ps = g_program_shading[shading - 1];
    if (ps.between <= 0) return false;
    float from = ShadingBefore(shading), to = s.vsc[size_t(ps.top)][0];
    if (from < 0 || to <= from) return false;
    BindGeometry(a);
    Bindings eb = b;
    for (int j = 1; j <= ps.between; j++) {
        Alloc vc = Upload(sizeof(s.vsc.v));
        std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc.v));
        float x = from + (to - from) * float(j) / float(ps.between + 1);
        std::memcpy(vc.cpu + size_t(ps.top) * sizeof(s.vsc[0]), &x, sizeof(x));
        eb.cb[0] = {vc.buf, vc.off, sizeof(s.vsc.v)};
        PushRaw(eb);
        IssueGeometry(a);
        if (zpso) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, zpso);
            IssueGeometry(a);
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        }
    }
    PushRaw(b);  // drawn_.b still holds it
    return true;
}

// Assemble, timed and counted
bool VulkanBackend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                 const uint8_t* indices, Assembled& a, bool patches) {
    auto perf_t = PerfClock::now();
    bool assembled = Assemble(s, prim, start, count, indices, a, patches);
    g_perf.assemble += PerfMs(perf_t);
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool VulkanBackend::EnsureQueries() {
    if (query_pool_) return true;
    if (queries_failed_) return false;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kQueries;
    if (vkCreateQueryPool(dev_, &qi, nullptr, &query_pool_) != VK_SUCCESS) {
        query_pool_ = VK_NULL_HANDLE;
        queries_failed_ = true;
        LOG_FIRST(1, "vulkan: no occlusion queries, visibility tests stay pending");
        return false;
    }
    vkResetQueryPool(dev_, query_pool_, 0, kQueries);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise; the query is
// begun inside the draw's render pass and ended there
uint32_t VulkanBackend::BeginCountedDraw(const State& s) {
    if (replay_ || !s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    frame_counted_++;
    // its last use was frames ago, done by now
    vkResetQueryPool(dev_, query_pool_, query, 1);
    vkCmdBeginQuery(cmd_, query_pool_, query, precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    return query;
}

void VulkanBackend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    vkCmdEndQuery(cmd_, query_pool_, query);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float VulkanBackend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(bound_ms_ ? msaa_ : 1);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void VulkanBackend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = Completed();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) {
            uint64_t n = 0;
            vkGetQueryPoolResults(dev_, query_pool_, q.index, 1, sizeof(n), &n, sizeof(n), VK_QUERY_RESULT_64_BIT);
            pixels += double(n) / q.samples_per_pixel;
        }
        // XBE_REPORTS_VISIBLE: every test passes, an A/B switch for culling problems
        static const bool all_visible = std::getenv("XBE_REPORTS_VISIBLE") != nullptr;
        if (all_visible) pixels = 640.0 * 480.0;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

bool VulkanBackend::PostWanted() const {
    const app::Settings& set = app::Get();
    return set.smaa || set.film_grain || set.chromatic_aberration;
}

bool VulkanBackend::ScreenSpaceDraw(const State& s, const Assembled& a) {
    int pos_w = kPosWUnknown;
    if ((s.vs_handle & 1) && s.vs && !s.vs->program.empty()) {
        EnsureProgramHash(s);
        auto [it, fresh] = pos_w_.try_emplace(s.vs->hash, kPosWUnknown);
        if (fresh) it->second = PositionWSource(s.vs->program);
        pos_w = it->second;
    }
    return ScreenSpace(s, a, pos_w);
}

// stage 0 samples a target: the title's own passes over the frame (bloom, depth of field), which stay under the effects
bool VulkanBackend::ReadsTarget(const State& s) {
    TextureDesc d;
    if (!s.textures[0] || !Describe(s.textures[0], d)) return false;
    return targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height)) != 0;
}

// a 3D draw onto the frame-sized target marks it; the first screen space one after that draws the effects under it
void VulkanBackend::TrackPost(const State& s, bool flat) {
    if (post_done_ || !PostWanted()) return;
    PostTrace& pt = post_trace_;
    Target* rt = FindTarget(s.rt, false);
    if (!rt) {
        pt.no_target++;
        return;
    }
    if (rt->gw != guest_w_ || rt->gh != guest_h_) {
        if (!flat) {
            pt.other++;
            pt.other_rt = rt->data;
            pt.ow = rt->gw;
            pt.oh = rt->gh;
        }
        return;
    }
    if (!flat) {
        pt.solid++;
        post_rt_ = rt->data;
        return;
    }
    if (post_rt_ != rt->data) {
        pt.flat_elsewhere++;
        return;
    }
    if (ReadsTarget(s)) {
        pt.flat_reads++;
        return;
    }
    pt.applied = "at a 2D draw";
    ApplyPost(*rt);
}

// SMAA's lookups as BGRA textures, made the first time SMAA runs
bool VulkanBackend::EnsureSmaaTextures() {
    if (!smaa_area_) {
        std::vector<uint8_t> px(size_t(kSmaaAreaW) * kSmaaAreaH * 4, 0xFF);
        for (size_t i = 0; i < size_t(kSmaaAreaW) * kSmaaAreaH; i++) {
            px[i * 4 + 0] = 0;
            px[i * 4 + 1] = kSmaaArea[i * 2 + 1];
            px[i * 4 + 2] = kSmaaArea[i * 2];
        }
        smaa_area_ = CreatePixelTexture(px, kSmaaAreaW * 4, kSmaaAreaW, kSmaaAreaH);
    }
    if (!smaa_search_) {
        std::vector<uint8_t> px(size_t(kSmaaSearchW) * kSmaaSearchH * 4, 0xFF);
        for (size_t i = 0; i < size_t(kSmaaSearchW) * kSmaaSearchH; i++) {
            px[i * 4 + 0] = px[i * 4 + 1] = 0;
            px[i * 4 + 2] = kSmaaSearch[i];
        }
        smaa_search_ = CreatePixelTexture(px, kSmaaSearchW * 4, kSmaaSearchW, kSmaaSearchH);
    }
    return smaa_area_ && smaa_search_;
}

// the target's picture through SMAA and grade, each pass reading what the one before wrote and the last
// writing the target
void VulkanBackend::ApplyPost(Target& t) {
    post_done_ = true;
    const app::Settings& set = app::Get();
    bool grade = set.film_grain || set.chromatic_aberration;
    bool smaa = set.smaa;
    post_trace_.outcome = "stopped: no SMAA textures";
    if (smaa && !EnsureSmaaTextures()) return;
    post_trace_.outcome = "stopped: no copy of the picture";
    // the picture's copy, SMAA's result when grade follows, then its edges and blending weights
    for (size_t i = 0; i < (smaa ? 4u : 1u); i++) {
        ImageRef& im = post_tmp_[i];
        if (im && im->w == t.w && im->h == t.h && im->format == t.fmt) continue;
        if (im) Retire(im);
        im = NewImage({.fmt = t.fmt, .w = t.w, .h = t.h,
                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT});
        if (!im) return;
    }
    if (cap_) fprintf(cap_, "     note effects over the 3D picture of %08X\n", t.data);
    ResolveSamples(t);
    Transition(*t.res, kSrc);
    Transition(*post_tmp_[0], kDst);
    Barrier();
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cmd_, t.res->image, kSrc, post_tmp_[0]->image, kDst, 1, &r);
    WriteRes(t, true);
    t.drawn = true;
    float p[8] = {1.0f / t.w, 1.0f / t.h, float(t.w), float(t.h),
                  set.chromatic_aberration / 100.0f, set.film_grain / 100.0f, float(presents_ % 1024), 0};
    auto run = [&](const char* hlsl, const char* entry, Image& dst, std::initializer_list<Image*> in) {
        VkImageView views[3] = {};
        size_t i = 0;
        for (Image* im : in) {
            Transition(*im, kRead);
            views[i++] = im->view;
        }
        Pass(hlsl, entry, t.fmt, dst, t.w, t.h, views, p, -1);
    };
    Image* src = post_tmp_[0].get();
    if (smaa) {
        Image& dst = grade ? *post_tmp_[1] : *t.res;
        Image& edges = *post_tmp_[2];
        Image& weights = *post_tmp_[3];
        ClearWhole(edges);  // the edge pass discards where it finds none
        run(SmaaHlsl(0), "edges", edges, {src});
        run(SmaaHlsl(1), "weights", weights, {&edges, smaa_area_.get(), smaa_search_.get()});
        run(SmaaHlsl(2), "blend", dst, {src, &weights});
        src = &dst;
    }
    if (grade) run(kPostHlsl, "grade", *t.res, {src});
    post_trace_.outcome = "drawn";
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (&s != &sprite_) pass_serial_++;
    if (replay_ && pass_serial_ <= skip_to_) return;  // drawn with the run it was in
    if (&s != &sprite_ && FixedSprite(s, prim)) {
        sprite_ = s;
        SpriteStage(sprite_);
        return Draw(sprite_, prim, start, count, indices);
    }
    if (!replay_) WatchDraw(s, start, count, indices);
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    frame_draws_++;
    draw_serial_++;
    if (!s.simple[kMBlendEnable >> 2]) frame_opaque_++;
    if (cap_) {
        CapStep(s.rt);
        CapAround(s);
    }
    bool same = std::exchange(same_state_, false) && &s != &sprite_;
    bool patches = tess_ && TessellatedDraw(s, prim);
    if (same && kept_.ok && !cap_ && !PostWanted() && !(kept_.surface && SurfaceDepth(s, start, count, indices)) &&
        ((replay_ && ReuseRun(s)) || FastDraw(s, prim, start, count, indices, patches))) {
        g_perf.fast++;
        return;
    }
    kept_.ok = false;
    FlushRun();
    auto lap = PerfClock::now();
    if (DepthCopy(s)) return CountedSkip(s, "depth copy");
    Assembled& a = asm_;
    g_perf.d_setup += PerfLap(lap);
    if (!AssembleDraw(s, prim, start, count, indices, a, patches)) return CountedSkip(s, "assemble");
    lap = PerfClock::now();
    bool flat = (cap_ || PostWanted()) && ScreenSpaceDraw(s, a);
    TrackPost(s, flat);
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return CountedSkip(s, "targets");
    g_perf.d_setup += PerfLap(lap);

    // vertex shader
    DrawShaders sh;
    ClassifyShaders(s, a, count, sh);
    if (sh.prog) EnsureProgramHash(s);
    sh.shading = DrawShading(s, sh);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);
    g_perf.d_vs += PerfLap(lap);

    // stage textures, then the pixel shader made for them
    DrawTextures stages{};  // zeroed: KeptDraw compares it whole
    if (!BindStageTexturesAndTargets(s, stages)) return CountedSkip(s, "textures");
    lap = PerfClock::now();  // bind times itself
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    pk.shading = sh.shading;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return CountedSkip(s, "shaders");
    g_perf.d_ps += PerfLap(lap);

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    // ProgramShading::depth_alpha: the depth goes in a pass of its own after the colour, where it isn't faint; and so
    // does a g_surface_depth surface's, which the title draws without
    VkPipeline zpso = VK_NULL_HANDLE;
    const ProgramShading* zps = sh.shading ? &g_program_shading[sh.shading - 1] : nullptr;
    float zalpha = 0;
    int surface = -1;  // g_surface_depth's answer for the draw, -1 not asked
    if (zps && zps->depth_alpha > 0 && k.blend && k.zen && k.zwrite &&
        (zps->top < 0 || s.vsc[size_t(zps->top)][0] > 0))
        zalpha = zps->depth_alpha;
    else if (k.blend && k.zen && !k.zwrite && (surface = SurfaceDepth(s, start, count, indices)))
        zalpha = g_surface_depth.alpha;
    if (zalpha > 0) {
        PixelKey zpk = pk;
        zpk.depth_pass = std::bit_cast<uint32_t>(zalpha);
        DrawShaders zsh = sh;
        zsh.pkey = Hash(&zpk, sizeof(zpk));
        EnsurePixelShader(zpk, zsh);
        if (zsh.ps_code) {
            PsoKey zk = k;
            zk.ps = zsh.ph;
            zk.blend = 0;
            zk.wmask = 0;
            zk.sten = 0;
            zk.zwrite = 1;
            zpso = EnsurePipeline(zk, zsh);
        }
        if (zpso) k.zwrite = 0;
    }
    VkPipeline pso = EnsurePipeline(k, sh);
    if (!pso) return CountedSkip(s, "pipeline");
    g_perf.d_pso += PerfLap(lap);

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    Bindings b;
    UploadConstants(s, sh, k, stages, size, c, b);
    g_perf.d_consts += PerfLap(lap);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        fprintf(cap_, " pkey %016llX%s%s%s%s", (unsigned long long)sh.pkey, flat ? " screen" : "",
                surface < 0 ? "" : surface ? " surface taken" : " surface refused", zpso ? " depth pass" : "",
                DescribeInputs(s, start, indices, 2).c_str());
        fprintf(cap_, "\n     fog chip %u en %u mode %X params %g %g rs en %u table %u | fogp %g %g %g %g color %08X | final %08X %08X",
                s.chip_fog, s.fog_enable, s.fog_mode, s.fog_param[0], s.fog_param[1], s.rs[kRsFogEnable],
                s.rs[kRsFogTableMode], c.fog[0], c.fog[1], c.fog[2], c.fog[3], s.rs[kRsFogColor],
                s.simple[0x288 >> 2], s.simple[0x28C >> 2]);
        // the stages as the pixel shader reads them: texture modes (5 bits per stage) or the fixed function's
        fprintf(cap_, "\n     stages modes %05X sampled %X", s.ps ? s.rs[kRsPsTextureModes] : 0, stages.sampled);
        // bump env map stages (modes 6, 7): the matrix 00 01 10 11, then luminance scale and offset
        for (int i = 1; i < 4; i++) {
            uint32_t mode = ((s.ps ? s.rs[kRsPsTextureModes] : 0) >> (i * 5)) & 0x1F;
            if (mode == 6 || mode == 7)
                fprintf(cap_, " | bump%d %g %g %g %g l %g %g", i, c.bump[i][0], c.bump[i][1], c.bump[i][2],
                        c.bump[i][3], c.bumpl[i][0], c.bumpl[i][1]);
        }
        // points: the render states the renderer draws by, then what the title pushed raw for the same registers
        // (NV097_SET_POINT_PARAMS_ENABLE 0x318, _POINT_SMOOTH_ENABLE 0x31C, _POINT_SIZE 0x43C)
        if (k.topo == uint32_t(Topology::Points))
            fprintf(cap_, "\n     points sprite %u size %g scale %u %g %g %g limits %g %g | raw 318 %X 31C %X 43C %X",
                    s.rs[kRsPointSpriteEnable], c.pts[0], s.rs[kRsPointScaleEnable], c.ptsk[0], c.ptsk[1], c.ptsk[2],
                    c.pts[2], c.pts[3], s.simple[0x318 >> 2], s.simple[0x31C >> 2], s.simple[0x43C >> 2]);
        // the border colour as the title set it (ARGB), with the D3DTADDRESS codes it came with
        for (int i = 0; i < 4; i++) {
            if (!stages.res[i] && i >= 2) continue;
            SamplerDesc sd = StageSampler(s, i, stages, UnitTexcoords(a), app::Get().aniso);
            const auto& t = s.tss[size_t(i)];
            fprintf(cap_,
                    "\n     s%d filter %u %u %u addr %u %u lod %g..%g bias %g aniso %u mips %u | d3d addr %u %u border %08X",
                    i, sd.min, sd.mag, sd.mip, sd.u, sd.v, sd.min_lod, sd.max_lod, sd.bias, sd.aniso, stages.levels[i],
                    t[kTssAddressU], t[kTssAddressV], t[kTssBorderColor]);
        }
        CaptureConstantChanges(cap_, s, cap_vsc_);
        // a program's first draw also lists its constants
        if (count > kQuadVertices && !cap_sh_.count(sh.vkey)) CaptureInputs(cap_, s, a, c);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
        lap = PerfClock::now();
    }

    // the render pass, the pipeline, texture views and samplers, viewport and scissor, blend factor and stencil
    // reference; nothing after this ends the pass before the draw
    EnsureRendering();
    if (pso != drawn_.pso) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
        drawn_.pso = pso;
    }
    SetTextureViewsAndSamplers(s, a, stages, b);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test; a shading's shells between go first, uncounted
    bool bound = sh.shading && DrawBetweenShells(s, a, sh.shading, b, pso, zpso);
    uint32_t query = BeginCountedDraw(s);
    if (bound)
        IssueGeometry(a);
    else
        DrawGeometry(a);
    EndCountedDraw(query);
    if (zpso) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, zpso);
        drawn_.pso = zpso;
        IssueGeometry(a);
    }
    if (!sh.shading && !zpso && &s != &sprite_) {
        KeepDraw(a, sh, stages, pso, b, size);
        kept_.surface = k.blend && k.zen && !k.zwrite;
    }
    g_perf.d_record += PerfLap(lap);
}

void VulkanBackend::KeepDraw(const Assembled& a, const DrawShaders& sh, const DrawTextures& stages, VkPipeline pso,
                             const Bindings& b, GuestExtent size) {
    kept_.ok = true;
    kept_.reads = sh.reads;
    kept_.quad = sh.quad;
    kept_.stages = stages;
    kept_.pso = pso;
    kept_.b = b;
    kept_.size = size;
    kept_.rt = bound_rt_;
    kept_.ds = bound_ds_;
    kept_.ms = bound_ms_;
    kept_.regs = a.regs;
    kept_.decode = a.decode;
    kept_.topology = a.topology;
    kept_.strip = a.strip;
    kept_.pretransformed = a.pretransformed;
    kept_.elements = a.elements;
}

// a draw whose state is the kept draw's but for its vertices and constants: false, nothing recorded, when anything it
// would draw with differs after all, and the whole way decides. Shading programs (fur) are never kept, their constants
// follow the frame. Its targets, textures and depth copy are the kept draw's: in one state nothing but another
// command (Changed) changes them, a guest texture is checked for writes once a frame anyway, and a kept draw was no
// depth copy
bool VulkanBackend::FastDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                             bool patches) {
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a, patches)) {
        CountedSkip(s, "assemble");
        return true;
    }
    auto lap = PerfClock::now();
    // the inputs the shaders and pipeline were made for
    if (a.regs != kept_.regs || a.decode != kept_.decode || a.topology != kept_.topology || a.strip != kept_.strip ||
        a.pretransformed != kept_.pretransformed)
        return false;
    for (int r = 0; r < 16; r++) {
        if (!(a.regs >> r & 1)) continue;
        const VertexElement &x = a.elements[size_t(r)], &y = kept_.elements[size_t(r)];
        if (x.format != y.format || x.buffer != y.buffer || x.offset != y.offset) return false;
    }
    bool quad = count <= kQuadVertices && !s.rs[kRsZEnable] && a.topology != Topology::Points;
    if (quad != kept_.quad) return false;
    // the constants still in the ring, holding what the shaders read
    if (!last_vc_ok_ || !last_dc_ok_ || kept_.b.cb[0].buffer != last_vc_.buf || kept_.b.cb[0].offset != last_vc_.off ||
        kept_.b.cb[1].buffer != last_dc_.buf || kept_.b.cb[1].offset != last_dc_.off)
        return false;
    for (size_t i = 0; i < kConstRegs; i++)
        if (kept_.reads[i] && std::memcmp(&s.vsc[i], &last_vsc_[i], sizeof(s.vsc[i])) != 0) return false;
    if (CurrentFrame().used > kUploadSize - kUploadReserve) return false;
    if (bound_rt_ != kept_.rt || bound_ds_ != kept_.ds || bound_ms_ != kept_.ms || (!bound_rt_ && !bound_ds_))
        return false;
    const DrawTextures& stages = kept_.stages;
    g_perf.d_setup += PerfLap(lap);

    EnsureRendering();
    if (kept_.pso != drawn_.pso) {
        FlushRun();
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, kept_.pso);
        drawn_.pso = kept_.pso;
    }
    Bindings b = kept_.b;
    SetTextureViewsAndSamplers(s, a, stages, b);
    SetViewportScissor(s, kept_.size.w);
    SetBlendFactorStencilRef(s);
    // in the state the draws before it set: its geometry joins theirs, drawn with them later (a ground drawn a quad at
    // a time is thousands of these)
    if (!(s.simple[kMCountEnable >> 2] && !replay_) && Mergeable(a)) {
        JoinRun(a);
    } else {
        FlushRun();
        uint32_t query = BeginCountedDraw(s);
        DrawGeometry(a);
        EndCountedDraw(query);
    }
    g_perf.d_record += PerfLap(lap);
    return true;
}

// a guest rect scaled to the target and clamped to it, empty ones left out
void PushScaledRect(const Target& t, int x0, int y0, int x1, int y1, std::vector<VkClearRect>& rs) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp<int32_t>(int32_t(x0 * sc), 0, int32_t(t.w));
    int32_t tp = std::clamp<int32_t>(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp<int32_t>(int32_t(x1 * sc), 0, int32_t(t.w));
    int32_t b = std::clamp<int32_t>(int32_t(y1 * sc), 0, int32_t(t.h));
    if (r > l && b > tp) rs.push_back({{{l, tp}, {uint32_t(r - l), uint32_t(b - tp)}}, 0, 1});
}

// rs: the rects buffer, reused across targets
void VulkanBackend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<VkClearRect>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            PushScaledRect(t, r[0], r[1], r[2], r[3], rs);
        }
    } else {
        const auto& v = s.viewport;
        PushScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h), rs);
    }
    // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
    if (!t.depth && LatchCpuWrite(t)) UploadTarget(t, s.rt);
    // a multisampled target clears its samples, as a draw would
    if (t.ms) {
        SpreadSamples(t);
        t.ms_ahead = true;
    }
    t.drawn = true;
    VkClearAttachment ca{};
    if (t.depth) {
        ca.aspectMask = ((c.flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((c.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue.depthStencil = {std::clamp(c.z, 0.0f, 1.0f), c.stencil & 0xFF};
    } else {
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        Color(c.color, ca.clearValue.color.float32);
    }
    if (rs.empty() || !ca.aspectMask) return;
    // a pass of its own on the image; the draw's targets begin theirs again after
    Image* im = t.ms ? t.ms.get() : t.res.get();
    Attach saved = desired_;
    SetTargets(t.depth ? nullptr : im, t.depth ? im : nullptr);
    EnsureRendering();
    vkCmdClearAttachments(cmd_, 1, &ca, uint32_t(rs.size()), rs.data());
    desired_ = saved;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                          float z, uint32_t stencil) {
    FlushRun();
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f stencil %02X\n", cap_i_++, s.rt.data,
                s.zs.data, flags, color, z, stencil & 0xFF);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<VkClearRect> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// one Blit per rect: for formats that differ
void VulkanBackend::CopyByDraw(const Target& src_t, Image& src, Target& dst_t, const CopyList& l) {
    WriteRes(dst_t, false);
    Transition(src, kRead);
    SetTargets(dst_t.res.get(), nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        float vx = r.dx * dst_scale;
        float vy = r.dy * dst_scale;
        float vw = (r.x1 - r.x0) * dst_scale;
        float vh = (r.y1 - r.y0) * dst_scale;
        int32_t right = std::min(int32_t(vx + vw), int32_t(dst_t.w));
        int32_t bottom = std::min(int32_t(vy + vh), int32_t(dst_t.h));
        if (right <= std::max(int32_t(vx), 0) || bottom <= std::max(int32_t(vy), 0)) continue;
        SetViewport(vx, vy, vw, vh);
        SetScissor(int32_t(vx), int32_t(vy), right, bottom);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        Blit(src.Sampled(), uv, {.alpha = true, .taps = taps, .rtv = dst_t.fmt});
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one image copy per rect, clamped to both targets
void VulkanBackend::CopyByRegion(const Target& src_t, Image& src, Target& dst_t, const CopyList& l) {
    WriteRes(dst_t, false);
    Transition(src, kSrc);
    Transition(*dst_t.res, kDst);
    Barrier();
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0;
        uint32_t top = 0;
        uint32_t right = std::min(src_t.w, dst_t.w);
        uint32_t bottom = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.srcOffset = {int32_t(left), int32_t(top), 0};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.dstOffset = {int32_t(dx), int32_t(dy), 0};
        r.extent = {right - left, bottom - top, 1};
        vkCmdCopyImage(cmd_, src.image, kSrc, dst_t.res->image, kDst, 1, &r);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    FlushRun();
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    ResolveSamples(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt) {
        // formats differ: drawn across
        CopyByDraw(*src_t, *src_t->res, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, *src_t->res, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

// a new swap chain when the window's size, vsync or the surface changed; none while minimised
void VulkanBackend::ResizeSwap() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return;
    VkExtent2D ext = caps.currentExtent.width == UINT32_MAX ? WindowExtent() : caps.currentExtent;
    minimised_ = !ext.width || !ext.height;
    if (minimised_) return;
    bool vsync = app::Get().vsync;
    uint32_t gen = std::clamp<uint32_t>(app::Get().frame_generation, 1, kMaxGen);
    if (swap_ && !swap_stale_ && ext.width == bb_w_ && ext.height == bb_h_ && vsync == swap_vsync_ && gen == swap_gen_)
        return;
    Flush();
    vkDeviceWaitIdle(dev_);
    if (!CreateSwapChain()) LOG_FIRST(4, "vulkan: the swap chain could not be made again");
}

void VulkanBackend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        CapSave({.res = t.res.get(), .view = t.view, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("vulkan: frame captured to %s", cap_dir_.string().c_str());
}

void VulkanBackend::StartCapture() {
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d%02d%02d_%u", tm.tm_hour, tm.tm_min, tm.tm_sec, cap_seq_);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    // two presses in one second: the second gets its own folder
    for (int n = 2; std::filesystem::exists(cap_dir_, ec); n++)
        cap_dir_ = std::filesystem::weakly_canonical(
            app::ExeDir() / diag::CaptureDir() / (std::string(stamp) + "-" + std::to_string(n)), ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = OpenFile(cap_dir_ / "frame.txt", "w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
    cap_vsc_ = {};
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

// the entry's field, uploaded when its version moved on; null for the dummy when it has none yet
VkImageView VulkanBackend::FieldView(uint32_t shading) {
    ShadingField f = g_program_shading[shading - 1].field();
    if (!f.data || !f.w || !f.h) return VK_NULL_HANDLE;
    if (fields_.size() < g_program_shading.size()) fields_.resize(g_program_shading.size());
    FieldImage& fi = fields_[shading - 1];
    if (!fi.im || fi.im->w != f.w || fi.im->h != f.h) {
        if (fi.im) Retire(fi.im);
        fi.im = NewImage({.fmt = VK_FORMAT_R32_SFLOAT, .w = f.w, .h = f.h});
        fi.version = f.version - 1;
        if (!fi.im) return VK_NULL_HANDLE;
    }
    if (fi.version != f.version) {
        fi.version = f.version;
        uint32_t bytes = f.w * f.h * 4;
        Alloc a = Upload(bytes, 16);
        std::memcpy(a.cpu, f.data, bytes);
        Transition(*fi.im, kDst);
        VkBufferImageCopy r{};
        r.bufferOffset = a.off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {f.w, f.h, 1};
        vkCmdCopyBufferToImage(cmd_, a.buf, fi.im->image, kDst, 1, &r);
        Transition(*fi.im, kRead);
    }
    return fi.im->view;
}

VkImageView VulkanBackend::ScreenView() {
    Target* t = draw_rt_;
    if (!t) return VK_NULL_HANDLE;
    if (!t->fb || t->fb_draw != draw_serial_) {
        t->fb_draw = draw_serial_;
        if (!Feedback(*t)) return VK_NULL_HANDLE;
    }
    return t->fb->view;
}

void VulkanBackend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(*lut_, kDst);
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {256, 1, 1};
    vkCmdCopyBufferToImage(cmd_, a.buf, lut_->image, kDst, 1, &r);
    Transition(*lut_, kRead);
}

// false when there is no image to draw this frame into (minimised, or the swap chain is out of date)
bool VulkanBackend::AcquireImage(uint32_t& bi, VkSemaphore acquired) {
    if (!swap_ || minimised_) return false;
    VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, acquired, VK_NULL_HANDLE, &bi);
    if (r == VK_SUBOPTIMAL_KHR) {
        swap_stale_ = true;
    } else if (r != VK_SUCCESS) {
        swap_stale_ = true;
        return false;
    }
    bb_[bi]->layout = VK_IMAGE_LAYOUT_UNDEFINED;  // its last picture is not read
    return true;
}

void VulkanBackend::BlitFrame(VkImageView src) {
    // 16:9 when the console reports a widescreen display, else 4:3, or the shape a patched title draws
    float aspect = app::FrameAspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    SetScissor(0, 0, int32_t(bb_w_), int32_t(bb_h_));
    Blit(src, uv, {.gamma = gamma_, .rtv = swap_fmt_});
}

// the overlay, once a frame: each of the frame's pictures draws it (DrawUi)
void VulkanBackend::BuildUi() {
    std::lock_guard lock(app::UiLock());
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    app::UiNewFrame();
#endif
    // the window's size is known now; a new font waits for the queue to go idle, on a resize alone
    if (app::UiFontResize()) ImGui_ImplVulkan_CreateFontsTexture();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
}

void VulkanBackend::DrawUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_);
    ForgetDrawCmd();
}

// a picture of the frame, readable (null: none), into the swap chain image bi, the overlay over it
void VulkanBackend::DrawFrame(VkImageView picture, uint32_t bi) {
    Image& bb = *bb_[bi];
    SetTargets(&bb, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    EnsureRendering();
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0};
    ca.clearValue.color = {{0, 0, 0, 1}};
    VkClearRect cr{{{0, 0}, {bb.w, bb.h}}, 0, 1};
    vkCmdClearAttachments(cmd_, 1, &ca, 1, &cr);
    if (picture) BlitFrame(picture);
    DrawUi();
    Transition(bb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

// the front buffer's picture done: the effects under the 2D, when no 2D came to draw them, samples resolved, readable;
// the next picture's effects start afresh
void VulkanBackend::FinishFront(Target& t) {
    if (!post_done_ && post_rt_ == t.data && PostWanted()) {
        post_trace_.applied = "at the front buffer";
        ApplyPost(t);
    }
    PostTraceEnd(t);
    post_rt_ = 0;
    post_done_ = false;
    ResolveSamples(t);
    Transition(*t.res, kRead);
}

// XBE_POST_TRACE: the picture's effects, where they were drawn or why not, logged when that changes from the picture
// before (the counts and addresses left out of the comparison)
void VulkanBackend::PostTraceEnd(const Target& front) {
    static const bool trace = std::getenv("XBE_POST_TRACE") != nullptr;
    PostTrace pt = std::exchange(post_trace_, {});
    if (!trace || post_trace_lines_ >= 400) return;
    char why[96];
    if (!PostWanted())
        snprintf(why, sizeof(why), "effects off");
    else if (pt.applied)
        snprintf(why, sizeof(why), "applied %s, %s", pt.applied, pt.outcome ? pt.outcome : "?");
    else if (!post_rt_)
        snprintf(why, sizeof(why), "not applied: no 3D on a %ux%u target", guest_w_, guest_h_);
    else
        snprintf(why, sizeof(why), "not applied: 3D there, no 2D after it there");
    // the addresses left out too: the game flips between two front buffers
    char key[192];
    snprintf(key, sizeof(key), "%s 3D on the front %d", why, post_rt_ == front.data);
    if (key == post_trace_key_) return;
    post_trace_key_ = key;
    post_trace_lines_++;
    LOGR("post: picture %llu, %s; front %08X %ux%u; 3D draws %u on %08X, %u on other sizes (last %08X %ux%u); 2D "
         "draws %u on other targets, %u reading a target, %u on none",
         (unsigned long long)presents_, why, front.data, front.gw, front.gh, pt.solid, post_rt_, pt.other,
         pt.other_rt, pt.ow, pt.oh, pt.flat_elsewhere, pt.flat_reads, pt.no_target);
}

// frame generation: the front buffer's picture copied into gen_pics_[slot], the frame's pictures one per slot
bool VulkanBackend::KeepPicture(const Surface& front, uint32_t slot) {
    Target* t = EnsureTarget(front, false);
    if (!t || slot >= gen_pics_.size()) return false;
    FinishFront(*t);
    uint32_t w = t->res->w, h = t->res->h;
    ImageRef& keep = gen_pics_[slot];
    if (!keep || keep->w != w || keep->h != h) {
        if (keep) Retire(keep);
        keep = NewImage({.w = w, .h = h, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (!keep) return false;
    }
    SetBlitTarget(*keep, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(t->res->view, uv, {.alpha = true});
    Transition(*keep, kRead);
    gen_taken_ |= 1u << slot;
    return true;
}

// the frame's own picture, before it is drawn again: the last of its pictures
void VulkanBackend::Keep(const Surface& front) {
    FlushRun();
    gen_taken_ = 0;
    gen_pics_.resize(Pictures());
    if (Target* t = EnsureTarget(front, false); t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    KeepPicture(front, uint32_t(gen_pics_.size() - 1));
}

void VulkanBackend::Between(const Surface& front, uint32_t i) {
    FlushRun();
    KeepPicture(front, i - 1);
}

void VulkanBackend::RecordPictures(const std::vector<Image*>& sources, const std::vector<std::string>& notes,
                                   uint32_t w, uint32_t h) {
    recorded_.resize(interp::Recording());
    for (size_t i = 0; i < sources.size(); i++) {
        Recorded& r = recorded_[recorded_next_++ % recorded_.size()];
        if (r.im && (r.w != w || r.h != h)) {
            Retire(r.im);
            r.im.reset();
        }
        r.im = Shrink(*sources[i], sources[i]->view, w, h, {.into = r.im});
        r.w = w;
        r.h = h;
        char b[96];
        snprintf(b, sizeof(b), "frame %llu, picture %zu of %zu: ", (unsigned long long)presents_, i + 1, sources.size());
        r.note = b + notes[i];
    }
}

// F4: the kept pictures, oldest first, as bitmaps beside notes.txt in a folder of their own
void VulkanBackend::SaveRecorded() {
    if (recorded_.empty()) {
        LOGR("interp: no pictures kept to save (XBE_INTERP_RECORD)");
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "interp_%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::error_code ec;
    std::filesystem::path dir = app::ExeDir() / diag::CaptureDir() / stamp;
    for (int n = 2; std::filesystem::exists(dir, ec); n++)
        dir = app::ExeDir() / diag::CaptureDir() / (std::string(stamp) + "-" + std::to_string(n));
    std::filesystem::create_directories(dir, ec);
    FILE* notes = OpenFile(dir / "notes.txt", "w");
    uint32_t saved = 0;
    for (size_t k = 0; k < recorded_.size(); k++) {
        Recorded& r = recorded_[(recorded_next_ + k) % recorded_.size()];
        if (!r.im) continue;
        std::vector<uint8_t> px;
        if (!ReadBack(*r.im, r.w, r.h, px)) continue;
        char name[16];
        snprintf(name, sizeof(name), "%04u", saved++);
        WriteBmp(dir / (std::string(name) + ".bmp"), px, r.w, r.h);
        if (notes) fprintf(notes, "%s: %s\n", name, r.note.c_str());
    }
    if (notes) fclose(notes);
    LOGR("interp: %u pictures saved to %s", saved, dir.string().c_str());
}

// the held pictures due by now, in order
void VulkanBackend::Show() {
    auto now = std::chrono::steady_clock::now();
    while (!held_.empty() && held_.front().at <= now) {
        QueuePresent(held_.front().bi);
        held_.pop_front();
    }
}

// every held picture, now: a new frame is here, or the swap chain is about to go
void VulkanBackend::ShowHeld() {
    for (const Held& h : held_) QueuePresent(h.bi);
    held_.clear();
}

void VulkanBackend::QueuePresent(uint32_t bi) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &present_sem_[bi];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &bi;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Retire(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && Completed() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitTimeline(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    vkResetCommandPool(dev_, f.pool, 0);
    f.garbage.clear();
    f.used = 0;
    BeginCommands(f.cmd);
    Begin();
}

void VulkanBackend::CountFps(uint32_t pictures) {
    fps_n_ += pictures;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

// The frame's draws go to the GPU first; the swap chain image is acquired after, and only its picture and the overlay
// wait for it
void VulkanBackend::Present(const Surface& front) {
    FlushRun();
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
        // XBE_CAPTURE_FRAMES=N: one F3 captures N frames in a row, to compare frames that alternate
        if (cap_more_) {
            cap_more_--;
            cap_seq_++;
            StartCapture();
        }
    } else if (g_capture.exchange(false) && CaptureOn()) {
        const char* n = std::getenv("XBE_CAPTURE_FRAMES");
        cap_more_ = n ? uint32_t(std::max(1l, std::strtol(n, nullptr, 10)) - 1) : 0;
        cap_seq_ = 0;
        StartCapture();
    }
    ShowHeld();
    if (g_record_dump.exchange(false)) SaveRecorded();
    // frame generation: the pictures Keep and Between took, in turn, its own last; else the front buffer alone. With
    // their notes for XBE_INTERP_RECORD
    std::vector<VkImageView> pictures;
    std::vector<Image*> sources;
    std::vector<std::string> notes;
    if (!gen_pics_.empty() && (gen_taken_ >> (gen_pics_.size() - 1) & 1)) {
        for (size_t i = 0; i < gen_pics_.size(); i++) {
            if (!(gen_taken_ >> i & 1)) continue;
            pictures.push_back(gen_pics_[i]->view);
            sources.push_back(gen_pics_[i].get());
            notes.push_back(i + 1 < gen_pics_.size() ? interp::Note(uint32_t(i)) : "the frame's own");
        }
    }
    gen_taken_ = 0;
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (pictures.empty() && t) {
        if (LatchCpuWrite(*t)) UploadTarget(*t, front);
        FinishFront(*t);
        pictures.push_back(t->res->view);
        sources.push_back(t->res.get());
        notes.push_back("the frame's own, alone");
    }
    post_rt_ = 0;
    post_done_ = false;
    if (t && interp::Recording()) RecordPictures(sources, notes, t->gw, t->gh);
    if (pictures.empty()) pictures.push_back(VK_NULL_HANDLE);
    // the frame's pictures share out the time to the next frame, the first shown now; the time between frames is
    // measured
    auto now = std::chrono::steady_clock::now();
    double since = gen_last_ == decltype(gen_last_){} ? 1.0 : std::chrono::duration<double>(now - gen_last_).count();
    gen_last_ = now;
    if (since < 0.25) gen_period_ += (since - gen_period_) * 0.1;
    if (ramp_dirty_) UploadGammaRamp();
    Submit(false);

    BuildUi();
    Frame& f = CurrentFrame();
    uint32_t shown = 0;
    for (size_t i = 0; i < pictures.size(); i++, shown++) {
        uint32_t bi = 0;
        if (!AcquireImage(bi, f.acquired[i])) break;
        BeginCommands(f.present[i]);
        DrawFrame(pictures[i], bi);
        Submit(false, f.acquired[i], present_sem_[bi]);
        held_.push_back({bi, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(gen_period_ * double(i) / double(pictures.size())))});
    }
    DeliverReports();
    {
        auto perf_swap = PerfClock::now();
        Show();
        g_perf.swap += PerfMs(perf_swap);
    }

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    if (std::getenv("XBE_FRAME_LOG"))
        LOGI("frame %llu: %u draws, %u opaque, %u counted, %zu reports left", (unsigned long long)presents_,
             frame_draws_, frame_opaque_, frame_counted_, reports_.size());
    frame_draws_ = frame_opaque_ = frame_counted_ = 0;
    kept_runs_.clear();
    pass_serial_ = 0;
    EndShadingFrame();
    ReleaseOldTextures();
    ResetFrame();
    CountFps(std::max(shown, 1u));
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

// every target made again at the scale and samples, keeping its picture: a texture the title drew once (an
// environment map) is read from its target for the rest of the level, and guest memory never had it
void VulkanBackend::RemakeTargets(float scale, uint32_t samples) {
    std::unordered_map<uint64_t, Target> old;
    old.swap(targets_);
    bound_rt_ = bound_ds_ = nullptr;
    for (auto& [key, o] : old) ResolveSamples(o);
    scale_ = scale;
    msaa_ = samples;
    VkPipeline depth_pso = EnsureDepthCopyPipeline();
    for (auto& [key, o] : old) {
        for (ImageRef* r : {&o.res, &o.ms, &o.fb, &o.zb}) {
            if (*r) Retire(*r);
        }
        Target t;
        t.depth = o.depth;
        t.gw = o.gw;
        t.gh = o.gh;
        t.fmt = o.fmt;
        if (!CreateTargetResource(t)) continue;
        CreateSampledTarget(t);
        if (t.depth) {
            if (depth_pso) DrawDepthCopy(o, t, depth_pso);
        } else {
            WriteRes(t, true);
            Transition(*o.res, kRead);
            SetBlitTarget(*t.res, t.w, t.h);
            float uv[4] = {0, 0, 1, 1};
            uint32_t taps = uint32_t(std::ceil(std::max(float(o.w) / t.w, 1.0f) - kTapsSlack));
            Blit(o.res->view, uv, {.alpha = true, .taps = taps, .rtv = t.fmt});
        }
        t.data = o.data;
        t.bytes = o.bytes;
        t.epoch = o.epoch;
        t.sig = o.sig;
        t.drawn = o.drawn;
        t.read = std::move(o.read);
        targets_[key] = std::move(t);
    }
    bound_rt_ = bound_ds_ = nullptr;
    LOGI("vulkan: %zu targets made again at render scale %.3f, %ux msaa", targets_.size(), scale_, msaa_);
}

void VulkanBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    uint32_t samples = WantSamples();
    if (std::fabs(want - scale_) > 1e-4f || samples != msaa_) {
        Flush();
        RemakeTargets(want, samples);
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
VulkanBackend::CopySource VulkanBackend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    ResolveSamples(t);
    CopySource out{t.res.get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(*t.res, t.res->Sampled(), t.gw, t.gh, o);
        out.res = out.down.get();
    } else {
        Transition(*t.res, kSrc);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool VulkanBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void VulkanBackend::Sync(uint32_t data, uint32_t) {
    FlushRun();
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(*src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void VulkanBackend::SyncLate(uint32_t data) {
    FlushRun();
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        const Buffer& rb = *l.rb[best];
        vmaInvalidateAllocation(vma_, rb.alloc, 0, VK_WHOLE_SIZE);
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, rb.map + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void VulkanBackend::ResetLate(Late& l, const Target& t) {
    for (auto& r : l.rb) {
        if (r) Retire(r);
    }
    if (l.down) Retire(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
Image* VulkanBackend::PrepareLateSource(Target& t, Late& l) {
    CopySource src = PrepareCopySource(t, {.into = l.down});
    if (src.down) l.down = src.down;
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void VulkanBackend::RecordLateCopy(Late& l, uint32_t slot, Image& src) {
    if (!l.rb[slot]) {
        l.rb[slot] = NewBuffer(uint64_t(l.gw) * l.gh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!l.rb[slot]) return;
    }
    Transition(src, kSrc);
    Barrier();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {l.gw, l.gh, 1};
    vkCmdCopyImageToBuffer(cmd_, src.image, kSrc, l.rb[slot]->buf, 1, &r);
    HostBarrier();
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void VulkanBackend::RecordLate(uint32_t data) {
    FlushRun();
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    Image* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, *src);
}

// filters src (through view) into a w x h BGRA image, left as a copy source; o.into is drawn over again when given
ImageRef VulkanBackend::Shrink(Image& src, VkImageView view, uint32_t w, uint32_t h, const ShrinkOptions& o) {
    g_perf.shrinks++;
    ImageRef down = o.into;
    if (!down) {
        down = NewImage({.w = w, .h = h,
                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
        if (!down) return nullptr;
    }
    Transition(src, kRead);
    SetBlitTarget(*down, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(view, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(*down, kSrc);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool VulkanBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint64_t bytes = uint64_t(w) * h * 4;
    BufferRef rb = NewBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!rb) return false;
    Transition(im, kSrc);
    Barrier();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd_, im.image, kSrc, rb->buf, 1, &r);
    HostBarrier();
    Flush();
    vmaInvalidateAllocation(vma_, rb->alloc, 0, VK_WHOLE_SIZE);
    out.assign(rb->map, rb->map + bytes);
    return true;
}

void VulkanBackend::CapSave(const CapImage& img, const char* name) {
    ImageRef down = Shrink(*img.res, img.view, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(*down, img.w, img.h, px)) return;
    WriteBmp(cap_dir_ / (std::string(name) + ".bmp"), px, img.w, img.h);
}

// XBE_CAPTURE_FULL=1: at the target's own size, upscaled as drawn, rather than the console's
void VulkanBackend::CapSave(Target& t, const char* name) {
    static const bool full = std::getenv("XBE_CAPTURE_FULL") && std::atoi(std::getenv("XBE_CAPTURE_FULL"));
    ResolveSamples(t);
    CapSave({.res = t.res.get(), .view = t.res->Sampled(), .w = full ? t.w : t.gw, .h = full ? t.h : t.gh}, name);
}

// XBE_CAPTURE_VS=<hex shader handle>, XBE_CAPTURE_RT=<hex surface address>: the colour and depth targets before each
// draw with that vertex shader or into that surface, and before the draw after the last of them
void VulkanBackend::CapAround(const State& s) {
    static const char* env = std::getenv("XBE_CAPTURE_VS");
    static const char* env_rt = std::getenv("XBE_CAPTURE_RT");
    if (!env && !env_rt) return;
    static const uint32_t vs = env ? uint32_t(std::strtoul(env, nullptr, 16)) : 0;
    static const uint32_t rt = env_rt ? mem::VirtualFromPhysical(uint32_t(std::strtoul(env_rt, nullptr, 16))) : 0;
    bool match = (env && s.vs_handle == vs) || (env_rt && s.rt.header && mem::VirtualFromPhysical(s.rt.data) == rt);
    if (!match && !cap_after_) return;
    cap_after_ = match;
    char name[48];
    if (Target* t = FindTarget(s.rt, false)) {
        snprintf(name, sizeof(name), "%04u_pre_%08X", cap_i_, t->data);
        CapSave(*t, name);
    }
    if (Target* z = FindTarget(s.zs, true)) {
        snprintf(name, sizeof(name), "%04u_pre_%08X_z", cap_i_, z->data);
        CapSave(*z, name);
    }
}

// the frame moves to another target: save the one it leaves
void VulkanBackend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    FlushRun();
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
