// OpenGL backend: the D3D11 backend's structure (d3d11.cpp) on desktop OpenGL 4.0+ core, for GPUs whose Vulkan is
// missing or incomplete (Ivy Bridge's HD 4000 on Mesa's crocus: GL 4.2). The same HLSL as the other renderers, made
// SPIR-V by glslang and GLSL by SPIRV-Cross. glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE) makes every target hold
// its rows as D3D does (row 0 the top, viewport and scissor rows D3D's, gl_FragCoord SV_Position's, facing kept), so
// the shared shaders and rectangles carry over unchanged; the window alone is flipped, as the picture is put on it.
// Without it (Intel's Windows driver for the HD 4000) the last vertex stage does the same (opengl_glsl.h's flip) and
// the front face is swapped.
#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL3/SDL_opengl.h>


#include <algorithm>
#include <bit>
#include <cmath>
#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw.h"
#include "gpu/draw_steps.h"
#include "gpu/dxgi_formats.h"
#include "gpu/fsr_hlsl.h"
#include "gpu/hud_split.h"
#include "gpu/interp.h"
#include "gpu/opengl_glsl.h"
#include "gpu/post_effects.h"
#include "gpu/post_hlsl.h"
#include "gpu/targets.h"
#include "gpu/smaa_tex.h"
#include "kernel/memory.h"

namespace gpu {
namespace {

// the GL functions drawn with, loaded from the context (SDL_GL_GetProcAddress); none is linked
#define XBE_GL_FUNCTIONS(X)                                                                                            \
    X(GetString) X(GetStringi) X(GetIntegerv) X(GetError) X(Enable) X(Disable) X(Viewport) X(ViewportIndexedf)         \
    X(Scissor) X(ClearBufferfv) X(ClearBufferfi) X(ClearBufferiv) X(ColorMask) X(DepthMask) X(StencilMask)            \
    X(DepthFunc) X(StencilFunc) X(StencilOp) X(BlendFuncSeparate) X(BlendEquationSeparate) X(BlendColor) X(CullFace)   \
    X(FrontFace) X(PolygonMode) X(PolygonOffset) X(PixelStorei) X(GenTextures) X(DeleteTextures) X(BindTexture)        \
    X(ActiveTexture) X(TexImage2D) X(TexImage3D) X(TexSubImage2D) X(CompressedTexImage2D) X(TexImage2DMultisample)    \
    X(TexParameteri) X(GenerateMipmap) X(GenSamplers) X(DeleteSamplers) X(BindSampler) X(SamplerParameteri)           \
    X(SamplerParameterf) X(SamplerParameterfv) X(GenFramebuffers) X(DeleteFramebuffers) X(BindFramebuffer)            \
    X(FramebufferTexture) X(CheckFramebufferStatus) X(DrawBuffer) X(ReadBuffer) X(BlitFramebuffer) X(ReadPixels)      \
    X(GenBuffers) X(DeleteBuffers) X(BindBuffer) X(BufferData) X(MapBufferRange) X(UnmapBuffer) X(BindBufferRange)    \
    X(GenVertexArrays) X(BindVertexArray) X(DeleteVertexArrays) X(EnableVertexAttribArray)                           \
    X(DisableVertexAttribArray) X(VertexAttribPointer) X(VertexAttribIPointer) X(CreateShader) X(ShaderSource)        \
    X(CompileShader) X(GetShaderiv) X(GetShaderInfoLog) X(DeleteShader) X(CreateProgram) X(AttachShader)              \
    X(LinkProgram) X(GetProgramiv) X(GetProgramInfoLog) X(DeleteProgram) X(UseProgram) X(DrawArrays)                  \
    X(DrawElementsBaseVertex) X(PatchParameteri) X(GenQueries) X(DeleteQueries) X(BeginQuery) X(EndQuery)             \
    X(GetQueryObjectuiv) X(Flush) X(FenceSync) X(ClientWaitSync) X(DeleteSync) X(ClipControl) X(DebugMessageCallback) \
    X(BufferStorage)
namespace gl {
#define XBE_GL_DECLARE(name) decltype(&::gl##name) name = nullptr;
XBE_GL_FUNCTIONS(XBE_GL_DECLARE)
#undef XBE_GL_DECLARE
bool Load() {
#define XBE_GL_LOAD(name) name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name));
    XBE_GL_FUNCTIONS(XBE_GL_LOAD)
#undef XBE_GL_LOAD
    // those a 4.0 context may lack, used only where present: ClipControl (ARB_clip_control),
    // ViewportIndexedf (else integer viewports), DebugMessageCallback (XBE_GL_DEBUG), BufferStorage (else the rings
    // mapped a piece at a time)
    bool ok = true;
#define XBE_GL_CHECK(name)                                                                                             \
    if (!name && std::strcmp(#name, "ClipControl") && std::strcmp(#name, "ViewportIndexedf") &&                       \
        std::strcmp(#name, "DebugMessageCallback") && std::strcmp(#name, "BufferStorage")) {                                                                  \
        LOGE("opengl: gl%s missing", #name);                                                                           \
        ok = false;                                                                                                    \
    }
    XBE_GL_FUNCTIONS(XBE_GL_CHECK)
#undef XBE_GL_CHECK
    return ok;
}
}  // namespace gl

void APIENTRY DebugMessage(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    static std::atomic<int> n{0};
    if (n++ < 1000) LOGW("opengl: %s%s", type == GL_DEBUG_TYPE_ERROR ? "error: " : "", message);
}


// a texture name, deleted with its owner; what referred to it (framebuffers, unit bindings) is dropped with it
void (*g_forget)(GLuint) = nullptr;
class Tex {
    GLuint id_ = 0;

public:
    Tex() = default;
    explicit Tex(GLuint id) : id_(id) {}
    ~Tex() { reset(); }
    Tex(const Tex&) = delete;
    Tex& operator=(const Tex&) = delete;
    Tex(Tex&& o) noexcept : id_(std::exchange(o.id_, 0)) {}
    Tex& operator=(Tex&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = std::exchange(o.id_, 0);
        }
        return *this;
    }
    GLuint get() const { return id_; }
    explicit operator bool() const { return id_ != 0; }
    void reset() {
        if (!id_) return;
        if (g_forget) g_forget(id_);
        gl::DeleteTextures(1, &id_);
        id_ = 0;
    }
};

struct Rect {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
};
struct Viewport {
    float x = 0, y = 0, w = 0, h = 0;
};
bool Empty(const Rect& r) { return r.right <= r.left || r.bottom <= r.top; }

// a texture as a shader stage reads it: the name and the target it is bound to
struct View {
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
};

struct Image {
    enum class Newest { Both, Single, Multisampled };
    Tex tex;
    // a depth snapshot's depth and stencil as a stencil texture reads them (StencilView): a copy, as one texture's
    // depth and stencil can't both be read at once without texture views
    Tex stencil;
    GLenum target = GL_TEXTURE_2D;
    uint32_t w = 0, h = 0, data = 0;
    uint32_t samples = 1;
    std::unique_ptr<Image> ms;
    Newest newest = Newest::Both;
    GuestExtent guest{};  // Xbox dimensions, independent of the allocated texture
    uint64_t epoch = 0, signature = 0, frame = 0, palette_hash = 0;
    uint64_t fur_frame = UINT64_MAX;
    bool depth = false, drawn = false;
    Surface surface{};
    View view() const { return {tex.get(), target}; }
};

Rect RenderRect(const Image& im, GuestRect rect) {
    auto edge = [](int32_t x, uint32_t guest, uint32_t render) {
        const uint32_t bounded = uint32_t(std::clamp(x, 0, int32_t(guest)));
        return int32_t((uint64_t(bounded) * render + guest / 2) / guest);
    };
    return {edge(rect.left, im.guest.w, im.w), edge(rect.top, im.guest.h, im.h), edge(rect.right, im.guest.w, im.w),
            edge(rect.bottom, im.guest.h, im.h)};
}
Rect WholeImage(const Image& im) { return {0, 0, int32_t(im.w), int32_t(im.h)}; }

// a texture unit beyond those a shader reads (Unit), where textures are bound to be made or filled
constexpr GLenum kEditUnit = GL_TEXTURE0 + kUnits;

// a stage of a program: its GLSL compiled, and what it reads
struct Stage {
    uint64_t hash = 0;
    GLuint shader = 0;
    std::vector<Bind> binds;
    std::string source;  // the HLSL, a vertex stage's alone (for a failed link's log)
};
struct Program {
    GLuint id = 0;
    std::vector<Bind> binds;
};

GLenum ShaderType(char profile) {
    switch (profile) {
        case 'v': return GL_VERTEX_SHADER;
        case 'g': return GL_GEOMETRY_SHADER;
        case 'h': return GL_TESS_CONTROL_SHADER;
        case 'd': return GL_TESS_EVALUATION_SHADER;
        default: return GL_FRAGMENT_SHADER;
    }
}

const char* kBlit = R"(
Texture2D picture : register(t0); SamplerState samp : register(s0);
cbuffer Fade : register(b0) { float fade; float gamma; float brightness; float spare; };
struct Out { float4 p:SV_Position; float2 uv:TEXCOORD0; };
Out vs(uint id:SV_VertexID) { Out o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
float4 ps(Out i):SV_Target { float3 c=picture.Sample(samp,i.uv).rgb;
 return float4(saturate(pow(saturate(c), gamma)*fade+brightness),1); }
)";
const char* kCopyColor = R"(
Texture2D<float4> source : register(t0);
SamplerState filter : register(s0);
cbuffer Copy : register(b0) { float4 window; };
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return source.SampleLevel(filter, window.xy + window.zw * uv, 0);
}
)";
// a single sampled depth and stencil into a multisampled target: the depth written, the stencil a bit a pass (no
// stencil export on GL 4.2), its stencil texture's value in red
const char* kCopyDepthStencil = R"(
Texture2D<float> depth : register(t0);
Texture2D<uint> stencil : register(t1);
cbuffer Copy : register(b0) { uint bit; };
uint2 SourcePixel(float2 uv) {
    uint width, height;
    depth.GetDimensions(width, height);
    uint2 size = uint2(width, height);
    return min(uint2(saturate(uv) * size), size - 1);
}
float copy_depth(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Depth {
    return depth.Load(int3(SourcePixel(uv), 0));
}
void copy_stencil(float4 position : SV_Position, float2 uv : TEXCOORD0) {
    if (!(stencil.Load(int3(SourcePixel(uv), 0)) & bit)) discard;
}
)";

struct HostFormat {
    GLenum internal = 0, format = 0, type = 0;
    bool compressed = false;
    uint32_t bytes = 4;  // per texel, uncompressed
};
HostFormat GlFormat(Host h) {
    switch (h) {
        case Host::BGRA8: return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false, 4};
        case Host::BC1: return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, true, 0};
        case Host::BC2: return {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, true, 0};
        case Host::BC3: return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, true, 0};
        case Host::R32F: return {GL_R32F, GL_RED, GL_FLOAT, false, 4};
        case Host::R16Unorm: return {GL_R16, GL_RED, GL_UNSIGNED_SHORT, false, 2};
        default: return {};
    }
}
// a vertex element's DXGI format as glVertexAttrib(I)Pointer takes it
struct AttribFormat {
    GLint size = 0;
    GLenum type = 0;
    bool normalized = false, integer = false;
    uint32_t bytes = 0;
};
AttribFormat Attrib(uint32_t format) {
    switch (DXGI_FORMAT(format)) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return {4, GL_FLOAT, false, false, 16};
        case DXGI_FORMAT_R32G32B32_FLOAT: return {3, GL_FLOAT, false, false, 12};
        case DXGI_FORMAT_R32G32_FLOAT: return {2, GL_FLOAT, false, false, 8};
        case DXGI_FORMAT_R32_FLOAT: return {1, GL_FLOAT, false, false, 4};
        case DXGI_FORMAT_R32_UINT: return {1, GL_UNSIGNED_INT, false, true, 4};
        case DXGI_FORMAT_R16G16_FLOAT: return {2, GL_HALF_FLOAT, false, false, 4};
        case DXGI_FORMAT_R16G16B16A16_SNORM: return {4, GL_SHORT, true, false, 8};
        case DXGI_FORMAT_R16G16B16A16_SINT: return {4, GL_SHORT, false, true, 8};
        case DXGI_FORMAT_R16G16_SNORM: return {2, GL_SHORT, true, false, 4};
        case DXGI_FORMAT_R16G16_SINT: return {2, GL_SHORT, false, true, 4};
        case DXGI_FORMAT_R16_SNORM: return {1, GL_SHORT, true, false, 2};
        case DXGI_FORMAT_R16_SINT: return {1, GL_SHORT, false, true, 2};
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true, false, 4};
        case DXGI_FORMAT_B8G8R8A8_UNORM: return {GL_BGRA, GL_UNSIGNED_BYTE, true, false, 4};
        case DXGI_FORMAT_R8G8_UNORM: return {2, GL_UNSIGNED_BYTE, true, false, 2};
        case DXGI_FORMAT_R8_UNORM: return {1, GL_UNSIGNED_BYTE, true, false, 1};
        default: return {};
    }
}
// the title's blend, stencil and compare values are GL's own (the NV2A's methods); anything else as D3D11 takes it
GLenum BlendFactor(uint32_t v) {
    if (v <= 1 || (v >= 0x300 && v <= 0x308) || (v >= 0x8001 && v <= 0x8004)) return v;
    return GL_ONE;
}
GLenum BlendEquation(uint32_t v) {
    switch (v) {
        case 0x800A: case 0x800B: case 0x8007: case 0x8008: return v;
        default: return GL_FUNC_ADD;
    }
}
GLenum StencilOperation(uint32_t v) {
    switch (v) {
        case 0: case 0x1E01: case 0x1E02: case 0x1E03: case 0x150A: case 0x8507: case 0x8508: return v;
        default: return GL_KEEP;
    }
}
GLenum Compare(uint32_t v) { return GL_NEVER + CmpIndex(v); }

// the fixed state of a draw (D3D11's blend, depth and rasterizer state objects)
struct Raster {
    bool blend = false;
    GLenum src = GL_ONE, dst = GL_ZERO, src_alpha = GL_ONE, dst_alpha = GL_ZERO, op = GL_FUNC_ADD,
           op_alpha = GL_FUNC_ADD;
    uint8_t mask = 15;
    bool zen = false, zwrite = false;
    GLenum zfunc = GL_ALWAYS;
    bool sten = false;
    GLenum sfunc = GL_ALWAYS;
    uint32_t smask = 255, swmask = 255;
    GLenum sfail = GL_KEEP, szfail = GL_KEEP, spass = GL_KEEP;
    bool cull = false;
    GLenum front = GL_CW;
    bool wire = false;
    float zslope = 0, zbias = 0;
};
enum class DepthPass { Original, Color, Depth };

