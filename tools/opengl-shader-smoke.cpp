// The OpenGL renderer's shaders on a real GL context (gpu/opengl.cpp's way: HLSL through glslang and SPIRV-Cross,
// glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)): representative generated shaders and every full-screen pass
// translated, compiled and linked, then drawn where a wrong orientation, point corner or stencil read shows. Build from
// the repository root once the zig-glibc build has fetched its dependencies (SPIRV-Cross in _deps), with the pinned Zig:
//
//   D=out/build/zig-glibc/_deps; X=$D/spirv_cross-src
//   toolchain/zig-*-linux/zig c++ -target x86_64-linux-gnu.2.31 -std=gnu++20 -O1 -DENABLE_HLSL \
//     -Isrc/runtime -I$D/glslang-src -I$D/glslang-build/include -I$X -I$D/sdl3-src/include \
//     tools/opengl-shader-smoke.cpp src/runtime/gpu/opengl_glsl.cpp src/runtime/gpu/shaders.cpp \
//     $X/spirv_cross.cpp $X/spirv_glsl.cpp $X/spirv_cfg.cpp $X/spirv_parser.cpp $X/spirv_cross_parsed_ir.cpp \
//     $D/glslang-build/glslang/libglslang.a $D/glslang-build/glslang/libglslang-default-resource-limits.a \
//     $D/sdl3-build/libSDL3.a -ldl -lpthread -lm -o opengl-shader-smoke
//   ./opengl-shader-smoke        (SMOKE_NO_CLIP_CONTROL=1: without glClipControl; Mesa as Ivy Bridge has it: MESA_GL_VERSION_OVERRIDE=4.2 MESA_GLSL_VERSION_OVERRIDE=420)
#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL3/SDL_opengl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "gpu/opengl_glsl.h"
#include "gpu/post_hlsl.h"
#include "gpu/fsr_hlsl.h"
#include "gpu/shaders.h"

namespace app {
Settings& Get() {
    static Settings settings;
    return settings;
}
}  // namespace app
// Link fixture for the translator's depth-as-color code (the real mapping from draw.cpp).
namespace gpu {
void DepthLayoutOrder(int layout, float* order) {
    static const float values[4][4] = {{2, 1, 0, 3}, {0, 1, 2, 3}, {1, 2, 3, 0}, {3, 2, 1, 0}};
    for (int i = 0; i < 4; i++) order[i] = values[layout & 3][i];
}
}  // namespace gpu

#define FUNCTIONS(X)                                                                                                   \
    X(GetString) X(GetIntegerv) X(CreateShader) X(ShaderSource) X(CompileShader) X(GetShaderiv) X(GetShaderInfoLog)   \
    X(CreateProgram) X(AttachShader) X(LinkProgram) X(GetProgramiv) X(GetProgramInfoLog) X(UseProgram) X(ClipControl) \
    X(GenTextures) X(BindTexture) X(ActiveTexture) X(TexImage2D) X(TexParameteri) X(GenFramebuffers)                   \
    X(BindFramebuffer) X(FramebufferTexture) X(CheckFramebufferStatus) X(DrawBuffer) X(ReadBuffer) X(ViewportIndexedf) \
    X(ClearBufferfv) X(ClearBufferfi) X(ReadPixels) X(GenBuffers) X(BindBuffer) X(BufferData) X(BindBufferBase)       \
    X(GenVertexArrays) X(BindVertexArray) X(DrawArrays) X(PixelStorei) X(Disable) X(Enable) X(CullFace) X(FrontFace)  \
    X(BlitFramebuffer) X(GetError)
#define DECLARE(name) decltype(&::gl##name) name = nullptr;
namespace gl {
FUNCTIONS(DECLARE)
}

static unsigned g_failures = 0, g_cases = 0;
static uint32_t g_version = 420;
static bool g_flip = false;  // as the renderer draws without glClipControl

