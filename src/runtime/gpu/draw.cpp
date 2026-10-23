// What the renderers share past their APIs, see draw.h
#include "gpu/draw.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "gpu/dxgi_formats.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <utility>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/memory.h"

namespace gpu {

bool CaptureOn() { return !diag::CaptureDir().empty(); }

int DepthByteLayout(uint32_t format) {
    switch (format) {
        case 0x3A: case 0x3F: return 1;
        case 0x3B: case 0x40: return 2;
        case 0x3C: case 0x41: return 3;
        default: return 0;
    }
}

void DepthLayoutOrder(int layout, float order[4]) {
    static const float o[4][4] = {{2, 1, 0, 3}, {0, 1, 2, 3}, {1, 2, 3, 0}, {3, 2, 1, 0}};  // ARGB ABGR BGRA RGBA
    std::copy(o[layout & 3], o[layout & 3] + 4, order);
}

void DepthByteOrder(uint32_t format, float order[4]) { DepthLayoutOrder(DepthByteLayout(format), order); }

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

uint64_t SourceHash(const std::string& src, const char* entry, const char* profile) {
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    return Hash(src.data(), src.size(), entry_seed);
}

PipelineEntry MakePipelineEntry(const PsoKey& k, const ShaderChoice& sh) {
    PipelineEntry e;
    std::memset(&e, 0, sizeof(e));  // padding included: the bytes are hashed and stored
    e.vs_id = sh.vid;
    e.ps_id = sh.pid;
    std::memcpy(&e.k, &k, sizeof(k));
    e.k.vs = e.k.ps = 0;
    return e;
}

void LowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

void ClassifyShaders(const State& s, const Assembled& a, uint32_t count, ShaderChoice& sh) {
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

namespace {

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

}  // namespace

uint64_t VertexKey(const State& s, const Assembled& a, const ShaderChoice& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    if (sh.shading) vkey = Hash(&sh.shading, sizeof(sh.shading), vkey);
    return vkey;
}

shader_record::VertexEntry MakeVertexEntry(const State& s, const Assembled& a, const ShaderChoice& sh) {
    shader_record::VertexEntry v;
    v.program = sh.prog ? s.vs->hash : 0;
    v.decode = a.decode;
    if (!sh.prog) v.fixed = sh.fk;
    v.shading = uint8_t(sh.shading);
    v.regs = a.regs;
    v.pretransformed = !sh.prog && a.pretransformed;
    v.quad = sh.quad;
    return v;
}

const std::vector<uint32_t>& DrawProgram(const State& s, const ShaderChoice& sh) {
    static const std::vector<uint32_t> none;
    return sh.prog ? s.vs->program : none;
}

uint32_t DrawShading(const State& s, const ShaderChoice& sh) {
    if (!sh.prog || g_program_shading.empty()) return 0;
    // translating a program to match it is slow: once per program
    static std::unordered_map<uint64_t, uint64_t> matched;
    auto [it, fresh] = matched.try_emplace(s.vs->hash, 0);
    if (fresh) it->second = MatchProgramShading(s.vs->program);
    // the first entry the program matches that is on and, with textures, takes the draw's
    for (uint64_t m = it->second; m; m &= m - 1) {
        const ProgramShading& e = g_program_shading[size_t(std::countr_zero(m))];
        if (!e.on || !*e.on) continue;
        if (const uint32_t* t = e.textures) {
            TextureDesc d;
            uint32_t header = s.textures[size_t(e.texture_stage)];
            if (!header || !Describe(header, d)) continue;
            while (*t && *t != d.data) t++;
            if (!*t) continue;
        }
        return uint32_t(std::countr_zero(m)) + 1;
    }
    return 0;
}

void WatchDraw(const State& s, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (g_program_watch.empty() || !((s.vs_handle & 1) && s.vs && !s.vs->program.empty())) return;
    EnsureProgramHash(s);
    struct Match {
        uint32_t watch = 0;
        std::string hlsl;
    };
    static std::unordered_map<uint64_t, Match> matched;  // as DrawShading's
    auto [it, fresh] = matched.try_emplace(s.vs->hash);
    if (fresh) {
        it->second.watch = MatchProgramWatch(s.vs->program);
        if (it->second.watch) it->second.hlsl = VertexProgramHlsl(s.vs->program, 0xFFFF);
    }
    if (!it->second.watch) return;
    const ProgramWatch& w = g_program_watch[it->second.watch - 1];
    static std::vector<float> in[16];  // kept: a watched draw allocating its own was a heap call per draw
    WatchedDraw d;
    d.vertices = std::min(count, w.vertices);
    for (int r = 0; r < 16; r++) {
        if (!((w.regs >> r) & 1)) continue;
        in[r].resize(size_t(d.vertices) * 4);
        auto out = reinterpret_cast<float (*)[4]>(in[r].data());
        bool read = true;
        for (uint32_t k = 0; read && k < (w.spread ? d.vertices : 1); k++) {
            uint32_t at = w.spread ? uint32_t(uint64_t(k) * count / d.vertices) : 0;
            read = ReadInput(s, r, start + at, indices ? indices + size_t(at) * 2 : nullptr,
                             w.spread ? 1 : d.vertices, out + k);
        }
        if (read) d.v[r] = out;
    }
    d.tex_modes = s.ps ? s.rs[kRsPsTextureModes] : 0;
    d.c = s.vsc.data();
    d.program = s.vs->hash;
    d.hlsl = &it->second.hlsl;
    w.seen(d);
}

bool TessellatedDraw(const State& s, uint32_t prim) {
    static const bool any = std::any_of(g_program_shading.begin(), g_program_shading.end(),
                                        [](const ProgramShading& p) { return *p.tess; });
    if (!any || prim < 5) return false;  // triangles, strips, fans, quads, polygons
    ShaderChoice sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    if (!sh.prog) return false;
    EnsureProgramHash(s);
    uint32_t i = DrawShading(s, sh);
    return i && *g_program_shading[i - 1].tess;
}

namespace {

// per g_program_shading entry: the largest top of each run of its draws (a draw whose top is 0 starts the next) this
// frame and last frame, by the runs' order in the frame
struct ShadingTop {
    std::vector<float> frame, last;
    float x = 0;          // the latest draw's top
    float before = -1;    // the top of the draw before it in its run, -1 when it began one
};
std::vector<ShadingTop> g_shading_tops;
float g_shading_clock = 0;  // xc.y: seconds from the first frame's end to this frame's start
float g_shading_clock_before = 0;  // the frame before's
std::vector<ShadingTop> g_replay_tops;
float g_replay_clock = 0;

}  // namespace

void FillShadingConstants(const State& s, uint32_t shading, DrawConstants& c) {
    const ProgramShading& ps = g_program_shading[shading - 1];
    c.xc[1] = g_shading_clock;
    if (ps.fill) ps.fill(c.xs);
    int top = ps.top;
    if (top < 0 || top >= int(s.vsc.size())) return;
    if (g_shading_tops.size() < g_program_shading.size()) g_shading_tops.resize(g_program_shading.size());
    ShadingTop& t = g_shading_tops[shading - 1];
    float x = s.vsc[size_t(top)][0];
    bool begins = t.frame.empty() || x <= 0;
    t.before = begins ? -1 : t.x;
    t.x = x;
    if (begins) t.frame.push_back(0);
    size_t run = t.frame.size() - 1;
    t.frame[run] = std::max(t.frame[run], x);
    float most = std::max(t.frame[run], run < t.last.size() ? t.last[run] : 0.0f);
    c.xc[0] = most > 0 ? 1 / most : 0;
}

float ShadingBefore(uint32_t shading) {
    return shading - 1 < g_shading_tops.size() ? g_shading_tops[shading - 1].before : -1;
}

void EndShadingFrame() {
    static const auto start = std::chrono::steady_clock::now();
    g_shading_clock_before = g_shading_clock;
    g_shading_clock = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    for (const ProgramWatch& w : g_program_watch)
        if (w.frame) w.frame();
    for (ShadingTop& t : g_shading_tops) {
        t.last.swap(t.frame);
        t.frame.clear();
    }
}

void ShadingReplay(bool begin, float t) {
    if (!begin) {
        g_shading_tops.swap(g_replay_tops);
        g_shading_clock = g_replay_clock;
        return;
    }
    g_replay_tops = g_shading_tops;
    g_replay_clock = g_shading_clock;
    for (ShadingTop& top : g_shading_tops) {
        top.frame.clear();
        top.x = 0;
        top.before = -1;
    }
    g_shading_clock = g_shading_clock_before + (g_shading_clock - g_shading_clock_before) * t;
}

std::string VertexHlsl(const shader_record::VertexEntry& v, const std::vector<uint32_t>& program) {
    std::string src = v.program ? VertexProgramHlsl(program, v.regs, v.decode, v.shading)
                                : FixedVertexHlsl(v.regs, v.pretransformed, v.decode, v.fixed);
    return v.quad ? QuadHlsl(std::move(src)) : src;
}

uint32_t SampledStages(const State& s) {
    uint32_t mask = 0;
    if (s.ps) {
        uint32_t modes = s.rs[kRsPsTextureModes];
        for (int i = 0; i < 4; i++) {
            uint32_t m = (modes >> (i * 5)) & 0x1F;
            if (m && m != 4 && m != 5 && m != 0xA && m != 0x11) mask |= 1u << i;  // none, pass through, clip plane, dots
        }
        return mask;
    }
    for (int i = 0; i < 4 && s.tss[size_t(i)][kTssColorOp] > 1; i++) mask |= 1u << i;  // D3DTOP_DISABLE
    return mask;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to RGBA bits, D3D12's and Vulkan's alike
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyDepthStencil(const State& s, const Assembled& a, float ds_scale, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if ((a.topology == Topology::Triangles || a.topology == Topology::Patches) && SimpleState(s, kMSolidOffsetEnable)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * ds_scale;
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;  // not patches: their winding is the tessellator's
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

namespace {

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

}  // namespace

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (ChipFogConstants(s, c.fog)) return;
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

namespace {

constexpr float kMaxPointSize = 64;  // the console's largest point

}  // namespace

// a vertex program's oPts is the size; the fixed function takes D3DRS_POINTSIZE, scaled when D3DRS_POINTSCALEENABLE
// is
void FillPointConstants(const State& s, bool vertex_program, bool stage0, DrawConstants& c) {
    c.pts[3] = kMaxPointSize;
    c.ptsk[3] = !s.rs[kRsPointSpriteEnable] ? 0.0f : stage0 ? 2.0f : 1.0f;
    if (vertex_program) return;
    c.pts[0] = Bits(s.rs[kRsPointSize]);
    if (s.rs[kRsPointScaleEnable]) {
        c.pts[1] = float(s.viewport.h);
        c.ptsk[0] = Bits(s.rs[kRsPointScaleA]);
        c.ptsk[1] = Bits(s.rs[kRsPointScaleB]);
        c.ptsk[2] = Bits(s.rs[kRsPointScaleC]);
    }
    // the limits as set, when they make sense
    float lo = Bits(s.rs[kRsPointSizeMin]), hi = Bits(s.rs[kRsPointSizeMax]);
    if (hi > 0 && hi < kMaxPointSize) c.pts[3] = hi;
    if (lo > 0 && lo <= c.pts[3]) c.pts[2] = lo;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(TextureFactor(s), c.tfactor);
}

namespace {

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void FillTexelConstants(int i, const TextureDesc& d, bool rt, bool exact, float k, float wide, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 and Vulkan half a pixel on: targets are read half a
    // texel back, but for those the draw reads exactly where they lie
    if (!rt) return;
    if (!exact) {
        c.texsize[i][2] = -0.5f / (float(d.width) * k);
        c.texsize[i][3] = -0.5f / (float(d.height) * k);
    }
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (wide > 0) {
        float n = std::ceil(kGlowBoxTexels * wide * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

}  // namespace

void FillTextureConstants(const State& s, const bool rt[4], const bool exact[4], const float k[4],
                          const float wide[4], DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d))
            FillTexelConstants(i, d, rt[i], exact[i], k[i], wide[i], c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

namespace {

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

}  // namespace

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

bool ScreenSpace(const State& s, const Assembled& a, int pos_w) {
    auto flat = [](float x, float y, float z) { return std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f && std::fabs(z) < 1e-6f; };
    if (a.pretransformed) return true;
    // the fixed function's w is the projection's last column, world and view being affine
    if (!((s.vs_handle & 1) && s.vs && !s.vs->program.empty())) {
        const float* m = s.transforms[kTsProjection].data();
        return flat(m[3], m[7], m[11]);
    }
    if (pos_w == kPosWInput) return true;
    if (pos_w < 0 || size_t(pos_w) >= s.vsc.size()) return false;
    const auto& c = s.vsc[size_t(pos_w)];
    return flat(c[0], c[1], c[2]);
}

namespace {

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

}  // namespace

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

namespace {

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

}  // namespace

void CaptureInputs(FILE* f, const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(f, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(16, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(f, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(f, " %g", a.vertices[v * n * 4 + k]);
        fprintf(f, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(f, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(f, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(f, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

void CaptureConstantChanges(FILE* f, const State& s, std::array<std::array<float, 4>, 192>& prev) {
    fprintf(f, "\n     cset");
    for (int r = 0; r < kVsConstants;) {
        if (s.vsc[size_t(r)] == prev[size_t(r)]) {
            r++;
            continue;
        }
        int e = r;
        while (e + 1 < kVsConstants && s.vsc[size_t(e + 1)] != prev[size_t(e + 1)]) e++;
        if (e == r)
            fprintf(f, " c%d", r - kVsConstantBias);
        else
            fprintf(f, " c%d-c%d", r - kVsConstantBias, e - kVsConstantBias);
        r = e + 1;
    }
    prev = s.vsc.v;
}

void CaptureDrawLine(FILE* f, uint32_t index, uint32_t ds_data, const State& s, uint32_t prim, uint32_t count,
                     const PsoKey& k, const DrawConstants& c) {
    fprintf(f,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            index, s.rt.data, ds_data, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend, k.src, k.dst,
            k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1], c.pc[0][2],
            c.pc[0][3]);
    // stencil as drawn, then the chip's own enable and fail op (the renderer takes those two from the render states)
    fprintf(f, " sten %u func %X ref %02X mask %02X wmask %02X ops %X %X %X chip %X %X", k.sten,
            SimpleState(s, kMStencilFunc), SimpleState(s, kMStencilRef) & 0xFF, SimpleState(s, kMStencilMask) & 0xFF,
            SimpleState(s, kMStencilWriteMask) & 0xFF, s.rs[kRsStencilFail], SimpleState(s, kMStencilZFail),
            SimpleState(s, kMStencilPass), SimpleState(s, kMStencilTestEnable), SimpleState(s, kMStencilFail));
}

FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

namespace {

// BMP file and info header: byte offsets of the fields WriteBmp writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

}  // namespace

void WriteBmp(const std::filesystem::path& p, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    FILE* f = OpenFile(p, "wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, w);
    put(kBmpHeight, uint32_t(-int32_t(h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

void CountedSkip(const State& s, const char* why) {
    static const bool trace = std::getenv("XBE_TRACE_REPORTS") != nullptr;
    static std::atomic<int> n{0};
    if (trace && s.simple[kMCountEnable >> 2] && n++ < 64) LOGR("gpu: counted draw skipped (%s)", why);
}

// The console gives only stage 3 sprite coordinates. A fixed function point sprite whose stages reach stage 3 samples
// the sprite there; one whose stages end before it runs them on stage 3's texture in stage 0, sampled as stage 3 sets
// it. Conker draws one blood splash both ways, by the stage states earlier draws left: stages 0 to 3 with the sprite
// on stage 3, or stage 0 alone, empty, with the sprite on stage 3, which read as white squares.
bool FixedSprite(const State& s, uint32_t prim) {
    if (prim != kPrimPoints || s.ps || !s.rs[kRsPointSpriteEnable] || !s.textures[3]) return false;
    uint32_t stages = 0;
    while (stages < 4 && s.tss[stages][kTssColorOp] > 1) stages++;  // D3DTOP_DISABLE ends them
    return stages >= 1 && stages < 4;
}

// s as FixedSprite draws it: stage 3's texture and sampling on stage 0, which takes the sprite's coordinates
void SpriteStage(State& s) {
    s.textures[0] = std::exchange(s.textures[3], 0);
    s.palettes[0] = std::exchange(s.palettes[3], 0);
    for (uint32_t i : {kTssAddressU, kTssAddressV, kTssAddressW, kTssMagFilter, kTssMinFilter, kTssMipFilter,
                       kTssMipLodBias, kTssMaxMipLevel, kTssMaxAnisotropy, kTssColorKeyOp, kTssColorSign,
                       kTssAlphaKill, kTssBorderColor, kTssColorKeyColor})
        s.tss.set(0, i, s.tss[3][i]);
}

namespace {

constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

}  // namespace

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: draw steps per frame: setup %.2f vs %.2f ps %.2f pso %.2f consts %.2f record %.2f | %.0f of %.0f "
             "draws in the state before, drawn as %.0f runs (%.0f reused)",
             p.d_setup / n, p.d_vs / n, p.d_ps / n, p.d_pso / n, p.d_consts / n, p.d_record / n, p.fast / n,
             p.draws / n, p.runs / n, p.reused / n);
        {
            uint64_t cyc[5], all = 0;
            for (int i = 0; i < 5; i++) all += cyc[i] = g_perf_rec_cycles[i].exchange(0);
            auto pct = [&](int i) { return all ? 100.0 * double(cyc[i]) / double(all) : 0.0; };
            LOGI("perf: title thread recording draws: state %.0f%% headers %.0f%% command %.0f%% uses %.0f%% "
                 "handover %.0f%%",
                 pct(0), pct(1), pct(2), pct(3), pct(4));
        }
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

}  // namespace gpu