struct SampleQuery {
    GLuint query = 0;
    double guest_per_sample = 1;
    ~SampleQuery() {
        if (query) gl::DeleteQueries(1, &query);
    }
};
using Query = std::shared_ptr<SampleQuery>;
struct Report {
    uint32_t address;
    std::vector<Query> queries;
};
struct CacheRecord {
    uint64_t key, checksum;
    uint32_t bytes, reserved;
};
static_assert(sizeof(CacheRecord) == 24);
constexpr uint64_t kShaderCacheMagic = 0x3145484341434C47ull;  // GLCACHE1: a stage's binds then its GLSL
// a stage's or program's key apart when it does glClipControl's work
constexpr uint64_t kFlipKey = 0x9E3779B97F4A7C15ull;
// bump when HlslToGlsl makes other GLSL of the same source
constexpr uint64_t kGlslRevision = 2;
constexpr size_t kPassConstantFloats = 32;  // a full-screen pass's constant buffer: 8 float4, kMotionBlurHlsl's

// a buffer written a piece at a time behind the GPU (D3D11's NO_OVERWRITE maps), started over, orphaned, when full
struct Ring {
    GLenum target = GL_ARRAY_BUFFER;
    GLuint buffer = 0;
    uint32_t capacity = 0, next = 0, least = 0;
    // stores the buffer has had: a new one (grown, or orphaned when full) leaves what was written before behind, so a
    // range bound from the old one reads nothing written
    uint64_t generation = 0;
    uint32_t frame_bytes = 0;  // put since the last Present
    // persistently mapped (ARB_buffer_storage): written in place with a copy, no map and unmap a piece (on NVIDIA
    // microseconds each, several a draw: the render thread's whole frame); a store left full waits in spare behind a
    // fence until the GPU is past it, then is written again
    uint8_t* mapped = nullptr;
    struct Spare {
        GLuint buffer;
        uint8_t* mapped;
        GLsync done;
    };
    std::deque<Spare> spare;
};
constexpr size_t kRingStores = 6;  // stores a ring keeps behind the GPU before it waits for the oldest

class OpenGLBackend final : public Backend, Targets<OpenGLBackend, Image>, DrawSteps<OpenGLBackend, Image>, PostEffects<OpenGLBackend, Image, View> {
    friend class Targets<OpenGLBackend, Image>;
    friend class DrawSteps<OpenGLBackend, Image>;
    friend class PostEffects<OpenGLBackend, Image, View>;
    static constexpr const char* kName = "opengl";
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::thread::id owner_{};
    uint32_t glsl_version_ = 420;
    bool clip_control_ = true;
    bool stencil_texturing_ = false, s3tc_ = false, anisotropic_ = false;
    int ubo_align_ = 256;
    int vsync_ = -1;
    GLuint vao_ = 0;
    bool persistent_ = false;  // the rings persistently mapped (Ring::mapped)
    Ring vertices_{GL_ARRAY_BUFFER, 0, 0, 0, 8u << 20}, indices_{GL_ELEMENT_ARRAY_BUFFER, 0, 0, 0, 2u << 20},
        uniforms_{GL_UNIFORM_BUFFER, 0, 0, 0, 4u << 20};
    double driver_timing_[9]{};
    uint32_t driver_frames_ = 0;
    PerfClock::time_point driver_start_ = PerfClock::now();
    std::unordered_map<uint64_t, Image> textures_;
    // stages by source hash, programs by their stages' hashes
    std::unordered_map<uint64_t, Stage> stages_;
    std::unordered_map<uint64_t, Stage*> vertex_stages_, pixel_stages_;
    std::unordered_map<uint32_t, std::pair<Stage*, Stage*>> tessellation_;
    std::unordered_map<uint64_t, Program> programs_;
    std::unordered_map<ShadingField (*)(), Image> fields_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> shader_cache_;
    std::filesystem::path shader_cache_path_;
    bool shader_cache_valid_ = false;
    std::array<std::array<float, 4>, 192> last_vsc_{};
    DrawConstants last_dc_{};
    bool last_vsc_valid_ = false, last_dc_valid_ = false;
    uint32_t vc_offset_ = 0, dc_offset_ = 0;
    std::unordered_map<uint64_t, GLuint> framebuffers_;
    std::unordered_map<uint64_t, Raster> rasters_;
    std::unordered_map<uint64_t, GLuint> samplers_;
    // the GL state as last set (Set*): what is set again only when it differs
    struct Bound {
        bool valid = false;
        GLuint program = 0, draw_fbo = 0, read_fbo = 0;
        bool blend = false, depth = false, stencil = false, cull = false, scissor = false, offset = false;
        GLenum src = 0, dst = 0, src_alpha = 0, dst_alpha = 0, op = 0, op_alpha = 0;
        float blend_color[4]{};
        uint8_t mask = 0;
        bool zwrite = false;
        GLenum zfunc = 0, sfunc = 0, sfail = 0, szfail = 0, spass = 0, front = 0, polygon = 0;
        uint32_t sref = 0, smask = 0, swmask = 0;
        float zslope = 0, zbias = 0;
        Viewport viewport{};
        Rect scissor_rect{};
        GLuint unit_tex[kUnits]{};
        GLenum unit_target[kUnits]{};
        GLuint unit_sampler[kUnits]{};
        uint16_t attribs = 0;  // vertex attribute arrays enabled
        struct Attribute {
            uint32_t format = 0, stride = 0, offset = 0;
            GLuint buffer = 0;  // a new store of the vertex ring is another buffer (persistent stores)
        } attrib[16];
        GLint patch = 0;
    } bound_;
    Stage *blit_vs_ = nullptr, *blit_ps_ = nullptr, *copy_ps_ = nullptr, *copy_depth_ps_ = nullptr,
          *copy_stencil_ps_ = nullptr;
    std::unordered_map<std::string, Stage*> pass_stages_;  // full-screen passes by entry and source
    Image screen_;  // the window's picture as drawn, upside down for the window (Display)
    HudSplit hud_;  // g_hud_split: the HUD's elements, and the draw's remap (its viewport and scissor)
    Image smaa_area_, smaa_search_;
    std::array<Image, 4> generated_;
    uint32_t taken_ = 0, generated_count_ = 0, next_picture_ = 0;
    int last_picture_ = -1;
    bool replaying_ = false, keeping_pictures_ = false;
    std::chrono::steady_clock::time_point schedule_{};
    std::chrono::steady_clock::duration picture_interval_{};
    GLuint blit_sampler_ = 0, copy_sampler_ = 0;
    std::vector<Query> counted_;
    struct Recorded {
        Image im;
        std::string note;
    };
    std::vector<Recorded> recorded_;  // XBE_INTERP_RECORD's ring, the next written at recorded_next_
    uint64_t recorded_next_ = 0;
    std::vector<Report> reports_;
    uint32_t poll_draws_ = 0;  // Draw looks at reports_ every 32nd draw
    // drawing again: the frame's draws' geometry where they put it in the rings, by their serial (Draw calls in the
    // pass, Pass and Uncounted as Vulkan's pass_serial_), for a draw drawn again as the frame drew it (AsDrawn) to
    // draw from instead of putting the same bytes in again, while the stores they went to are the rings' still
    struct KeptGeometry {
        uint32_t serial = 0;
        uint64_t vertex_store = 0, index_store = 0;  // the rings' generations
        uint32_t buffer_count = 0, index_bytes = 0, index_offset = 0;
        GLenum index_type = 0;
        uint32_t base[16]{}, bytes[16]{};
        // as assembled, before Draw repacked misaligned streams, and the strides and element offsets after
        uint32_t source_bytes[16]{}, stride[16]{};
        uint16_t offset[16]{};
    };
    std::vector<KeptGeometry> kept_geometry_;
    size_t kept_next_ = 0;
    uint32_t serial_ = 0;
    bool as_drawn_ = false, uncounted_ = false;  // AsDrawn and Uncounted, for the next Draw
    bool keep_geometry_ = false;
    uint64_t shown_ = 0;
    uint32_t ww_ = 0, wh_ = 0;
    int max_texture_ = 8192;
    bool ui_ = false;
    std::chrono::steady_clock::time_point last_{};
    std::chrono::steady_clock::time_point fps_since_{};
    uint32_t fps_pictures_ = 0;
    float fps_ = 0;
    State sprite_;
    Assembled assembled_;
    std::array<std::vector<uint8_t>, 16> color_buffers_;

    static OpenGLBackend*& Self() {
        static OpenGLBackend* self = nullptr;
        return self;
    }

    // ---- the context: current on the thread drawing, handed over when another starts to
    void Current() {
        const auto me = std::this_thread::get_id();
        if (owner_ == me || !context_) return;
        if (!SDL_GL_MakeCurrent(window_, context_)) LOGE("opengl: context not current: %s", SDL_GetError());
        owner_ = me;
    }
    // Current, and a render size SetGuestSize changed worked out before anything is drawn at it
    bool rescale_ = false;
    void Begin() {
        Current();
        if (std::exchange(rescale_, false)) ApplyRenderScale();
    }
    void Release() {
        if (!context_ || owner_ != std::this_thread::get_id()) return;
        SDL_GL_MakeCurrent(window_, nullptr);
        owner_ = {};
    }