// a stage: HLSL to GLSL to a compiled shader, 0 when either fails (reported)
static GLuint Stage(const char* name, const std::string& hlsl, const char* entry, const char* profile,
                    std::vector<gpu::Bind>* binds = nullptr, bool last = true) {
    g_cases++;
    std::string glsl, err;
    std::vector<gpu::Bind> b;
    if (!gpu::HlslToGlsl(hlsl, entry, profile, g_version, glsl, b, err, g_flip && last)) {
        std::printf("FAIL %s: translation: %s\n", name, err.c_str());
        g_failures++;
        return 0;
    }
    const GLenum type = profile[0] == 'v'   ? GL_VERTEX_SHADER
                        : profile[0] == 'g' ? GL_GEOMETRY_SHADER
                        : profile[0] == 'h' ? GL_TESS_CONTROL_SHADER
                        : profile[0] == 'd' ? GL_TESS_EVALUATION_SHADER
                                            : GL_FRAGMENT_SHADER;
    const GLuint shader = gl::CreateShader(type);
    const char* text = glsl.c_str();
    gl::ShaderSource(shader, 1, &text, nullptr);
    gl::CompileShader(shader);
    GLint ok = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]{};
        gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::printf("FAIL %s: GLSL: %s\n%s\n", name, log, glsl.c_str());
        g_failures++;
        return 0;
    }
    if (binds) *binds = b;
    std::printf("PASS %s (%zu bytes of GLSL, %zu samplers)\n", name, glsl.size(), b.size());
    return shader;
}
static GLuint Link(const char* name, std::initializer_list<GLuint> stages) {
    for (GLuint s : stages)
        if (!s) return 0;
    g_cases++;
    const GLuint p = gl::CreateProgram();
    for (GLuint s : stages) gl::AttachShader(p, s);
    gl::LinkProgram(p);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]{};
        gl::GetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::printf("FAIL link %s: %s\n", name, log);
        g_failures++;
        return 0;
    }
    std::printf("PASS link %s\n", name);
    return p;
}

static const char* kBlitVs = R"(
struct Out { float4 p:SV_Position; float2 uv:TEXCOORD0; };
Out vs(uint id:SV_VertexID) { Out o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
)";

static GLuint Target(GLenum internal, GLenum format, GLenum type, int w, int h, const void* pixels = nullptr) {
    GLuint t = 0;
    gl::GenTextures(1, &t);
    gl::BindTexture(GL_TEXTURE_2D, t);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GLint(internal), w, h, 0, format, type, pixels);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}
static GLuint Fbo(GLuint color, GLuint depth) {
    GLuint f = 0;
    gl::GenFramebuffers(1, &f);
    gl::BindFramebuffer(GL_FRAMEBUFFER, f);
    if (color) gl::FramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
    if (depth) gl::FramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0);
    if (!color) {
        gl::DrawBuffer(GL_NONE);
        gl::ReadBuffer(GL_NONE);
    }
    return f;
}

// a picture's top row (row 0 uploaded first) drawn by a full-screen pass lands in the target's row 0, and a pass's
// SV_Position reads that row's y as D3D does
static bool TestOrientation(GLuint vs) {
    const char* ps_hlsl = R"(
Texture2D img : register(t0); SamplerState ps : register(s1);
float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(img.SampleLevel(ps, uv, 0).r, p.y < 1 ? 1 : 0, 0, 1);
}
)";
    std::vector<gpu::Bind> binds;
    const GLuint ps = Stage("orientation pass", ps_hlsl, "main", "ps_5_0", &binds);
    const GLuint p = Link("orientation", {vs, ps});
    if (!p) return false;
    const uint8_t pixels[] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255};  // BGRA: top red
    const GLuint src = Target(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 2, 2, pixels);
    const GLuint dst = Target(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 2, 2);
    gl::BindFramebuffer(GL_FRAMEBUFFER, Fbo(dst, 0));
    gl::ViewportIndexedf(0, 0, 0, 2, 2);
    gl::UseProgram(p);
    for (const auto& b : binds) {
        gl::ActiveTexture(GL_TEXTURE0 + b.unit);
        gl::BindTexture(GL_TEXTURE_2D, src);
    }
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t out[16]{};
    gl::ReadPixels(0, 0, 2, 2, GL_BGRA, GL_UNSIGNED_BYTE, out);
    // row 0: red from the texture (BGRA byte 2), green from SV_Position.y < 1 (byte 1); row 1 neither
    const bool ok = out[2] == 255 && out[1] == 255 && out[8 + 2] == 0 && out[8 + 1] == 0;
    std::printf("%s orientation: row 0 r %u g %u, row 1 r %u g %u\n", ok ? "PASS" : "FAIL", out[2], out[1], out[10],
                out[9]);
    g_cases++;
    if (!ok) g_failures++;
    return ok;
}