    // ---- the shader cache: each stage's binds and GLSL by its source's hash
    void LoadShaderCache() {
        shader_cache_path_ = app::CacheDir() / "opengl-shaders-v1.bin";
        FILE* f = OpenFile(shader_cache_path_, "rb");
        if (!f) return;
        std::fseek(f, 0, SEEK_END);
        const long length = std::ftell(f);
        std::rewind(f);
        uint64_t magic = 0;
        if (length < 8 || length > 128 * 1024 * 1024 || std::fread(&magic, 8, 1, f) != 1 ||
            magic != kShaderCacheMagic) {
            std::fclose(f);
            return;
        }
        shader_cache_valid_ = true;
        for (;;) {
            CacheRecord r{};
            const size_t got = std::fread(&r, 1, sizeof(r), f);
            if (!got && std::feof(f)) break;
            if (got != sizeof(r) || !r.bytes || r.bytes > 8 * 1024 * 1024 || r.reserved) {
                shader_cache_valid_ = false;
                break;
            }
            std::vector<uint8_t> bytes(r.bytes);
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size() ||
                Hash(bytes.data(), bytes.size()) != r.checksum) {
                shader_cache_valid_ = false;
                break;
            }
            shader_cache_[r.key] = std::move(bytes);
        }
        std::fclose(f);
        LOGR("opengl: loaded %zu cached shaders", shader_cache_.size());
    }
    void SaveShader(uint64_t key, std::vector<uint8_t> bytes) {
        FILE* f = OpenFile(shader_cache_path_, shader_cache_valid_ ? "ab" : "wb");
        shader_cache_[key] = std::move(bytes);
        if (!f) return;
        bool ok = true;
        auto put = [&](uint64_t k, const std::vector<uint8_t>& b) {
            const CacheRecord r{k, Hash(b.data(), b.size()), uint32_t(b.size()), 0};
            ok = ok && std::fwrite(&r, sizeof(r), 1, f) == 1 && std::fwrite(b.data(), 1, b.size(), f) == b.size();
        };
        if (!shader_cache_valid_) {
            ok = std::fwrite(&kShaderCacheMagic, 8, 1, f) == 1;
            for (const auto& [k, b] : shader_cache_) put(k, b);
        } else {
            put(key, shader_cache_[key]);
        }
        ok = std::fclose(f) == 0 && ok;
        shader_cache_valid_ = ok;
    }

    // ---- stages and programs
    // flip: the stage does glClipControl's work (opengl_glsl.h)
    Stage Compile(const std::string& source, const char* entry, const char* profile, bool flip) {
        PerfScope timing{g_perf.compile};
        Stage out;
        out.hash = SourceHash(source, entry, profile) ^ (flip ? kFlipKey : 0);
        if (profile[0] == 'v') out.source = source;
        const uint64_t key = out.hash ^ (uint64_t(glsl_version_) << 48) ^ (kGlslRevision << 40);
        std::string glsl;
        if (auto it = shader_cache_.find(key); it != shader_cache_.end() && !it->second.empty()) {
            const auto& b = it->second;
            const size_t n = b[0];
            if (b.size() >= 1 + n * sizeof(Bind)) {
                out.binds.resize(n);
                if (n) std::memcpy(out.binds.data(), b.data() + 1, n * sizeof(Bind));
                glsl.assign(reinterpret_cast<const char*>(b.data() + 1 + n * sizeof(Bind)),
                            b.size() - 1 - n * sizeof(Bind));
            }
        }
        if (glsl.empty()) {
            g_perf.shaders++;
            const auto start = std::chrono::steady_clock::now();
            std::string err;
            if (!HlslToGlsl(source, entry, profile, glsl_version_, glsl, out.binds, err, flip)) {
                LOGE("opengl: shader %s/%s: %s", entry, profile, err.c_str());
                return out;
            }
            const double ms = SinceMs(start);
            if (ms >= 100) LOGR("opengl: translating %s took %.0f ms (saved for next launch)", profile, ms);
            std::vector<uint8_t> bytes(1 + out.binds.size() * sizeof(Bind));
            bytes[0] = uint8_t(out.binds.size());
            if (!out.binds.empty()) std::memcpy(bytes.data() + 1, out.binds.data(), out.binds.size() * sizeof(Bind));
            AppendBytes(bytes, glsl.data(), glsl.size());
            SaveShader(key, std::move(bytes));
        }
        const GLuint shader = gl::CreateShader(ShaderType(profile[0]));
        const char* text = glsl.c_str();
        gl::ShaderSource(shader, 1, &text, nullptr);
        gl::CompileShader(shader);
        GLint ok = 0;
        gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048]{};
            gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGE("opengl: GLSL %s/%s: %s", entry, profile, log);
            LOG_FIRST(4, "opengl: the GLSL:\n%s", glsl.c_str());
            gl::DeleteShader(shader);
            return out;
        }
        out.shader = shader;
        return out;
    }
    // last: no tessellation or geometry stage follows this vertex stage
    Stage* CachedStage(const std::string& source, const char* entry, const char* profile, bool last = true) {
        const bool flip = !clip_control_ && last && (profile[0] == 'v' || profile[0] == 'd' || profile[0] == 'g');
        const uint64_t key = SourceHash(source, entry, profile) ^ (flip ? kFlipKey : 0);
        auto [it, added] = stages_.try_emplace(key);
        if (added) it->second = Compile(source, entry, profile, flip);
        return it->second.shader ? &it->second : nullptr;
    }
    Program* Link(std::initializer_list<const Stage*> stages) {
        uint64_t key = 0x51ED27;
        for (const Stage* s : stages)
            if (s) key = Hash(&s->hash, sizeof(s->hash), key);
        auto [it, added] = programs_.try_emplace(key);
        Program& p = it->second;
        if (!added) return p.id ? &p : nullptr;
        PerfScope timing{g_perf.compile};
        g_perf.psos++;
        const GLuint id = gl::CreateProgram();
        for (const Stage* s : stages) {
            if (!s) continue;
            gl::AttachShader(id, s->shader);
            for (const Bind& b : s->binds)
                if (std::none_of(p.binds.begin(), p.binds.end(), [&](const Bind& o) { return o.unit == b.unit; }))
                    p.binds.push_back(b);
        }
        gl::LinkProgram(id);
        GLint ok = 0;
        gl::GetProgramiv(id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048]{};
            gl::GetProgramInfoLog(id, sizeof(log), nullptr, log);
            LOGE("opengl: link: %s", log);
            for (const Stage* s : stages)
                if (s && !s->source.empty()) LOG_FIRST(2, "opengl: the vertex stage's HLSL:\n%s", s->source.c_str());
            gl::DeleteProgram(id);
            p.binds.clear();
            return nullptr;
        }
        p.id = id;
        return &p;
    }
    Stage* PassStage(const char* source, const char* entry) {
        std::string name = std::string(entry) + '\n' + source;
        auto it = pass_stages_.find(name);
        if (it != pass_stages_.end()) return it->second;
        Stage* s = CachedStage(source, entry, "ps_5_0");
        pass_stages_.emplace(std::move(name), s);
        return s;
    }

    // ---- state, set only when it differs from what is set
    void SetEnable(GLenum cap, bool on, bool& cur) {
        if (bound_.valid && cur == on) return;
        on ? gl::Enable(cap) : gl::Disable(cap);
        cur = on;
    }
    void SetProgram(GLuint id) {
        if (bound_.valid && bound_.program == id) return;
        gl::UseProgram(id);
        bound_.program = id;
    }
    void SetDrawFbo(GLuint fbo) {
        if (bound_.valid && bound_.draw_fbo == fbo) return;
        gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        bound_.draw_fbo = fbo;
    }
    void SetReadFbo(GLuint fbo) {
        if (bound_.valid && bound_.read_fbo == fbo) return;
        gl::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        bound_.read_fbo = fbo;
    }
    void SetViewport(const Viewport& v) {
        if (bound_.valid && !std::memcmp(&bound_.viewport, &v, sizeof(v))) return;
        if (gl::ViewportIndexedf) gl::ViewportIndexedf(0, v.x, v.y, v.w, v.h);
        else gl::Viewport(GLint(std::lround(v.x)), GLint(std::lround(v.y)), GLsizei(std::lround(v.w)),
                          GLsizei(std::lround(v.h)));
        bound_.viewport = v;
    }
    void SetScissor(const Rect& r) {
        SetEnable(GL_SCISSOR_TEST, true, bound_.scissor);
        if (bound_.valid && !std::memcmp(&bound_.scissor_rect, &r, sizeof(r))) return;
        gl::Scissor(r.left, r.top, std::max(r.right - r.left, 0), std::max(r.bottom - r.top, 0));
        bound_.scissor_rect = r;
    }
    void SetMasks(uint8_t color, bool depth, uint32_t stencil) {
        if (!bound_.valid || bound_.mask != color) {
            gl::ColorMask(color & 1, (color >> 1) & 1, (color >> 2) & 1, (color >> 3) & 1);
            bound_.mask = color;
        }
        if (!bound_.valid || bound_.zwrite != depth) {
            gl::DepthMask(depth);
            bound_.zwrite = depth;
        }
        if (!bound_.valid || bound_.swmask != stencil) {
            gl::StencilMask(stencil);
            bound_.swmask = stencil;
        }
    }
    void SetBlend(bool on, GLenum src, GLenum dst, GLenum src_alpha, GLenum dst_alpha, GLenum op, GLenum op_alpha,
                  const float* color = nullptr) {
        SetEnable(GL_BLEND, on, bound_.blend);
        if (!on) return;
        if (!bound_.valid || bound_.src != src || bound_.dst != dst || bound_.src_alpha != src_alpha ||
            bound_.dst_alpha != dst_alpha) {
            gl::BlendFuncSeparate(src, dst, src_alpha, dst_alpha);
            bound_.src = src, bound_.dst = dst, bound_.src_alpha = src_alpha, bound_.dst_alpha = dst_alpha;
        }
        if (!bound_.valid || bound_.op != op || bound_.op_alpha != op_alpha) {
            gl::BlendEquationSeparate(op, op_alpha);
            bound_.op = op, bound_.op_alpha = op_alpha;
        }
        if (color && (!bound_.valid || std::memcmp(bound_.blend_color, color, sizeof(bound_.blend_color)))) {
            gl::BlendColor(color[0], color[1], color[2], color[3]);
            std::memcpy(bound_.blend_color, color, sizeof(bound_.blend_color));
        }
    }
    void SetDepth(bool test, GLenum func) {
        SetEnable(GL_DEPTH_TEST, test, bound_.depth);
        if (test && (!bound_.valid || bound_.zfunc != func)) {
            gl::DepthFunc(func);
            bound_.zfunc = func;
        }
    }
    void SetStencil(bool on, GLenum func, uint32_t ref, uint32_t mask, GLenum fail, GLenum zfail, GLenum pass) {
        SetEnable(GL_STENCIL_TEST, on, bound_.stencil);
        if (!on) return;
        if (!bound_.valid || bound_.sfunc != func || bound_.sref != ref || bound_.smask != mask) {
            gl::StencilFunc(func, GLint(ref), mask);
            bound_.sfunc = func, bound_.sref = ref, bound_.smask = mask;
        }
        if (!bound_.valid || bound_.sfail != fail || bound_.szfail != zfail || bound_.spass != pass) {
            gl::StencilOp(fail, zfail, pass);
            bound_.sfail = fail, bound_.szfail = zfail, bound_.spass = pass;
        }
    }
    void SetRaster(bool cull, GLenum front, bool wire, float zslope, float zbias) {
        SetEnable(GL_CULL_FACE, cull, bound_.cull);
        if (!bound_.valid || bound_.front != front) {
            gl::FrontFace(clip_control_ ? front : front == GL_CW ? GL_CCW : GL_CW);  // mirrored by the vertex stage
            bound_.front = front;
        }
        const GLenum polygon = wire ? GL_LINE : GL_FILL;
        if (!bound_.valid || bound_.polygon != polygon) {
            gl::PolygonMode(GL_FRONT_AND_BACK, polygon);
            bound_.polygon = polygon;
        }
        const bool offset = zslope != 0 || zbias != 0;
        if (!bound_.valid || bound_.offset != offset) {
            for (GLenum cap : {GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_LINE, GL_POLYGON_OFFSET_POINT})
                offset ? gl::Enable(cap) : gl::Disable(cap);
            bound_.offset = offset;
        }
        if (offset && (!bound_.valid || bound_.zslope != zslope || bound_.zbias != zbias)) {
            gl::PolygonOffset(zslope, zbias);
            bound_.zslope = zslope, bound_.zbias = zbias;
        }
    }
    void SetTexture(int unit, GLenum target, GLuint tex) {
        if (bound_.valid && bound_.unit_tex[unit] == tex && bound_.unit_target[unit] == target) return;
        gl::ActiveTexture(GL_TEXTURE0 + unit);
        // another target's texture left on the unit would be read by nothing, but is unbound so a deleted name's
        // binding never lingers in what is cached
        if (bound_.unit_tex[unit] && bound_.unit_target[unit] != target)
            gl::BindTexture(bound_.unit_target[unit], 0);
        gl::BindTexture(target, tex);
        bound_.unit_tex[unit] = tex;
        bound_.unit_target[unit] = target;
    }
    void SetSampler(int unit, GLuint sampler) {
        if (bound_.valid && bound_.unit_sampler[unit] == sampler) return;
        gl::BindSampler(unit, sampler);
        bound_.unit_sampler[unit] = sampler;
    }
    // the GL state as nothing of ours knows it (ImGui drew, or nothing was set yet): everything set again
    void Invalidate() {
        bound_ = Bound{};
        last_vsc_valid_ = last_dc_valid_ = false;
    }
    void Validate() { bound_.valid = true; }
    // what a clear or a blit is affected by, off: every channel written, no scissor
    void Neutral() {
        SetEnable(GL_SCISSOR_TEST, false, bound_.scissor);
        SetMasks(15, true, 255);
        Validate();
    }
    // a full-screen triangle's state: Neutral, nothing tested or blended, no culling
    void PassState() {
        Neutral();
        SetBlend(false, 0, 0, 0, 0, 0, 0);
        SetDepth(false, GL_ALWAYS);
        SetStencil(false, 0, 0, 0, 0, 0, 0);
        SetRaster(false, GL_CW, false, 0, 0);
        Validate();
    }
    void BindViews(const Program& p, const View* views, size_t view_count, const GLuint* samplers,
                   size_t sampler_count) {
        for (const Bind& b : p.binds) {
            const View v = b.t < view_count ? views[b.t] : View{};
            SetTexture(b.unit, b.target, v.target == b.target ? v.tex : 0);
            SetSampler(b.unit, b.s < sampler_count ? samplers[b.s] : 0);
        }
    }

    // ---- framebuffers: one for each pair of color and depth textures drawn to
    GLuint Framebuffer(GLuint color, GLuint depth) {
        const uint64_t key = uint64_t(color) | uint64_t(depth) << 32;
        auto it = framebuffers_.find(key);
        if (it != framebuffers_.end()) return it->second;
        GLuint fbo = 0;
        gl::GenFramebuffers(1, &fbo);
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        bound_.draw_fbo = bound_.read_fbo = fbo;
        if (color) gl::FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
        if (depth) gl::FramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0);
        if (!color) {
            gl::DrawBuffer(GL_NONE);
            gl::ReadBuffer(GL_NONE);
        }
        const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) LOG_FIRST(8, "opengl: framebuffer incomplete (0x%X)", unsigned(status));
        framebuffers_.emplace(key, fbo);
        return fbo;
    }
    GLuint FramebufferOf(const Image& im) { return im.depth ? Framebuffer(0, im.tex.get()) : Framebuffer(im.tex.get(), 0); }
    void Forget(GLuint tex) {
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
            if (GLuint(it->first) == tex || GLuint(it->first >> 32) == tex) {
                if (bound_.draw_fbo == it->second) bound_.draw_fbo = ~0u;
                if (bound_.read_fbo == it->second) bound_.read_fbo = ~0u;
                gl::DeleteFramebuffers(1, &it->second);
                it = framebuffers_.erase(it);
            } else {
                ++it;
            }
        }
        for (int u = 0; u < kUnits; u++)
            if (bound_.unit_tex[u] == tex) bound_.unit_tex[u] = 0;
    }

    // ---- buffers
    // room for bytes more in one store: grown or orphaned now if it hasn't, so the Puts that follow (a draw's streams,
    // its two constant blocks) land in the same store; one started over between them leaves those before behind
    void Reserve(Ring& r, uint32_t bytes) {
        if (bytes > r.capacity / 4 || !r.capacity) {
            const uint32_t wanted = std::max(r.least, std::bit_ceil(bytes * 4));
            if (wanted > r.capacity) {
                NewStore(r, wanted);
                return;
            }
        }
        if (uint64_t(r.next) + bytes > r.capacity) NewStore(r, r.capacity);
    }
    // a new store of capacity bytes, written from its start: the old one orphaned (the GPU keeps it), or, mapped,
    // fenced and kept for when the GPU is past it (a grown ring's old ones deleted, the driver keeping them as long)
    void NewStore(Ring& r, uint32_t capacity) {
        r.generation++;
        r.next = 0;
        if (!persistent_) {
            if (!r.buffer) gl::GenBuffers(1, &r.buffer);
            gl::BindBuffer(r.target, r.buffer);
            gl::BufferData(r.target, capacity, nullptr, GL_STREAM_DRAW);
            r.capacity = capacity;
            return;
        }
        if (r.buffer) {
            if (r.mapped && capacity == r.capacity)
                r.spare.push_back({r.buffer, r.mapped, gl::FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
            else gl::DeleteBuffers(1, &r.buffer);
            r.buffer = 0;
            r.mapped = nullptr;
        }
        if (capacity != r.capacity) {
            for (const auto& old : r.spare) {
                gl::DeleteSync(old.done);
                gl::DeleteBuffers(1, &old.buffer);
            }
            r.spare.clear();
            r.capacity = capacity;
        }
        // the oldest kept store, once the GPU is past it; waited for when there are many (the GPU frames behind)
        while (!r.spare.empty()) {
            const Ring::Spare old = r.spare.front();
            const bool many = r.spare.size() >= kRingStores;
            const GLenum w = gl::ClientWaitSync(old.done, GL_SYNC_FLUSH_COMMANDS_BIT, many ? 1'000'000'000ull : 0);
            if (w != GL_ALREADY_SIGNALED && w != GL_CONDITION_SATISFIED) {
                if (many) LOG_FIRST(4, "opengl: a ring's store still in use after a second (0x%X)", unsigned(w));
                break;
            }
            r.spare.pop_front();
            gl::DeleteSync(old.done);
            r.buffer = old.buffer;
            r.mapped = old.mapped;
            gl::BindBuffer(r.target, r.buffer);
            return;
        }
        constexpr GLbitfield kFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        gl::GenBuffers(1, &r.buffer);
        gl::BindBuffer(r.target, r.buffer);
        gl::BufferStorage(r.target, capacity, nullptr, kFlags);
        r.mapped = static_cast<uint8_t*>(gl::MapBufferRange(r.target, 0, capacity, kFlags));
        if (!r.mapped) LOG_FIRST(4, "opengl: persistent map failed (0x%X)", unsigned(gl::GetError()));
    }
    bool Put(Ring& r, const void* data, uint32_t bytes, uint32_t align, uint32_t& offset) {
        if (bytes > r.capacity / 4 || !r.capacity) {
            const uint32_t wanted = std::max(r.least, std::bit_ceil(bytes * 4));
            if (wanted > r.capacity) NewStore(r, wanted);
        }
        uint32_t at = (r.next + align - 1) / align * align;
        if (uint64_t(at) + bytes > r.capacity) {
            NewStore(r, r.capacity);
            at = 0;
        }
        if (r.mapped) {
            // the index list's binding is the vertex array's: set as before; the others are bound by range or
            // attribute
            if (r.target == GL_ELEMENT_ARRAY_BUFFER) gl::BindBuffer(r.target, r.buffer);
            std::memcpy(r.mapped + at, data, bytes);
        } else {
            gl::BindBuffer(r.target, r.buffer);
            void* p = gl::MapBufferRange(r.target, at, bytes,
                                         GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (!p) {
                LOG_FIRST(8, "opengl: buffer map failed (0x%X)", unsigned(gl::GetError()));
                return false;
            }
            std::memcpy(p, data, bytes);
            gl::UnmapBuffer(r.target);
        }
        offset = at;
        r.next = at + bytes;
        r.frame_bytes += bytes;
        g_perf.upload += bytes;
        return true;
    }
    // a uniform block's constants at binding, from the ring (at least min bytes bound, zeros past size)
    bool Constants(GLuint binding, const void* data, uint32_t size, uint32_t* where = nullptr) {
        // the draws' registers (b0) and draw constants (b1) bound again by the next draw: a pass took the binding
        (binding ? last_dc_valid_ : last_vsc_valid_) = false;
        uint32_t offset = 0;
        const uint64_t generation = uniforms_.generation;
        if (!Put(uniforms_, data, size, uint32_t(ubo_align_), offset)) return false;
        gl::BindBufferRange(GL_UNIFORM_BUFFER, binding, uniforms_.buffer, offset, size);
        if (where) *where = offset;
        // a new store: the draws' other block, kept bound from draw to draw while unchanged (or under a fur draw's
        // shells), written to it again; its range in the old store now reads unwritten bytes (flickering draws)
        if (uniforms_.generation != generation) {
            if (binding != 0 && last_vsc_valid_) {
                if (Put(uniforms_, last_vsc_.data(), sizeof(last_vsc_), uint32_t(ubo_align_), offset))
                    gl::BindBufferRange(GL_UNIFORM_BUFFER, 0, uniforms_.buffer, offset, sizeof(last_vsc_));
                else last_vsc_valid_ = false;
            }
            if (binding != 1 && last_dc_valid_) {
                if (Put(uniforms_, &last_dc_, sizeof(last_dc_), uint32_t(ubo_align_), offset))
                    gl::BindBufferRange(GL_UNIFORM_BUFFER, 1, uniforms_.buffer, offset, sizeof(last_dc_));
                else last_dc_valid_ = false;
            }
        }
        return true;
    }

    // ---- textures
    GLuint NewTexture(GLenum target) {
        GLuint id = 0;
        gl::GenTextures(1, &id);
        gl::ActiveTexture(kEditUnit);
        gl::BindTexture(target, id);
        return id;
    }
    // the texture just made: complete with levels levels, filtered nearest (a unit without a sampler reads it so)
    void Complete(GLenum target, uint32_t levels) {
        if (target == GL_TEXTURE_2D_MULTISAMPLE) return;
        gl::TexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::TexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        gl::TexParameteri(target, GL_TEXTURE_MAX_LEVEL, GLint(levels ? levels - 1 : 0));
    }
    bool ColorTexture(Image& im, GLenum internal, GLenum format, GLenum type, const void* pixels = nullptr,
                      uint32_t row = 0) {
        im.target = GL_TEXTURE_2D;
        im.tex = Tex(NewTexture(GL_TEXTURE_2D));
        if (row) gl::PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(row));
        gl::TexImage2D(GL_TEXTURE_2D, 0, GLint(internal), GLsizei(im.w), GLsizei(im.h), 0, format, type, pixels);
        if (row) gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        Complete(GL_TEXTURE_2D, 1);
        return true;
    }
    View GetField(ShadingField (*read)()) {
        if (!read) return {};
        const auto data = read();
        if (!data.data || !data.w || !data.h) return {};
        auto& field = fields_[read];
        if (!field.tex || field.w != data.w || field.h != data.h || field.epoch != data.version) {
            if (!field.tex || field.w != data.w || field.h != data.h) {
                field = Image{};
                field.w = data.w;
                field.h = data.h;
            }
            ColorTexture(field, GL_R32F, GL_RED, GL_FLOAT, data.data);
            field.epoch = data.version;
        }
        return field.view();
    }
    uint32_t RenderSize(float size) const {
        return uint32_t(std::clamp(std::lround(size), 1L, long(max_texture_)));
    }
    bool CreateTargetImage(Image& im) {
        im.target = im.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
        im.tex = Tex(NewTexture(im.target));
        const GLenum internal = im.depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8;
        if (im.samples > 1) {
            gl::TexImage2DMultisample(im.target, GLsizei(im.samples), internal, GLsizei(im.w), GLsizei(im.h), GL_TRUE);
        } else {
            gl::TexImage2D(im.target, 0, GLint(internal), GLsizei(im.w), GLsizei(im.h), 0,
                           im.depth ? GL_DEPTH_STENCIL : GL_BGRA,
                           im.depth ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_BYTE, nullptr);
            Complete(im.target, 1);
        }
        if (im.depth) {
            SetDrawFbo(Framebuffer(0, im.tex.get()));
            Neutral();
            gl::ClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
        return true;
    }
    // what a framebuffer's attachments are read as, copied so drawing into one while reading it is no feedback loop
    void Blit(const Image& source, const Rect& from, const Image& destination, const Rect& to, GLbitfield mask,
              GLenum filter) {
        Neutral();
        SetReadFbo(FramebufferOf(source));
        SetDrawFbo(FramebufferOf(destination));
        gl::BlitFramebuffer(from.left, from.top, from.right, from.bottom, to.left, to.top, to.right, to.bottom, mask,
                            filter);
    }
    static GLbitfield Mask(const Image& im) {
        return im.depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT;
    }
    Image* SnapshotResource(Image& source) {
        auto& copy = feedback_[ResourceKey(source.data, source.depth) | (uint64_t(source.samples) << 40)];
        if (!copy.tex || copy.w != source.w || copy.h != source.h || copy.samples != source.samples) {
            copy = Image{};
            copy.w = source.w;
            copy.h = source.h;
            copy.depth = source.depth;
            copy.data = source.data;
            copy.guest = source.guest;
            copy.samples = source.samples;
            if (!CreateTargetImage(copy)) return nullptr;
        }
        Blit(source, WholeImage(source), copy, WholeImage(copy), Mask(source), GL_NEAREST);
        copy.drawn = false;  // its stencil copy (StencilView) stale
        return &copy;
    }
    // a depth snapshot's stencil, as a stencil texture (a copy in stencil index mode); none without stencil textures
    View StencilView(Image& snapshot) {
        if (!stencil_texturing_ || !snapshot.depth) return {};
        if (!snapshot.stencil) {
            Image s;
            s.w = snapshot.w;
            s.h = snapshot.h;
            s.depth = true;
            s.samples = snapshot.samples;
            if (!CreateTargetImage(s)) return {};
            gl::ActiveTexture(kEditUnit);
            gl::BindTexture(s.target, s.tex.get());
            gl::TexParameteri(s.target, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
            snapshot.stencil = std::move(s.tex);
            snapshot.drawn = false;
        }
        if (!snapshot.drawn) {
            Neutral();
            SetReadFbo(FramebufferOf(snapshot));
            SetDrawFbo(Framebuffer(0, snapshot.stencil.get()));
            gl::BlitFramebuffer(0, 0, GLint(snapshot.w), GLint(snapshot.h), 0, 0, GLint(snapshot.w),
                                GLint(snapshot.h), GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
            snapshot.drawn = true;
        }
        return {snapshot.stencil.get(), snapshot.target};
    }
    Image* DepthBytes(Image& source, uint32_t format, Image* furred = nullptr) {
        const uint64_t key = uint64_t(source.data) << 32 | format;
        auto& out = depth_bytes_[key];
        if (!out.tex || out.w != source.w || out.h != source.h) {
            out = Image{};
            out.w = source.w;
            out.h = source.h;
            out.guest = source.guest;
            if (!CreateTargetImage(out)) return nullptr;
        }
        const bool multisampled = source.ms && source.newest == Image::Newest::Multisampled;
        Image* sampled = SnapshotResource(multisampled ? *source.ms : source);
        if (!sampled) return nullptr;
        if (furred && !Resolve(*furred)) furred = nullptr;
        // GL's stencil texture has the stencil in red; multisampled, the size in constants 8 and 9
        float constants[12]{};
        constants[8] = float(source.w);
        constants[9] = float(source.h);
        DepthByteOrder(format, constants);
        constants[7] = g_outlines.load(std::memory_order_relaxed) ? 1.0f : 0.0f;  // the marks left out
        if (furred) {
            constants[4] = 1;
            constants[5] = float(furred->w) / source.w;
            constants[6] = float(source.w) / source.guest.w;
        }
        const Viewport viewport{0, 0, float(out.w), float(out.h)};
        const View stencil = StencilView(*sampled);
        return FullScreenPass(multisampled ? GlDepthBytesMultisampled().c_str() : kDepthBytesHlsl, "psmain", out, viewport,
                              {sampled->view(), stencil, furred ? furred->view() : View{}}, constants)
                   ? &out
                   : nullptr;
    }
    // a full-screen triangle of ps into destination's viewport, views at t0 up, the pass constants at b1 and b0
    bool Pass(Stage* ps, Image& destination, const Viewport& viewport, std::initializer_list<View> views,
              const void* b0 = nullptr, uint32_t b0_size = 0, const float* b1 = nullptr) {
        if (!ps) return false;
        Program* p = Link({blit_vs_, ps});
        if (!p) return false;
        PassState();
        SetDrawFbo(destination.depth ? Framebuffer(0, destination.tex.get()) : Framebuffer(destination.tex.get(), 0));
        SetViewport(viewport);
        SetProgram(p->id);
        if (b0 && !Constants(0, b0, b0_size)) return false;
        if (b1 && !Constants(1, b1, sizeof(float) * kPassConstantFloats)) return false;
        const GLuint samplers[] = {blit_sampler_, copy_sampler_};
        BindViews(*p, views.begin(), views.size(), samplers, 2);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        return true;
    }
    bool FullScreenPass(const char* source, const char* entry, Image& destination, const Viewport& viewport,
                        std::initializer_list<View> views, std::span<const float> constants) {
        float padded[kPassConstantFloats]{};
        if (constants.size() > kPassConstantFloats) return false;
        std::copy(constants.begin(), constants.end(), padded);
        return Pass(PassStage(source, entry), destination, viewport, views, nullptr, 0, padded);
    }
    // source's from into destination's to, scaled with sampler's filter: a blit, or a draw into a multisampled one
    bool BlitColor(const Image& source, const Rect& from, Image& destination, const Rect& to, GLuint sampler) {
        if (destination.samples == 1 && source.samples == 1) {
            Blit(source, from, destination, to, GL_COLOR_BUFFER_BIT, sampler == copy_sampler_ ? GL_NEAREST : GL_LINEAR);
            return true;
        }
        const float uv[] = {float(from.left) / source.w, float(from.top) / source.h,
                            float(from.right - from.left) / source.w, float(from.bottom - from.top) / source.h};
        const Viewport vp{float(to.left), float(to.top), float(to.right - to.left), float(to.bottom - to.top)};
        if (!copy_ps_) return false;
        Program* p = Link({blit_vs_, copy_ps_});
        if (!p) return false;
        PassState();
        SetDrawFbo(Framebuffer(destination.tex.get(), 0));
        SetViewport(vp);
        SetProgram(p->id);
        if (!Constants(0, uv, sizeof(uv))) return false;
        const View view = source.view();
        BindViews(*p, &view, 1, &sampler, 1);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        return true;
    }
    // source's depth and stencil into destination: a blit (resolving, or scaled), or into a multisampled target from a
    // single sampled one, drawn (the stencil a bit a pass, kept only with stencil textures)
    bool PreserveDepth(Image& source, Image& destination) {
        if (destination.samples == 1 || destination.samples == source.samples) {
            if (source.samples > 1 && (source.w != destination.w || source.h != destination.h)) return false;
            Blit(source, WholeImage(source), destination, WholeImage(destination),
                 GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
            return true;
        }
        if (source.samples != 1 || !copy_depth_ps_) return false;
        Image* sampled = SnapshotResource(source);
        if (!sampled) return false;
        const View stencil = StencilView(*sampled);
        const View views[] = {sampled->view(), stencil};
        SetDrawFbo(Framebuffer(0, destination.tex.get()));
        Neutral();
        gl::ClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
        Program* depth = Link({blit_vs_, copy_depth_ps_});
        if (!depth) return false;
        PassState();
        SetViewport({0, 0, float(destination.w), float(destination.h)});
        SetDepth(true, GL_ALWAYS);
        SetMasks(0, true, 0);
        SetProgram(depth->id);
        BindViews(*depth, views, 2, nullptr, 0);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        Program* bits = stencil.tex && copy_stencil_ps_ ? Link({blit_vs_, copy_stencil_ps_}) : nullptr;
        if (bits) {
            SetDepth(false, GL_ALWAYS);
            SetProgram(bits->id);
            BindViews(*bits, views, 2, nullptr, 0);
            for (uint32_t i = 0; i < 8; ++i) {
                const uint32_t constants[4] = {1u << i};
                if (!Constants(0, constants, sizeof(constants))) break;
                SetMasks(0, false, 1u << i);
                SetStencil(true, GL_ALWAYS, 255, 255, GL_KEEP, GL_KEEP, GL_REPLACE);
                gl::DrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        return true;
    }
    Viewport PictureViewport() const {
        const float aspect = app::FrameAspect();
        const float width = std::min(float(ww_), wh_ * aspect);
        const float height = width / aspect;
        return {(ww_ - width) / 2, (wh_ - height) / 2, width, height};
    }
    bool SmaaLookup(Image& image, GLenum internal, GLenum format, uint32_t width, uint32_t height,
                    const uint8_t* pixels) {
        if (image.tex) return true;
        image.w = width;
        image.h = height;
        gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        return ColorTexture(image, internal, format, GL_UNSIGNED_BYTE, pixels);
    }
    // what Targets asks of a backend, beside the images' own making and copying
    static bool HasTexture(const Image& image) { return bool(image.tex); }
    bool BlitWhole(const Image& source, Image& destination, bool linear) {
        return BlitColor(source, WholeImage(source), destination, WholeImage(destination),
                         linear ? blit_sampler_ : copy_sampler_);
    }
    void ResolveColor(const Image& source, Image& destination) {
        Blit(source, WholeImage(source), destination, WholeImage(destination), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    bool CopyPixels(const Image& source, GuestRect source_rect, Image& destination, GuestRect destination_rect) {
        const auto from = RenderRect(source, source_rect);
        const auto to = RenderRect(destination, destination_rect);
        if (Empty(from) || Empty(to)) return false;
        Blit(source, from, destination, to, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        return true;
    }
    void BeforeResize() {}
    // what DrawSteps asks of a backend
    bool VertexConstants(const void* data, size_t size) { return Constants(0, data, uint32_t(size), &vc_offset_); }
    // what PostEffects asks of a backend, beside FullScreenPass
    static Viewport ViewportOf(uint32_t width, uint32_t height) { return {0, 0, float(width), float(height)}; }
    static std::pair<float, float> ViewportSize(const Viewport& viewport) { return {viewport.w, viewport.h}; }
    static View In(const Image& image) { return image.view(); }
    View StencilIn(Image& image) { return StencilView(image); }
    bool SmaaLookups(View& area, View& search) {
        if (!SmaaLookup(smaa_area_, GL_RG8, GL_RG, kSmaaAreaW, kSmaaAreaH, kSmaaArea) ||
            !SmaaLookup(smaa_search_, GL_R8, GL_RED, kSmaaSearchW, kSmaaSearchH, kSmaaSearch))
            return false;
        area = smaa_area_.view();
        search = smaa_search_.view();
        return true;
    }
    void ClearColor(Image& image) {
        SetDrawFbo(FramebufferOf(image));
        Neutral();
        const float zero[4] = {};
        gl::ClearBufferfv(GL_COLOR, 0, zero);
    }
    // none without stencil textures
    Image* StencilSnapshot(Image& depth) { return stencil_texturing_ ? Snapshot(depth) : nullptr; }
    static const char* OutlineSource() { return kOutlineHlsl; }
    // no HDR output here
    bool HdrHighlights() const { return false; }
    void KeepHdrWorld(Image&) {}
    void Fill(const Image& im, const std::vector<uint8_t>& px, uint32_t pitch) {
        gl::ActiveTexture(kEditUnit);
        gl::BindTexture(GL_TEXTURE_2D, im.tex.get());
        gl::PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / 4));
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(im.w), GLsizei(im.h), GL_BGRA, GL_UNSIGNED_BYTE, px.data());
        gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    Image* Texture(const State& s, int stage, TexInfo& info, bool& target, bool sampled) {
        PerfScope timing{g_perf.bind};
        TextureDesc d;
        info = {};
        target = false;
        if (!s.textures[stage] || !Describe(s.textures[stage], d)) return nullptr;
        const auto& fi = Format(d.format);
        info.kind = TextureKind(d);
        info.rect = d.linear;
        auto ti = targets_.find(ResourceKey(d.data, fi.depth));
        auto surface_format = [](uint32_t f) {
            switch (f) {
                case 0x02: case 0x03: case 0x05: case 0x06: case 0x07: case 0x10: case 0x11: case 0x12: case 0x1C:
                case 0x1E: return true;
                default: return false;
            }
        };
        if (ti != targets_.end() && !fi.depth && frame_ > ti->second.frame + 8 &&
            (ti->second.guest.w != d.width || ti->second.guest.h != d.height || !surface_format(d.format))) {
            targets_.erase(ti);
            ti = targets_.end();
        }
        if (ti != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed && ti->second.guest.w == d.width &&
            ti->second.guest.h == d.height) {
            info.shadow = fi.depth ? ShadowDepthKind(d.format) : 0;
            target = true;
            if (!sampled) return nullptr;
            CpuWrite(ti->second);
            return Resolve(ti->second) ? &ti->second : nullptr;
        }
        if (!fi.depth && !fi.compressed && fi.bits == 32 && !d.cube && d.depth == 1) {
            auto z = targets_.find(ResourceKey(d.data, true));
            if (z != targets_.end() && z->second.guest.w == d.width && z->second.guest.h == d.height) {
                info.kind = kTex2D;
                info.flags = uint8_t(kTexDepthBytes | (DepthByteLayout(d.format) << kTexDepthLayoutShift));
                auto color = targets_.find(ResourceKey(s.rt.data, false));
                Image* furred =
                    color != targets_.end() && color->second.fur_frame == frame_ ? &color->second : nullptr;
                target = true;
                return sampled ? DepthBytes(z->second, d.format, furred) : nullptr;
            }
        }
        info.sign = OffsetSigned(d.format, s.tss[stage][kTssColorSign]);
        info.shadow = fi.depth ? ShadowDepthKind(d.format) : 0;
        if (!sampled) return nullptr;
        StagePalette pal;
        ReadStagePalette(s, stage, d, pal);
        const uint32_t layout[] = {d.data,  d.format, d.width, d.height, d.depth, d.levels, d.pitch,
                                   uint32_t(d.cube), uint32_t(d.linear)};
        uint64_t key = Hash(layout, sizeof(layout));
        key = Hash(&pal.va, sizeof(pal.va), key);
        key = Hash(&info.sign, sizeof(info.sign), key);
        auto& im = textures_[key];
        const uint64_t epoch = DirtyEpoch(d.data);
        if (im.tex && im.frame == frame_ && im.epoch == epoch && im.palette_hash == pal.hash) return &im;
        im.frame = frame_;
        auto check_start = PerfClock::now();
        const uint32_t texture_bytes = TotalSize(d);
        const uint64_t signature = MemSig(d.data, texture_bytes) ^ pal.hash;
        g_perf.sig += PerfMs(check_start);
        g_perf.sigs++;
        g_perf.sig_bytes += texture_bytes;
        if (im.tex && im.signature == signature && im.epoch == epoch) return &im;
        PerfScope uploading{g_perf.texup};
        g_perf.texups++;
        g_perf.texup_bytes += texture_bytes;
        im.frame = frame_;
        im.signature = signature;
        im.epoch = DirtyEpoch(d.data);
        im.w = d.width;
        im.h = d.height;
        im.palette_hash = pal.hash;
        HostFormat fmt = GlFormat(fi.host);
        if (!fmt.internal) return nullptr;
        // BC textures of fewer than 4x4 texels, volumes (no S3TC 3D textures in GL) and S3TC missing: decoded
        const bool expand = fmt.compressed && (!s3tc_ || d.width < 4 || d.height < 4 || d.depth > 1);
        if (expand) fmt = GlFormat(Host::BGRA8);
        const uint32_t levels =
            std::min(d.levels, uint32_t(std::bit_width(std::max({d.width, d.height, d.depth}))));
        const GLenum gl_target = d.cube ? GL_TEXTURE_CUBE_MAP : d.depth > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
        if (!im.tex || im.target != gl_target) {
            im.tex = Tex(NewTexture(gl_target));
            im.target = gl_target;
        } else {
            gl::ActiveTexture(kEditUnit);
            gl::BindTexture(gl_target, im.tex.get());
        }
        const auto* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        std::vector<uint8_t> bytes, slice, all;
        for (uint32_t face = 0; face < (d.cube ? 6u : 1u); face++)
            for (uint32_t level = 0; level < levels; level++) {
                uint32_t pitch = 0;
                DecodeLevel(d, level, face, guest, pal.va ? pal.colors : nullptr, bytes, pitch);
                if (bytes.empty()) return nullptr;
                const uint32_t w = std::max(d.width >> level, 1u), h = std::max(d.height >> level, 1u),
                               slices = std::max(d.depth >> level, 1u);
                if (expand) {
                    const size_t block_slice = size_t(pitch) * std::max((h + 3) / 4, 1u);
                    all.clear();
                    for (uint32_t z = 0; z < slices; z++) {
                        DecodeBlocks(fi.host, bytes.data() + z * block_slice, w, h, slice);
                        AppendBytes(all, slice.data(), slice.size());
                    }
                    bytes.swap(all);
                    pitch = w * 4;
                }
                if (info.sign) {
                    const uint8_t mask[4] = {uint8_t(info.sign & 4 ? 128 : 0), uint8_t(info.sign & 2 ? 128 : 0),
                                             uint8_t(info.sign & 1 ? 128 : 0), uint8_t(info.sign & 8 ? 128 : 0)};
                    for (size_t i = 0; i < bytes.size(); i++) bytes[i] ^= mask[i & 3];
                }
                const GLenum face_target = d.cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : gl_target;
                if (fmt.compressed) {
                    // the block rows tight: GL takes no pitch for compressed data
                    const uint32_t row = std::max((w + 3) / 4, 1u) * (fi.host == Host::BC1 ? 8u : 16u);
                    const uint32_t rows = std::max((h + 3) / 4, 1u);
                    if (pitch > row && bytes.size() >= size_t(pitch) * rows) {
                        for (uint32_t y = 1; y < rows; y++)
                            std::memmove(bytes.data() + size_t(y) * row, bytes.data() + size_t(y) * pitch, row);
                    }
                    bytes.resize(std::min(bytes.size(), size_t(row) * rows));
                    gl::CompressedTexImage2D(face_target, GLint(level), fmt.internal, GLsizei(w), GLsizei(h), 0,
                                             GLsizei(bytes.size()), bytes.data());
                } else {
                    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / fmt.bytes));
                    if (gl_target == GL_TEXTURE_3D) {
                        gl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, GLint(bytes.size() / slices / pitch));
                        gl::TexImage3D(gl_target, GLint(level), GLint(fmt.internal), GLsizei(w), GLsizei(h),
                                       GLsizei(slices), 0, fmt.format, fmt.type, bytes.data());
                        gl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                    } else {
                        gl::TexImage2D(face_target, GLint(level), GLint(fmt.internal), GLsizei(w), GLsizei(h), 0,
                                       fmt.format, fmt.type, bytes.data());
                    }
                    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
            }
        Complete(gl_target, levels);
        return &im;
    }
    GLuint Sampler(const State& s, int stage, bool shadow, bool clamp = false) {
        const auto& t = s.tss[stage];
        struct Desc {
            uint32_t min, mag, wrap_s, wrap_t, wrap_r, aniso, compare, func;
            float bias, min_lod, max_lod, border[4];
        } d{};
        bool min = t[kTssMinFilter] >= 2, mag = t[kTssMagFilter] >= 2, mip = t[kTssMipFilter] >= 2;
        const bool mips = t[kTssMipFilter] != 0;
        d.min = mips ? (min ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                            : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST))
                     : (min ? GL_LINEAR : GL_NEAREST);
        d.mag = mag ? GL_LINEAR : GL_NEAREST;
        const uint32_t aniso = app::Get().aniso ? app::Get().aniso : t[kTssMaxAnisotropy];
        d.aniso = std::clamp(aniso, 1u, 16u);
        if (!(aniso > 1 && t[kTssMinFilter] == 3 && !shadow)) d.aniso = 1;
        auto address = [](uint32_t a) -> uint32_t {
            switch (a == 5 ? 3 : std::clamp(a, 1u, 4u)) {
                case 1: return GL_REPEAT;
                case 2: return GL_MIRRORED_REPEAT;
                case 4: return GL_CLAMP_TO_BORDER;
                default: return GL_CLAMP_TO_EDGE;
            }
        };
        auto unit = [&](uint32_t mode) { return clamp && (mode == 1 || mode == 4) ? 3u : mode; };
        d.wrap_s = address(unit(t[kTssAddressU]));
        d.wrap_t = address(unit(t[kTssAddressV]));
        d.wrap_r = address(t[kTssAddressW]);
        d.bias = std::clamp(Bits(t[kTssMipLodBias]), -16.0f, 15.99f);
        d.min_lod = float(t[kTssMaxMipLevel]);
        d.max_lod = mips ? 1000.0f : 0.0f;
        d.max_lod = std::max(d.max_lod, d.min_lod);
        d.compare = shadow ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE;
        d.func = Compare(s.rs[kRsShadowFunc]);
        Color(t[kTssBorderColor], d.border);
        auto& sampler = samplers_[Hash(&d, sizeof(d))];
        if (!sampler) {
            gl::GenSamplers(1, &sampler);
            gl::SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GLint(d.min));
            gl::SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GLint(d.mag));
            gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GLint(d.wrap_s));
            gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GLint(d.wrap_t));
            gl::SamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GLint(d.wrap_r));
            gl::SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, d.bias);
            gl::SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, d.min_lod);
            gl::SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, d.max_lod);
            gl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GLint(d.compare));
            gl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GLint(d.func));
            gl::SamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, d.border);
            if (anisotropic_) gl::SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, float(d.aniso));
        }
        return sampler;
    }
    GLuint FixedSampler(GLenum filter) {
        GLuint sampler = 0;
        gl::GenSamplers(1, &sampler);
        gl::SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GLint(filter));
        gl::SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GLint(filter));
        for (GLenum wrap : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
            gl::SamplerParameteri(sampler, wrap, GL_CLAMP_TO_EDGE);
        return sampler;
    }
    const Raster& RasterOf(const State& s, const Assembled& a, uint64_t vs, uint32_t samples,
                           DepthPass pass = DepthPass::Original, bool fur_mask = false) {
        PerfScope timing{g_perf.d_pso};
        PsoKey k{};
        k.vs = vs;
        k.regs = a.regs;
        k.topo = uint32_t(a.topology);
        k.samples = samples;
        KeyBlend(s, k);
        KeyDepthStencil(s, a, k);
        KeyRasterizer(s, a, k);
        if (fur_mask && (k.wmask & 7)) k.wmask |= 8 | kWmaskAlphaMax;
        if (pass != DepthPass::Original) k.zwrite = pass == DepthPass::Depth;
        if (pass == DepthPass::Depth) k.blend = k.wmask = k.sten = 0;
        k.vs = k.regs = 0;  // what the state takes alone
        auto [it, added] = rasters_.try_emplace(Hash(&k, sizeof(k)));
        Raster& r = it->second;
        if (!added) return r;
        r.blend = k.blend;
        r.src = BlendFactor(k.src);
        r.dst = BlendFactor(k.dst);
        r.src_alpha = r.src;
        r.dst_alpha = r.dst;
        r.op = r.op_alpha = BlendEquation(k.op);
        r.mask = uint8_t(k.wmask & 15);
        if (k.wmask & kWmaskAlphaMax) {
            if (!k.blend) {
                r.blend = true;
                r.src = GL_ONE;
                r.dst = GL_ZERO;
                r.op = GL_FUNC_ADD;
            }
            r.src_alpha = r.dst_alpha = GL_ONE;
            r.op_alpha = GL_MAX;
        }
        r.zen = k.zen;
        r.zwrite = k.zwrite;
        r.zfunc = Compare(k.zfunc);
        r.sten = k.sten;
        r.smask = k.smask & 255;
        r.swmask = k.swmask & 255;
        r.sfunc = Compare(k.sfunc);
        r.sfail = StencilOperation(k.sfail);
        r.szfail = StencilOperation(k.szfail);
        r.spass = StencilOperation(k.spass);
        // Expanded points have the geometry shader's winding, independent of
        // the last triangle cull mode. Match the original renderer's front face.
        const bool points = a.topology == Topology::Points;
        r.wire = !points && k.fill == kFillWireframe;
        r.cull = k.cull;
        r.front = !points && k.ccw ? GL_CCW : GL_CW;
        r.zbias = float(k.zbias);
        r.zslope = k.zslope;
        return r;
    }
    void ApplyRaster(const Raster& r, uint32_t stencil_ref, const float* blend) {
        SetBlend(r.blend, r.src, r.dst, r.src_alpha, r.dst_alpha, r.op, r.op_alpha, blend);
        SetDepth(r.zen, r.zfunc);
        SetMasks(r.mask, r.zen && r.zwrite, r.sten ? r.swmask : 255);
        SetStencil(r.sten, r.sfunc, stencil_ref, r.smask, r.sfail, r.szfail, r.spass);
        SetRaster(r.cull, r.front, r.wire, r.zslope, r.zbias);
    }
    GLenum mode_ = GL_TRIANGLES;
    GLenum index_type_ = GL_UNSIGNED_SHORT;
    uint32_t index_offset_ = 0;
    // the frame's geometry for the draw drawn again at serial_, when it is a's as assembled (the same streams' sizes
    // and indices)
    // and still in the rings' stores; null to put it in again
    const KeptGeometry* KeptFor(const Assembled& a, uint32_t index_bytes) {
        while (kept_next_ < kept_geometry_.size() && kept_geometry_[kept_next_].serial < serial_) kept_next_++;
        if (kept_next_ >= kept_geometry_.size()) return nullptr;
        const KeptGeometry& k = kept_geometry_[kept_next_];
        if (k.serial != serial_ || k.vertex_store != vertices_.generation || k.buffer_count != a.buffer_count ||
            k.index_bytes != index_bytes || (index_bytes && k.index_store != indices_.generation) ||
            a.buffer_count > 16)
            return nullptr;
        for (uint32_t i = 0; i < a.buffer_count; i++)
            if (k.source_bytes[i] != a.buffers[i].bytes) return nullptr;
        return &k;
    }
    void IssueGeometry(const Assembled& a) {
        if (a.index_count)
            gl::DrawElementsBaseVertex(mode_, GLsizei(a.index_count), index_type_,
                                       reinterpret_cast<const void*>(uintptr_t(index_offset_)), a.base_vertex);
        else
            gl::DrawArrays(mode_, 0, GLsizei(a.draw_count));
    }
    // the reports whose queries are done, written in the order made: the GPU ends them in that order, so the first
    // not done ends the look (each look is a driver call per query, once asked every draw for every report)
    void PollReports(bool wait = false) {
        for (auto it = reports_.begin(); it != reports_.end();) {
            double samples = 0;
            bool ready = true;
            for (const auto& q : it->queries) {
                GLuint available = 0;
                for (;;) {
                    gl::GetQueryObjectuiv(q->query, GL_QUERY_RESULT_AVAILABLE, &available);
                    if (available || !wait) break;
                    gl::Flush();
                    std::this_thread::yield();
                }
                if (!available) {
                    ready = false;
                    break;
                }
                GLuint n = 0;
                gl::GetQueryObjectuiv(q->query, GL_QUERY_RESULT, &n);
                samples += double(n) * q->guest_per_sample;
            }
            if (!ready) break;
            kernel::W32(it->address, uint32_t(frame_));
            kernel::W32(it->address + 4, 0);
            kernel::W32(it->address + 8, uint32_t(std::min(std::ceil(samples), double(UINT32_MAX))));
            std::atomic_thread_fence(std::memory_order_release);
            kernel::W32(it->address + 12, 0);
            it = reports_.erase(it);
        }
    }
    bool Resize() {
        uint32_t w = 0, h = 0;
        app::PixelSize(w, h);
        if (!w || !h) return false;
        if (screen_.tex && w == ww_ && h == wh_) return true;
        screen_ = Image{};
        screen_.w = w;
        screen_.h = h;
        if (!CreateTargetImage(screen_)) return false;
        ww_ = w;
        wh_ = h;
        return true;
    }
    void Display(Image* picture = nullptr, bool count = true) {
        if (!context_ || !Resize()) return;
        SetDrawFbo(FramebufferOf(screen_));
        Neutral();
        const float black[4] = {0, 0, 0, 1};
        gl::ClearBufferfv(GL_COLOR, 0, black);
        if (!picture) {
            auto it = targets_.find(shown_);
            if (it != targets_.end()) picture = &it->second;
        }
        const auto vp = PictureViewport();
        if (picture && picture->tex && !picture->depth && picture->samples == 1) {
            if (!app::Get().fsr || !DrawFsr(*picture, screen_, vp)) {
                const float constants[4] = {g_fade.load(), float(app::Get().gamma) / 220.0f,
                                            float(app::Get().brightness) / 100.0f, 0};
                Pass(blit_ps_, screen_, vp, {picture->view()}, constants, sizeof(constants));
            }
        }
        // onto the window, flipped: its rows run bottom up
        Neutral();
        SetReadFbo(FramebufferOf(screen_));
        SetDrawFbo(0);
        gl::BlitFramebuffer(0, 0, GLint(ww_), GLint(wh_), 0, GLint(wh_), GLint(ww_), 0, GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
        if (ui_) {
            std::lock_guard lock(app::UiLock());
            app::UiNewFrame();
            app::UiFontResize();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            if (app::Welcoming()) app::DrawWelcome();
            else if (app::kGame.ui) app::kGame.ui(fps_);
            ImGui::Render();
            if (clip_control_) gl::ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);  // ImGui draws the window's way up
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (clip_control_) gl::ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
            Invalidate();
            gl::BindVertexArray(vao_);
            gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_.buffer);
        }
        const int vsync = app::Get().vsync ? 1 : 0;
        if (vsync != vsync_) {
            SDL_GL_SetSwapInterval(vsync);
            vsync_ = vsync;
        }
        PerfScope timing{g_perf.swap};
        if (!SDL_GL_SwapWindow(window_)) {
            LOG_FIRST(4, "opengl: swap failed: %s", SDL_GetError());
            return;
        }
        if (count) {
            const auto now = std::chrono::steady_clock::now();
            if (fps_since_ == decltype(fps_since_){} || now - fps_since_ > std::chrono::seconds(2)) {
                fps_since_ = now;
                fps_pictures_ = 0;
            } else if (++fps_pictures_; now - fps_since_ >= std::chrono::milliseconds(500)) {
                fps_ = fps_pictures_ / std::chrono::duration<float>(now - fps_since_).count();
                fps_since_ = now;
                fps_pictures_ = 0;
            }
        }
    }
    void FinishPicture(Image& image) {
        if (!post_done_ && post_target_ == ResourceKey(image.data, false) && PostWanted()) ApplyPost(image);
        Resolve(image);
        post_target_ = 0;
        post_done_ = occlusion_done_ = view_ok_ = many_views_ = false;
    }
    void KeepPicture(const Surface& front, uint32_t slot) {
        Image* picture = Target(front, false);
        if (!picture) return;
        CpuWrite(*picture);
        FinishPicture(*picture);
        auto& copy = generated_[slot];
        if (!ScratchColor(copy, picture->w, picture->h)) return;
        Blit(*picture, WholeImage(*picture), copy, WholeImage(copy), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        copy.fur_frame = picture->fur_frame;
        taken_ |= 1u << slot;
    }
    // a vertex attribute at its register's location, set again only when it changed
    bool Attribute(int r, const VertexElement& e, uint32_t stride, uint32_t base) {
        const uint32_t offset = base + e.offset;
        auto& b = bound_.attrib[r];
        if (bound_.valid && (bound_.attribs >> r & 1) && b.format == e.format && b.stride == stride &&
            b.offset == offset && b.buffer == vertices_.buffer)
            return true;
        const AttribFormat f = Attrib(e.format);
        if (!f.size) {
            LOG_FIRST(8, "opengl: vertex format %u unknown", unsigned(e.format));
            return false;
        }
        gl::BindBuffer(GL_ARRAY_BUFFER, vertices_.buffer);
        const void* at = reinterpret_cast<const void*>(uintptr_t(offset));
        if (f.integer) gl::VertexAttribIPointer(GLuint(r), f.size, f.type, GLsizei(stride), at);
        else gl::VertexAttribPointer(GLuint(r), f.size, f.type, f.normalized, GLsizei(stride), at);
        b = {e.format, stride, offset, vertices_.buffer};
        return true;
    }
    void Attributes(uint16_t wanted) {
        const uint16_t was = bound_.valid ? bound_.attribs : uint16_t(~wanted);
        for (int r = 0; r < 16; r++) {
            const bool on = wanted >> r & 1;
            if (bound_.valid && (was >> r & 1) == on) continue;
            on ? gl::EnableVertexAttribArray(GLuint(r)) : gl::DisableVertexAttribArray(GLuint(r));
        }
        bound_.attribs = wanted;
    }

public:
    OpenGLBackend() {
        Self() = this;
        g_forget = [](GLuint tex) {
            if (Self()) Self()->Forget(tex);
        };
    }
    ~OpenGLBackend() override {
        if (context_) {
            Current();
            if (ui_) {
                std::lock_guard lock(app::UiLock());
                ImGui_ImplOpenGL3_Shutdown();
            }
            // every texture deleted while the context is current
            targets_.clear();
            textures_.clear();
            feedback_.clear();
            depth_bytes_.clear();
            fields_.clear();
            generated_ = {};
            recorded_.clear();
            for (Image* im : {&post_edges_, &post_weights_, &post_blend_, &fsr_easu_, &occlusion_, &occlusion_blur_,
                              &motion_blur_, &outline_, &screen_, &smaa_area_, &smaa_search_, &native_color_})
                *im = Image{};
            Release();
            SDL_GL_DestroyContext(context_);
        }
        g_forget = nullptr;
        Self() = nullptr;
    }
    bool Init(uint32_t width, uint32_t height) override {
        g_dlss_state = 4;  // no DLSS here: the menu says so and keeps the render size and FSR 1 (ui.cpp)
        g_opengl = true;
        if (!width || !height) return false;
        guest_frame_ = {width, height};
        window_ = static_cast<SDL_Window*>(app::Window());
        if (!window_ || !(SDL_GetWindowFlags(window_) & SDL_WINDOW_OPENGL)) {
            LOGE("opengl: the window was made for another renderer");
            return false;
        }
        const bool debug = diag::Env("XBE_GL_DEBUG") != nullptr;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
        for (int minor : {2, 1, 0}) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
            if ((context_ = SDL_GL_CreateContext(window_))) break;
        }
        if (!context_) {
            LOGE("opengl: no OpenGL 4.0 context: %s", SDL_GetError());
            return false;
        }
        owner_ = std::this_thread::get_id();
        if (!gl::Load()) return false;
        GLint major = 0, minor = 0;
        gl::GetIntegerv(GL_MAJOR_VERSION, &major);
        gl::GetIntegerv(GL_MINOR_VERSION, &minor);
        const char* renderer = reinterpret_cast<const char*>(gl::GetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(gl::GetString(GL_VERSION));
        LOGR("opengl: %s, %s", renderer ? renderer : "?", version ? version : "?");
        if (renderer && std::find(Adapters().begin(), Adapters().end(), renderer) == Adapters().end())
            Adapters().push_back(renderer);
        std::unordered_set<std::string> extensions;
        GLint count = 0;
        gl::GetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; i++)
            if (auto* e = reinterpret_cast<const char*>(gl::GetStringi(GL_EXTENSIONS, GLuint(i)))) extensions.insert(e);
        const auto has = [&](const char* e) { return extensions.count(e) != 0; };
        const auto since = [&](GLint v) { return major > 4 || (major == 4 && minor >= v); };
        // GLSL 4.00 gets its binding and location layouts from these
        if ((!since(2) && !has("GL_ARB_shading_language_420pack")) ||
            (!since(1) && !has("GL_ARB_separate_shader_objects"))) {
            LOGE("opengl: the driver lacks ARB_shading_language_420pack or ARB_separate_shader_objects");
            return false;
        }
        {
            const char* e = diag::Env("XBE_GL_CLIP_CONTROL");  // 0: as on a driver without it
            clip_control_ = gl::ClipControl && (since(5) || has("GL_ARB_clip_control")) && !(e && *e == '0');
        }
        if (!since(1) && !has("GL_ARB_viewport_array")) gl::ViewportIndexedf = nullptr;
        stencil_texturing_ = has("GL_ARB_stencil_texturing") || major > 4 || (major == 4 && minor >= 3);
        s3tc_ = has("GL_EXT_texture_compression_s3tc");
        anisotropic_ = has("GL_EXT_texture_filter_anisotropic") || has("GL_ARB_texture_filter_anisotropic");
        glsl_version_ = since(2) ? 420 : since(1) ? 410 : 400;
        LOGR("opengl: GLSL %u, clip control %s, stencil textures %s, S3TC %s", glsl_version_,
             clip_control_ ? "yes" : "no", stencil_texturing_ ? "yes" : "no", s3tc_ ? "yes" : "no");
        {
            const char* e = diag::Env("XBE_GL_PERSISTENT");
            persistent_ = gl::BufferStorage && (major > 4 || (major == 4 && minor >= 4) || has("GL_ARB_buffer_storage")) &&
                          !(e && *e == '0');
            if (persistent_) {  // written a frame or more ahead of the GPU: room for a few frames' worth per store
                vertices_.least = 32u << 20;
                indices_.least = 4u << 20;
                uniforms_.least = 16u << 20;
            }
            LOGR("opengl: streamed buffers %s", persistent_ ? "persistently mapped" : "mapped a piece at a time");
        }
        if (!stencil_texturing_) LOGR("opengl: no stencil textures: the outlines and depth's stencil bytes are left out");
        if (debug && gl::DebugMessageCallback) {
            gl::Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            gl::DebugMessageCallback(DebugMessage, nullptr);
        }
        if (clip_control_) gl::ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        gl::GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align_);
        ubo_align_ = std::max(ubo_align_, 16);
        gl::GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_);
        GLint color_samples = 1, depth_samples = 1, max_samples = 1;
        gl::GetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &color_samples);
        gl::GetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &depth_samples);
        gl::GetIntegerv(GL_MAX_SAMPLES, &max_samples);
        for (uint32_t samples : {2u, 4u, 8u})
            if (GLint(samples) <= std::min({color_samples, depth_samples, max_samples})) supported_samples_ |= samples;
        SetHdrAvailable(false);
        gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl::GenVertexArrays(1, &vao_);
        gl::BindVertexArray(vao_);
        Invalidate();
        GlslInit();
        LoadShaderCache();
        blit_sampler_ = FixedSampler(GL_LINEAR);
        copy_sampler_ = FixedSampler(GL_NEAREST);
        blit_vs_ = CachedStage(kBlit, "vs", "vs_5_0");
        blit_ps_ = CachedStage(kBlit, "ps", "ps_5_0");
        copy_ps_ = CachedStage(kCopyColor, "main", "ps_5_0");
        copy_depth_ps_ = CachedStage(kCopyDepthStencil, "copy_depth", "ps_5_0");
        copy_stencil_ps_ = stencil_texturing_ ? CachedStage(kCopyDepthStencil, "copy_stencil", "ps_5_0") : nullptr;
        if (!blit_vs_ || !blit_ps_ || !Link({blit_vs_, blit_ps_})) return false;
        {
            std::lock_guard lock(app::UiLock());
            ui_ = ImGui_ImplOpenGL3_Init("#version 400 core");
        }
        Invalidate();
        gl::BindVertexArray(vao_);
        if (!ui_ || !Resize()) return false;
        ApplyRenderScale();
        Release();  // the thread drawing takes it (Current)
        return true;
    }
    // the title's thread, before the render thread takes the backend over: the context stays with whoever draws, the
    // render size worked out again by the next to (Begin)
    void SetGuestSize(uint32_t width, uint32_t height) override {
        if (!width || !height || (width == guest_frame_.w && height == guest_frame_.h)) return;
        guest_frame_ = {width, height};
        requested_frame_ = {};
        rescale_ = true;
    }
    // g_hud_split: the sides a frame wide HUD draw no longer covers, black (kHudInPlaceBars), on color (target's
    // attachment); over the draw's own edge by a console pixel: its outermost texels blend with what lies under, a
    // hairline where the frame's edge hid it
    void HudBars(Image& color, const Image& target) {
        const float W = float(target.w), sc = W / float(std::max<uint32_t>(target.guest.w, 1));
        const float inset = std::ceil(hud_.scale * sc);
        const float l = std::ceil(hud_.shift * sc + inset), r = std::floor(hud_.shift * sc + W * hud_.scale - inset);
        SetDrawFbo(Framebuffer(color.tex.get(), 0));
        SetMasks(15, bound_.zwrite, bound_.swmask);
        const float black[4] = {0, 0, 0, 1};
        const int32_t H = int32_t(target.h);
        if (l > 0) {
            SetScissor({0, 0, int32_t(std::min(l, W)), H});
            gl::ClearBufferfv(GL_COLOR, 0, black);
        }
        if (r < W) {
            SetScissor({int32_t(std::max(r, 0.0f)), 0, int32_t(W), H});
            gl::ClearBufferfv(GL_COLOR, 0, black);
        }
    }
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        PerfScope timing{g_perf.clear};
        if (!context_) return;
        Begin();
        Image* rt = (flags & kClearTarget) ? Target(s.rt, false) : nullptr;
        Image* ds = (flags & (kClearZ | kClearStencil)) ? Target(s.zs, true) : nullptr;
        if (count && rects && rt && ds && (rt->w != ds->w || rt->h != ds->h)) {
            Clear(s, count, rects, flags & kClearTarget, color, z, stencil);
            Clear(s, count, rects, flags & (kClearZ | kClearStencil), color, z, stencil);
            return;
        }
        const bool multi = (!rt || rt->ms) && (!ds || ds->ms);
        const bool partial = count && rects;
        Image* color_target = rt && multi && !partial ? rt->ms.get() : Attachment(rt, multi);
        const bool both_depth = (flags & (kClearZ | kClearStencil)) == (kClearZ | kClearStencil);
        Image* depth_target = ds && multi && !partial && both_depth ? ds->ms.get() : Attachment(ds, multi);
        if ((rt && !color_target) || (ds && !depth_target)) return;
        if (!rt && !ds) return;
        float c[4];
        Color(color, c);
        SetDrawFbo(Framebuffer(color_target ? color_target->tex.get() : 0, depth_target ? depth_target->tex.get() : 0));
        Neutral();
        const GLint s8 = GLint(stencil & 255);
        auto clear = [&] {
            if (rt) gl::ClearBufferfv(GL_COLOR, 0, c);
            if (ds && (flags & kClearZ) && (flags & kClearStencil)) gl::ClearBufferfi(GL_DEPTH_STENCIL, 0, z, s8);
            else if (ds && (flags & kClearZ)) gl::ClearBufferfv(GL_DEPTH, 0, &z);
            else if (ds && (flags & kClearStencil)) gl::ClearBufferiv(GL_STENCIL, 0, &s8);
        };
        if (partial) {
            const Image& target = rt ? *rt : *ds;
            for (uint32_t i = 0; i < count; ++i) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * 16, 16);
                const auto area = RenderRect(target, {r[0], r[1], r[2], r[3]});
                if (Empty(area)) continue;
                SetScissor(area);
                clear();
            }
        } else {
            clear();
            if (rt) rt->fur_frame = UINT64_MAX;
        }
        if (rt) MarkWritten(*rt, multi);
        if (ds) MarkWritten(*ds, multi);
    }
    void Draw(const State& original, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        PerfScope timing{g_perf.draw};
        g_perf.draws++;
        g_perf.verts += count;
        if (!context_) return;
        Begin();
        hud_.Reset();
        auto step_start = PerfClock::now();
        auto step = [&](unsigned part) {
            driver_timing_[part] += PerfMs(step_start);
            step_start = PerfClock::now();
        };
        const State& s = SpriteState(original, prim);
        const bool uncounted = std::exchange(uncounted_, false);
        if (!uncounted) serial_++;
        const bool as_drawn = std::exchange(as_drawn_, false) && replaying_ && !uncounted;
        if (!reports_.empty() && (++poll_draws_ & 31) == 0) PollReports();
        if (!replaying_) WatchDraw(s, start, count, indices);
        auto assembly_start = PerfClock::now();
        const bool patches = TessellatedDraw(s, prim);
        Assembled& a = assembled_;
        if (!Assemble(s, prim, start, count, indices, a, patches)) return;
        g_perf.assemble += PerfMs(assembly_start);
        const uint32_t index_bytes = a.index_count * (a.index16 ? 2 : 4);
        uint32_t source_bytes[16]{};
        for (uint32_t i = 0; i < a.buffer_count && i < 16; i++) source_bytes[i] = a.buffers[i].bytes;
        // drawn again as the frame drew it: its geometry as the frame put it in the rings, repacked as it was (a left
        // as assembled, its vertices read through its own layout; the attributes take the kept one)
        const KeptGeometry* kept = as_drawn ? KeptFor(a, index_bytes) : nullptr;
        if (!kept && !AlignStreams(a, color_buffers_, [](uint32_t format) { return Attrib(format).bytes; })) return;
        step(0);
        Image* rt = nullptr;
        Image* ds = nullptr;
        bool hud = false;
        if (!DrawTargets(s, a, start, count, indices, rt, ds, hud)) return;
        const Image& target = rt ? *rt : *ds;
        step(1);
        ShaderChoice sh;
        ClassifyShaders(s, a, count, sh);
        if (sh.prog) EnsureProgramHash(s);
        TrackPost(s, a, sh.prog, rt, ds);
        sh.shading = DrawShading(s, sh);
        const ProgramShading* shading = sh.shading ? &g_program_shading[sh.shading - 1] : nullptr;
        StartFurOcclusion(s, shading, rt);
        Stage *hull = nullptr, *domain = nullptr;
        if (a.topology == Topology::Patches && shading) {
            auto [it, inserted] = tessellation_.try_emplace(sh.shading);
            if (inserted) {
                const auto src = TessHlsl(sh.shading);
                it->second = {CachedStage(src, "hs", "hs_5_0"), CachedStage(src, "ds", "ds_5_0")};
            }
            hull = it->second.first;
            domain = it->second.second;
            if (!hull || !domain) hull = domain = nullptr;
        }
        // without glClipControl a vertex stage differs when tessellation or the points' geometry stage follows it
        const bool vs_last = !hull && a.topology != Topology::Points;
        auto [vit, new_vs] =
            vertex_stages_.try_emplace(VertexKey(s, a, sh) ^ (!clip_control_ && vs_last ? kFlipKey : 0));
        if (new_vs) {
            const auto ve = MakeVertexEntry(s, a, sh);
            vit->second = CachedStage(VertexHlsl(ve, DrawProgram(s, sh)), "main", "vs_5_0", vs_last);
        }
        Stage* vs = vit->second;
        if (!vs) return;
        step(2);
        const bool clamp = UnitTexcoords(s, a);
        StageTextures t;
        auto& info = t.info;
        View views[6]{};
        GLuint samplers[6]{};
        ReadStageTextures(s, rt, ds, target, t, views);
        const uint32_t sampled = SampledStages(s);
        for (int i = 0; i < 4; i++)
            if ((sampled >> i) & 1) samplers[i] = Sampler(s, i, info[i].shadow != 0, clamp);
        step(3);
        if (shading) {
            views[4] = GetField(shading->field);
            if (rt && shading->screen)
                if (Image* screen = Snapshot(*rt)) views[5] = screen->view();
            samplers[4] = samplers[5] = blit_sampler_;
        }
        PixelKey pk = MakePixelKey(s, info);
        pk.quad = sh.quad;
        pk.shading = sh.shading;
        auto [pit, new_ps] = pixel_stages_.try_emplace(Hash(&pk, sizeof(pk)));
        if (new_ps) pit->second = CachedStage(PixelHlsl(pk), "main", "ps_5_0");
        Stage* ps = pit->second;
        if (!ps) return;
        const bool multi = (!rt || rt->ms) && (!ds || ds->ms);
        Image* color_target = Attachment(rt, multi);
        Image* depth_target = Attachment(ds, multi);
        if ((rt && !color_target) || (ds && !depth_target)) return;
        const uint32_t samples = (color_target ? color_target : depth_target)->samples;
        Stage* geometry = a.topology == Topology::Points ? CachedStage(PointHlsl(), "main", "gs_5_0") : nullptr;
        if (a.topology == Topology::Points && !geometry) return;
        Program* program = Link({vs, hull, domain, geometry, ps});
        if (!program) return;
        Program* depth_program = nullptr;
        const Raster* depth_raster = nullptr;
        if (const float alpha = DepthPassAlpha(s, start, count, indices, ds, shading); alpha > 0) {
            PixelKey key = pk;
            key.depth_pass = std::bit_cast<uint32_t>(alpha);
            auto [it, added] = pixel_stages_.try_emplace(Hash(&key, sizeof(key)));
            if (added) it->second = CachedStage(PixelHlsl(key), "main", "ps_5_0");
            if (it->second) {
                depth_program = Link({vs, hull, domain, geometry, it->second});
                if (depth_program) depth_raster = &RasterOf(s, a, vs->hash, samples, DepthPass::Depth);
            }
        }
        const bool fur_mask = rt && shading && shading->unsharpened && (app::Get().fsr || DepthOfFieldLately());
        const Raster& raster =
            RasterOf(s, a, vs->hash, samples, depth_program ? DepthPass::Color : DepthPass::Original, fur_mask);
        step(4);
        auto record_start = PerfClock::now();
        uint32_t base[16]{};
        if (kept) {
            std::copy(std::begin(kept->base), std::end(kept->base), base);
            if (index_bytes) {
                gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_.buffer);
                index_offset_ = kept->index_offset;
                index_type_ = kept->index_type;
            }
            g_perf.redrawn++;
        } else {
            uint32_t streams = 0;  // the draw's streams, each aligned: in one store (Reserve)
            for (uint32_t i = 0; i < a.buffer_count; i++) streams += (a.buffers[i].bytes + 15) & ~15u;
            Reserve(vertices_, streams + 16);
            for (uint32_t i = 0; i < a.buffer_count; i++) {
                if (!a.buffers[i].bytes) continue;
                if (!Put(vertices_, a.buffers[i].src, a.buffers[i].bytes, 16, base[i])) return;
            }
            if (index_bytes) {
                if (!Put(indices_, a.index_src, index_bytes, 16, index_offset_)) return;
                index_type_ = a.index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
            }
            if (!replaying_ && !uncounted && keep_geometry_) {
                KeptGeometry& k = kept_geometry_.emplace_back();
                k.serial = serial_;
                k.vertex_store = vertices_.generation;
                k.index_store = indices_.generation;
                k.buffer_count = a.buffer_count;
                k.index_bytes = index_bytes;
                k.index_offset = index_offset_;
                k.index_type = index_type_;
                for (uint32_t i = 0; i < a.buffer_count && i < 16; i++) {
                    k.base[i] = base[i];
                    k.bytes[i] = a.buffers[i].bytes;
                    k.source_bytes[i] = source_bytes[i];
                    k.stride[i] = a.buffers[i].stride;
                }
                for (int r = 0; r < 16; r++) k.offset[r] = a.elements[r].offset;
            }
        }
        gl::BindVertexArray(vao_);
        Attributes(a.regs);
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) {
                VertexElement e = a.elements[r];
                if (e.buffer >= a.buffer_count) return;
                uint32_t stride = a.buffers[e.buffer].stride;
                if (kept) {
                    e.offset = kept->offset[r];
                    stride = kept->stride[e.buffer];
                }
                if (!Attribute(r, e, stride, base[e.buffer])) return;
            }
        step(5);
        DrawConstants c{};
        const GuestExtent extent = target.guest;
        FillDrawConstants(s, a, sh, pk, t, target, &s == &sprite_, c);
        auto constants_start = PerfClock::now();
        const uint64_t store = uniforms_.generation;
        Reserve(uniforms_, 2 * uint32_t(sizeof(s.vsc.v) + sizeof(c) + ubo_align_));
        if (uniforms_.generation != store) last_vsc_valid_ = last_dc_valid_ = false;  // both written to the new store
        if (!last_vsc_valid_ || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc.v))) {
            if (!Constants(0, s.vsc.data(), sizeof(s.vsc.v), &vc_offset_)) return;
            last_vsc_ = s.vsc.v;  // set after Constants, which marks the binding taken
            last_vsc_valid_ = true;
        }
        if (!last_dc_valid_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
            if (!Constants(1, &c, sizeof(c), &dc_offset_)) return;
            last_dc_ = c;
            last_dc_valid_ = true;
        }
        g_perf.d_consts += PerfMs(constants_start);
        step(6);
        SetDrawFbo(Framebuffer(color_target ? color_target->tex.get() : 0,
                               depth_target ? depth_target->tex.get() : 0));
        SetProgram(program->id);
        BindViews(*program, views, 6, samplers, 6);
        float blend[4];
        Color(SimpleState(s, kMBlendColor), blend);
        const uint32_t stencil_ref = SimpleState(s, kMStencilRef) & 255;
        ApplyRaster(raster, stencil_ref, blend);
        // Shared HLSL already applies the Xbox viewport; the host viewport covers the entire target.
        Viewport vp{0, 0, float(target.w), float(target.h)};
        // g_hud_split's remap: the target narrowed to the draw's place, the scissor with it
        const float hud_sc = float(target.w) / float(std::max<uint32_t>(target.guest.w, 1));
        if (hud) {
            vp.x = hud_.shift * hud_sc;
            vp.w = float(target.w) * hud_.scale;
        }
        SetViewport(vp);
        auto rect = RenderRect(target, DrawClip(s));
        FinishScissor(rect, target, hud);
        SetScissor(rect);
        Validate();
        mode_ = hull && domain                     ? GL_PATCHES
                : a.topology == Topology::Points ? GL_POINTS
                : a.topology == Topology::Lines  ? (a.strip ? GL_LINE_STRIP : GL_LINES)
                                                 : (a.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES);
        if (mode_ == GL_PATCHES && bound_.patch != 3) {
            gl::PatchParameteri(GL_PATCH_VERTICES, 3);
            bound_.patch = 3;
        }
        step(7);
        DrawFurShells(s, a, sh.shading, false);
        Query q;
        if (!replaying_ && SimpleState(s, kMCountEnable)) {
            q = std::make_shared<SampleQuery>();
            q->guest_per_sample = (double(extent.w) * extent.h) / (double(target.w) * target.h * samples);
            gl::GenQueries(1, &q->query);
            gl::BeginQuery(GL_SAMPLES_PASSED, q->query);
        }
        IssueGeometry(a);
        if (q) {
            gl::EndQuery(GL_SAMPLES_PASSED);
            counted_.push_back(std::move(q));
        }
        DrawFurShells(s, a, sh.shading, true);
        if (depth_program) {
            SetProgram(depth_program->id);
            BindViews(*depth_program, views, 6, samplers, 6);
            ApplyRaster(*depth_raster, stencil_ref, blend);
            IssueGeometry(a);
        }
        step(8);
        g_perf.d_record += PerfMs(record_start);
        FinishDraw(s, rt, ds, color_target, target, fur_mask, multi, depth_program != nullptr);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) override {
        PerfScope timing{g_perf.copy};
        if (!context_) return;
        Begin();
        CopySurfaces(src, dst, count, rects, points);
    }
    void Present(const Surface& front) override {
        if (!context_) return;
        Begin();
        if (!std::exchange(hud_.rolled, false)) hud_.Roll(guest_frame_.w);
        Image* image = Target(front, false);
        if (image) {
            CpuWrite(*image);
            shown_ = ResourceKey(front.data, false);
        }
        if (!keeping_pictures_ && next_picture_ < generated_count_ && last_picture_ >= 0)
            Display(&generated_[last_picture_]);
        last_picture_ =
            keeping_pictures_ && (taken_ & (1u << (generated_count_ - 1))) ? int(generated_count_ - 1) : -1;
        keeping_pictures_ = false;
        if (image && last_picture_ < 0) FinishPicture(*image);
        if (g_record_dump.exchange(false)) SaveRecorded();
        if (interp::Recording()) {
            if (last_picture_ >= 0) {
                const uint32_t n = uint32_t(std::popcount(taken_));
                for (uint32_t i = 0, k = 0; i < generated_count_; i++)
                    if (taken_ >> i & 1)
                        Record(generated_[i], i + 1 < generated_count_ ? interp::Note(i) : "the frame's own", ++k, n);
            } else if (image) {
                Record(*image, "the frame's own, alone", 1, 1);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto pictures = last_picture_ < 0 ? 1 : std::popcount(taken_);
        using Duration = std::chrono::steady_clock::duration;
        const auto elapsed = last_ == decltype(last_){} ? Duration(std::chrono::milliseconds(16)) : now - last_;
        picture_interval_ =
            std::clamp(elapsed, Duration(std::chrono::milliseconds(4)), Duration(std::chrono::milliseconds(50))) /
            pictures;
        last_ = now;
        {
            PerfScope timing{g_perf.present};
            if (last_picture_ < 0) {
                next_picture_ = generated_count_;
                Display(image);
            } else {
                next_picture_ = 0;
                schedule_ = now;
                Show();
            }
            EndShadingFrame();
            PollReports();
            ApplyRenderScale();
        }
        if (diag::Perf()) {
            LogPerf();
            driver_frames_++;
            if (PerfMs(driver_start_) >= 1000) {
                const double n = driver_frames_;
                LOGR("opengl timing ms/frame: prepare %.2f targets %.2f vs %.2f textures/samplers %.2f ps/program %.2f "
                     "stream %.2f constants %.2f bind %.2f submit/queries %.2f",
                     driver_timing_[0] / n, driver_timing_[1] / n, driver_timing_[2] / n, driver_timing_[3] / n,
                     driver_timing_[4] / n, driver_timing_[5] / n, driver_timing_[6] / n, driver_timing_[7] / n,
                     driver_timing_[8] / n);
                std::fill(std::begin(driver_timing_), std::end(driver_timing_), 0.0);
                driver_frames_ = 0;
                driver_start_ = PerfClock::now();
            }
        }
        kept_geometry_.clear();
        serial_ = 0;
        keep_geometry_ = Pictures() > 1;  // the next frame to be drawn again
        // a frame's geometry and its drawing again in one store, as far as it goes: one started over between them
        // leaves the frame's kept geometry behind (KeptFor), so a frame starts a store of its own when the one in use
        // hasn't room for the last frame's, the ring grown when a frame overfills it
        for (Ring* r : {&vertices_, &indices_}) {
            const uint64_t wanted = uint64_t(r->frame_bytes) * 5 / 4;
            if (r->capacity && wanted > r->capacity && r->capacity < (256u << 20))
                NewStore(*r, std::min(256u << 20, std::bit_ceil(uint32_t(wanted * 2))));
            else if (r->capacity && r->capacity - r->next < wanted)
                NewStore(*r, r->capacity);
            r->frame_bytes = 0;
        }
        frame_++;
        if (frame_ % 120 == 0) {
            for (auto it = textures_.begin(); it != textures_.end();)
                if (it->second.frame + 120 < frame_) it = textures_.erase(it);
                else ++it;
        }
    }
    void Redisplay(bool = true) override {
        if (!context_) return;
        Begin();
        next_picture_ = generated_count_;
        Display(last_picture_ < 0 ? nullptr : &generated_[last_picture_], false);
        ApplyRenderScale();
        PollReports();
        // the welcome screen draws from the window's thread, the title from its own after it: handed over
        if (app::Welcoming()) Release();
    }
    uint32_t Pictures() override {
        if (!context_ || g_capture.load() || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) return 1;
        const auto& set = app::Get();
        const uint32_t wanted = std::clamp(set.frame_generation, 1u, 4u);
        if (!set.vsync || wanted == 1) return wanted;
        // A 60 Hz display with VSync can present one picture per 60 Hz game frame.
        return std::min(wanted, std::max(1u, (app::MonitorRefresh() + 1) / 60));
    }
    // own is always true here: no frame is drawn light (Light) and the last picture time is the frame's own
    void Keep(const Surface& front, bool /*own*/) override {
        Current();
        if (next_picture_ < generated_count_ && last_picture_ >= 0) Display(&generated_[last_picture_]);
        generated_count_ = std::clamp(app::Get().frame_generation, 2u, 4u);
        if (app::Get().vsync) generated_count_ = std::clamp((app::MonitorRefresh() + 1) / 60, 2u, generated_count_);
        next_picture_ = generated_count_;
        taken_ = 0;
        keeping_pictures_ = true;
        KeepPicture(front, generated_count_ - 1);
    }
    void Between(const Surface& front, uint32_t pass) override {
        Current();
        if (pass < generated_count_) KeepPicture(front, pass - 1);
    }
    void AsDrawn(bool same) override { as_drawn_ = same; }
    void Uncounted() override { uncounted_ = true; }
    void Pass() override {
        if (replaying_) serial_++;
    }
    void Replaying(bool on) override {
        serial_ = 0;
        kept_next_ = 0;
        as_drawn_ = uncounted_ = false;
        if (on && !hud_.rolled) {  // the frame's draws again take its own boxes
            hud_.Roll(guest_frame_.w);
            hud_.rolled = true;
        }
        replaying_ = on;
    }
    bool Due(std::chrono::steady_clock::time_point& at) override {
        if (next_picture_ >= generated_count_) return false;
        at = schedule_;
        return true;
    }
    void Show() override {
        Current();
        const auto now = std::chrono::steady_clock::now();
        while (next_picture_ < generated_count_ && schedule_ <= now) {
            const uint32_t slot = next_picture_++;
            schedule_ += picture_interval_;
            if ((taken_ & (1u << slot)) && (schedule_ > now || next_picture_ == generated_count_)) {
                Display(&generated_[slot]);
                break;
            }
        }
    }
    void Sync(uint32_t data, uint32_t) override {
        PerfScope timing{g_perf.sync};
        if (!context_) return;
        Current();
        auto it = targets_.find(ResourceKey(data, false));
        if (it == targets_.end() || !it->second.drawn) return;
        Image& im = it->second;
        if (!Resolve(im)) return;
        TextureDesc guest;
        if (!DescribeSurface(im.surface, guest) || Format(guest.format).host != Host::BGRA8 || !guest.linear) return;
        g_perf.syncs++;
        const Image* source = &im;
        if (im.w != im.guest.w || im.h != im.guest.h) {
            if (!NativeColor(im.guest)) return;
            Blit(im, WholeImage(im), native_color_, WholeImage(native_color_), GL_COLOR_BUFFER_BIT, GL_NEAREST);
            source = &native_color_;
        }
        const uint32_t pitch = guest.pitch ? guest.pitch : GuestPitch(im.guest.w);
        auto* out = static_cast<uint8_t*>(kernel::HostPtr(guest.data));
        Neutral();
        SetReadFbo(FramebufferOf(*source));
        gl::PixelStorei(GL_PACK_ROW_LENGTH, GLint(pitch / 4));
        gl::ReadPixels(0, 0, GLsizei(im.guest.w), GLsizei(im.guest.h), GL_BGRA, GL_UNSIGNED_BYTE, out);
        gl::PixelStorei(GL_PACK_ROW_LENGTH, 0);
        im.drawn = false;
        im.epoch = DirtyEpoch(im.data);
        im.signature = MemSig(guest.data, TotalSize(guest));
    }
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    // XBE_INTERP_RECORD: a picture shown, kept at the console's size with its note in the ring
    void Record(const Image& picture, const std::string& note, uint32_t k, uint32_t n) {
        if (!picture.tex || picture.depth || picture.samples != 1) return;
        recorded_.resize(interp::Recording());
        Recorded& r = recorded_[recorded_next_++ % recorded_.size()];
        if (!ScratchColor(r.im, guest_frame_.w, guest_frame_.h)) return;
        Blit(picture, WholeImage(picture), r.im, WholeImage(r.im), GL_COLOR_BUFFER_BIT, GL_LINEAR);
        char b[96];
        std::snprintf(b, sizeof(b), "frame %llu, picture %u of %u: ", (unsigned long long)frame_, k, n);
        r.note = b + note;
    }
    // F4: the kept pictures, oldest first, as bitmaps beside notes.txt in a folder of their own
    void SaveRecorded() {
        if (recorded_.empty()) {
            LOGR("interp: no pictures kept to save (XBE_INTERP_RECORD)");
            return;
        }
        const std::time_t now = std::time(nullptr);
        const std::tm tm = *std::localtime(&now);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "interp_%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        std::error_code ec;
        std::filesystem::path dir = app::StateDir() / diag::CaptureDir() / stamp;
        for (int n = 2; std::filesystem::exists(dir, ec); n++)
            dir = app::StateDir() / diag::CaptureDir() / (std::string(stamp) + "-" + std::to_string(n));
        std::filesystem::create_directories(dir, ec);
        FILE* notes = OpenFile(dir / "notes.txt", "w");
        uint32_t saved = 0;
        for (size_t k = 0; k < recorded_.size(); k++) {
            Recorded& r = recorded_[(recorded_next_ + k) % recorded_.size()];
            if (!r.im.tex) continue;
            std::vector<uint8_t> px(size_t(r.im.w) * r.im.h * 4);
            Neutral();
            SetReadFbo(FramebufferOf(r.im));
            gl::ReadPixels(0, 0, GLsizei(r.im.w), GLsizei(r.im.h), GL_BGRA, GL_UNSIGNED_BYTE, px.data());
            char name[16];
            std::snprintf(name, sizeof(name), "%04u", saved++);
            WriteBmp(dir / (std::string(name) + ".bmp"), px, r.im.w, r.im.h);
            if (notes) std::fprintf(notes, "%s: %s\n", name, r.note.c_str());
        }
        if (notes) std::fclose(notes);
        LOGR("interp: %u pictures saved to %s", saved, dir.string().c_str());
    }
    void Flush() override {
        if (!context_) return;
        Current();
        gl::Flush();
        PollReports(true);
    }
    void SetGamma(const uint8_t*) override {}  // Presentation gamma comes from app::Settings.
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override {
        if (!context_) return;
        Current();
        reports_.push_back({report, counted_});
        PollReports();
    }
};
}  // namespace

std::unique_ptr<Backend> CreateOpenGLBackend() { return std::make_unique<OpenGLBackend>(); }

}  // namespace gpu