// the point sprite geometry shader's corners on the GPU (the HD 4000 corrupted them under D3D11)
static bool TestPointCoordinates() {
    const std::string geometry = gpu::PointHlsl();
    const std::string common = geometry.substr(0, geometry.find("[maxvertexcount"));
    const GLuint vs = Stage("point test vertex",
                            common + "VOut main(uint n:SV_VertexID){VOut o=(VOut)0;o.pos=float4(0,0,0.5,1);o.psize=32;"
                                     "return o;}",
                            "main", "vs_5_0", nullptr, false);
    const GLuint gs = Stage("point sprite geometry", geometry, "main", "gs_5_0");
    const GLuint ps = Stage("point test pixel", common + "float4 main(VOut i):SV_Target{return float4(i.tc0.xy/i.tc0.w,0,1);}",
                            "main", "ps_5_0");
    const GLuint p = Link("point sprite", {vs, gs, ps});
    if (!p) return false;
    gpu::DrawConstants constants{};
    constants.surf[0] = constants.surf[1] = 64;
    constants.pts[3] = 64;
    constants.ptsk[3] = 2;
    constants.ptpx[0] = constants.ptpx[1] = 1;
    GLuint ubo = 0;
    gl::GenBuffers(1, &ubo);
    gl::BindBuffer(GL_UNIFORM_BUFFER, ubo);
    gl::BufferData(GL_UNIFORM_BUFFER, sizeof(constants), &constants, GL_STATIC_DRAW);
    gl::BindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
    std::vector<float> vc(192 * 4, 0.0f);
    GLuint vcb = 0;
    gl::GenBuffers(1, &vcb);
    gl::BindBuffer(GL_UNIFORM_BUFFER, vcb);
    gl::BufferData(GL_UNIFORM_BUFFER, GLsizeiptr(vc.size() * 4), vc.data(), GL_STATIC_DRAW);
    gl::BindBufferBase(GL_UNIFORM_BUFFER, 0, vcb);
    const GLuint dst = Target(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 64, 64);
    gl::BindFramebuffer(GL_FRAMEBUFFER, Fbo(dst, 0));
    const float black[4] = {0, 0, 0, 1};
    gl::ClearBufferfv(GL_COLOR, 0, black);
    gl::ViewportIndexedf(0, 0, 0, 64, 64);
    gl::UseProgram(p);
    gl::DrawArrays(GL_POINTS, 0, 1);
    std::vector<uint8_t> px(64 * 64 * 4);
    gl::ReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    bool passed = true;
    for (int y : {24, 40})
        for (int x : {24, 40}) {
            const uint8_t* pixel = &px[size_t(y * 64 + x) * 4];
            const int er = x == 24 ? 71 : 196, eg = y == 24 ? 71 : 196;
            std::printf("Point UV at %d,%d: %u,%u (expected %d,%d)\n", x, y, pixel[0], pixel[1], er, eg);
            if (std::abs(int(pixel[0]) - er) > 10 || std::abs(int(pixel[1]) - eg) > 10) passed = false;
        }
    std::printf("%s point-sprite coordinates\n", passed ? "PASS" : "FAIL");
    g_cases++;
    if (!passed) g_failures++;
    return passed;
}

// kDepthBytesHlsl from a depth texture and its stencil copy in stencil index mode, as DepthBytes reads them
static bool TestDepthStencilBytes(GLuint vs) {
    std::vector<gpu::Bind> binds;
    const GLuint ps = Stage("depth bytes", gpu::kDepthBytesHlsl, "psmain", "ps_5_0", &binds);
    const GLuint p = Link("depth bytes", {vs, ps});
    if (!p) return false;
    const GLuint depth = Target(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 4);
    const GLuint stencil = Target(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 4);
    gl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    const GLuint mark = Target(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 4);
    const GLuint out = Target(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 4);
    const GLuint depth_fbo = Fbo(0, depth), stencil_fbo = Fbo(0, stencil), out_fbo = Fbo(out, 0);
    GLuint ubo = 0;
    gl::GenBuffers(1, &ubo);
    bool passed = true;
    for (float z : {0.0f, 0.25f, 0.5f, 1.0f})
        for (unsigned s : {0u, 32u, 176u, 255u})
            for (int layout = 0; layout < 4; layout++) {
                gl::BindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
                gl::ClearBufferfi(GL_DEPTH_STENCIL, 0, z, GLint(s));
                gl::BindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
                gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, stencil_fbo);
                gl::BlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
                float constants[32]{};
                gpu::DepthLayoutOrder(layout, constants);
                gl::BindBuffer(GL_UNIFORM_BUFFER, ubo);
                gl::BufferData(GL_UNIFORM_BUFFER, sizeof(constants), constants, GL_DYNAMIC_DRAW);
                gl::BindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
                gl::BindFramebuffer(GL_FRAMEBUFFER, out_fbo);
                gl::ViewportIndexedf(0, 0, 0, 4, 4);
                gl::UseProgram(p);
                for (const auto& b : binds) {
                    gl::ActiveTexture(GL_TEXTURE0 + b.unit);
                    gl::BindTexture(GL_TEXTURE_2D, b.t == 0 ? depth : b.t == 1 ? stencil : mark);
                }
                gl::DrawArrays(GL_TRIANGLES, 0, 3);
                uint8_t pixel[4]{};
                gl::ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                const unsigned iz = z == 0 ? 0 : z == 0.25f ? 0x400000 : z == 0.5f ? 0x800000 : 0xFFFFFF;
                const unsigned bytes[4] = {s, iz & 255, (iz >> 8) & 255, (iz >> 16) & 255};
                bool correct = true;
                for (int i = 0; i < 4; i++) correct &= std::abs(int(pixel[i]) - int(bytes[int(constants[i])])) <= 1;
                if (!correct)
                    std::printf("FAIL depth/stencil bytes layout %d z %g stencil %u: %u,%u,%u,%u\n", layout, z, s,
                                pixel[0], pixel[1], pixel[2], pixel[3]);
                passed &= correct;
            }
    std::printf("%s depth/stencil byte layouts\n", passed ? "PASS" : "FAIL");
    g_cases++;
    if (!passed) g_failures++;
    return passed;
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* window = SDL_CreateWindow("opengl-shader-smoke", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        std::printf("no GL 4.0 context: %s\n", SDL_GetError());
        return 1;
    }
#define LOAD(name) gl::name = reinterpret_cast<decltype(gl::name)>(SDL_GL_GetProcAddress("gl" #name));
    FUNCTIONS(LOAD)
    GLint major = 0, minor = 0;
    gl::GetIntegerv(GL_MAJOR_VERSION, &major);
    gl::GetIntegerv(GL_MINOR_VERSION, &minor);
    const char* flip = std::getenv("SMOKE_NO_CLIP_CONTROL");
    g_flip = flip && *flip == '1';
    g_version = major > 4 || (major == 4 && minor >= 2) ? 420 : minor == 1 ? 410 : 400;
    std::printf("%s, %s; GLSL %u\n", reinterpret_cast<const char*>(gl::GetString(GL_RENDERER)),
                reinterpret_cast<const char*>(gl::GetString(GL_VERSION)), g_version);
    if (!g_flip && !gl::ClipControl) {
        std::printf("FAIL no glClipControl (SMOKE_NO_CLIP_CONTROL=1 tests without)\n");
        return 1;
    }
    if (!g_flip) gl::ClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
    GLuint vao = 0;
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);
    gpu::GlslInit();

    const GLuint vs = Stage("blit vertex", kBlitVs, "vs", "vs_5_0");
    TestOrientation(vs);
    TestPointCoordinates();
    TestDepthStencilBytes(vs);

    // the draws' shaders
    const GLuint fixed_vs = Stage("fixed transform", gpu::FixedVertexHlsl(0x1F, false), "main", "vs_5_0");
    Stage("pretransformed", gpu::FixedVertexHlsl(0x1F, true), "main", "vs_5_0");
    const GLuint program_vs = Stage("register program", gpu::VertexProgramHlsl({}, 0xFFFF), "main", "vs_5_0");
    for (int kind = 0; kind <= 3; kind++) {
        gpu::PixelKey key{};
        key.ff[0][0] = 4;
        key.ff[0][1] = 2;
        key.ff[0][2] = 0;
        key.ff[0][4] = 2;
        key.ff[0][5] = 2;
        key.tex[0].kind = uint8_t(kind);
        char name[48];
        std::snprintf(name, sizeof(name), "fixed pixel texture kind %d", kind);
        const GLuint ps = Stage(name, gpu::PixelHlsl(key), "main", "ps_5_0");
        std::snprintf(name, sizeof(name), "fixed draw, texture kind %d", kind);
        Link(name, {fixed_vs, ps});
    }
    gpu::PixelKey key{};
    key.programmable = 1;
    key.count = 1;
    key.tex_modes = 1;
    key.tex[0].kind = gpu::kTex2D;
    const GLuint combiners = Stage("register combiners", gpu::PixelHlsl(key), "main", "ps_5_0");
    Link("program draw", {program_vs, combiners});
    key.tex[0].shadow = 1;
    const GLuint shadow = Stage("depth comparison texture", gpu::PixelHlsl(key), "main", "ps_5_0");
    Link("shadow draw", {program_vs, shadow});

    // the full-screen passes, each with the blit vertex shader
    struct Pass {
        const char* name;
        std::string source;
        const char* entry;
    };
    const Pass passes[] = {
        {"depth bytes multisampled", gpu::GlDepthBytesMultisampled(), "psmain"},
        {"outline", gpu::kOutlineHlsl, "outline"},
        {"grade", gpu::kPostHlsl, "grade"},
        {"occlusion", gpu::kOcclusionHlsl, "occlusion"},
        {"occlusion blur x", gpu::kOcclusionHlsl, "blurx"},
        {"occlusion blur y", gpu::kOcclusionHlsl, "blury"},
        {"occlusion apply", gpu::kOcclusionHlsl, "apply"},
        {"motion blur", gpu::kMotionBlurHlsl, "blur"},
        {"SMAA edges", gpu::SmaaHlsl(0), "edges"},
        {"SMAA weights", gpu::SmaaHlsl(1), "weights"},
        {"SMAA blend", gpu::SmaaHlsl(2), "blend"},
        {"FSR EASU", gpu::kFsrHlsl, "easu"},
        {"FSR RCAS", std::string("#define FSR_PRESENTATION_FADE\n") + gpu::kFsrHlsl, "rcas"},
    };
    for (const Pass& p : passes) {
        const GLuint ps = Stage(p.name, p.source, p.entry, "ps_5_0");
        Link(p.name, {vs, ps});
    }
    std::printf("%u cases, %u failures\n", g_cases, g_failures);
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return g_failures ? 1 : 0;
}
