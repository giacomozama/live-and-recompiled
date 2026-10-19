#pragma once
// What the renderers share past their APIs: the keys a draw's shaders and pipeline are made from, its constants, the
// title's surfaces as guest memory holds them, the frame captures' text, and the perf diagnostic
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gpu/shader_record.h"
#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

namespace gpu {

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
inline double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};
inline Perf g_perf;
struct PerfScope {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    ~PerfScope() { acc += PerfMs(t); }
};
// each present: the second's lines once one has passed
void LogPerf();

// F3 captures only with diag::CaptureDir set
bool CaptureOn();

// the memory byte R G B A take in a 32 bit colour format, as texture.cpp decodes it
void DepthByteOrder(uint32_t format, float order[4]);
float Bits(uint32_t v);
void Color(uint32_t argb, float* o);
// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as 0 to 7, the order D3D12's and Vulkan's comparisons both take
inline uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
inline uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
inline uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;
uint64_t MemSig(uint32_t data, uint32_t total);

// A renderer's target as guest memory sees it: data, bytes, gw, gh, epoch, sig, drawn and read

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
template <typename T>
bool Changed(const T& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
template <typename T>
bool LatchCpuWrite(T& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
template <typename T>
std::vector<uint8_t> LatchCpuChanges(T& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;    // DXGI format of the colour target, whichever API draws it
    uint32_t samples;  // of the bound targets
};

// a pipeline in shaders.rec: its shaders by record id, the key with vs and ps zero
struct PipelineEntry {
    uint64_t vs_id;
    uint64_t ps_id;
    PsoKey k;
};
// the record layouts shaders.rec was written with; bump kRecordRevision when what a record makes changes meaning
constexpr uint64_t kRecordRevision = 1;
constexpr uint64_t kRecordLayout = kRecordRevision << 48 | uint64_t(sizeof(PixelKey)) << 32 |
                                   uint64_t(sizeof(PipelineEntry)) << 16 | sizeof(shader_record::VertexEntry);
// program: the words v.program names, unused without one
std::string VertexHlsl(const shader_record::VertexEntry& v, const std::vector<uint32_t>& program);

// a draw's shaders: what they are made from, their cache keys and the hashes the compiles returned
struct ShaderChoice {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    uint64_t vid = 0;  // shader_record ids
    uint64_t pid = 0;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// a renderer's targets by key: a surface's colour or depth target
inline uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a compile's key for the source, entry point and profile: one source can hold several entry points
uint64_t SourceHash(const std::string& src, const char* entry, const char* profile);

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// the pipeline as shaders.rec keeps it
PipelineEntry MakePipelineEntry(const PsoKey& k, const ShaderChoice& sh);

// a pool thread making shaders ahead, below the title's threads
void LowerThreadPriority();

// fn(i) for each i below n on a few threads under the title's, until stop
template <typename Fn>
void ParallelFor(size_t n, const std::atomic<bool>& stop, Fn fn) {
    unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
    std::atomic<size_t> next{0};
    auto run = [&] {
        LowerThreadPriority();
        for (size_t i; !stop && (i = next++) < n;) fn(i);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
}

TexKind TextureKind(const TextureDesc& d);

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};
// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette);

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

inline uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

// what kind of shaders the draw takes, into sh
void ClassifyShaders(const State& s, const Assembled& a, uint32_t count, ShaderChoice& sh);
// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s);
uint64_t VertexKey(const State& s, const Assembled& a, const ShaderChoice& sh);
// what the draw's vertex shader is made from, as shaders.rec keeps it
shader_record::VertexEntry MakeVertexEntry(const State& s, const Assembled& a, const ShaderChoice& sh);
// the draw's vertex program, empty for the fixed function pipeline
const std::vector<uint32_t>& DrawProgram(const State& s, const ShaderChoice& sh);

// the stages the pixel shader reads: a combiner's by their texture modes (as MakePixelKey takes them), the fixed
// function's up to the first disabled one
uint32_t SampledStages(const State& s);

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

void KeyBlend(const State& s, PsoKey& k);
constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0
// ds_scale: host pixels per console pixel of the bound depth target
void KeyDepthStencil(const State& s, const Assembled& a, float ds_scale, PsoKey& k);
void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k);

// alpha test off: compare always, in CmpIndex terms like the enabled test
constexpr float kAlphaTestAlways = 7.0f;

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c);
void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c);
// stage0: the sprite's coordinates go to stage 0 (FixedSprite), else to stage 3
void FillPointConstants(const State& s, bool vertex_program, bool stage0, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
// per stage: rt it samples a render target, exact one read where it lies (no half texel back), k Extension::Sample's,
// wide host texels per console texel of a target read into a smaller one (0 otherwise)
void FillTextureConstants(const State& s, const bool rt[4], const bool exact[4], const float k[4],
                          const float wide[4], DrawConstants& c);
void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

// frame captures: a draw's line without its textures, and a small draw's inputs and constants
void CaptureDrawLine(FILE* f, uint32_t index, uint32_t ds_data, const State& s, uint32_t prim, uint32_t count,
                     const PsoKey& k, const DrawConstants& c);
void CaptureInputs(FILE* f, const State& s, const Assembled& a, const DrawConstants& c);
// the vertex constants that differ from prev (the last captured draw's), which it then takes
void CaptureConstantChanges(FILE* f, const State& s, std::array<std::array<float, 4>, 192>& prev);
FILE* OpenFile(const std::filesystem::path& p, const char* mode);
// a top down 32 bit BMP of w x h BGRA pixels
void WriteBmp(const std::filesystem::path& p, const std::vector<uint8_t>& px, uint32_t w, uint32_t h);

// A small draw whose every float2 input lies in 0..1, see draw.cpp
bool UnitTexcoords(const Assembled& a);

// XBE_TRACE_REPORTS: a draw inside a visibility test that never reaches the GPU leaves the test short of its pixels
void CountedSkip(const State& s, const char* why);

constexpr uint32_t kPrimPoints = 1;  // D3DPT_POINTLIST

// a fixed function point sprite whose stages end before stage 3 (draw.cpp), and its state as the console draws it
bool FixedSprite(const State& s, uint32_t prim);
void SpriteStage(State& s);

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i);
GuestPoint ReadGuestPoint(uint32_t points, uint32_t i);

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i);

// a guest coordinate read as unsigned, times the host scale
inline uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

}  // namespace gpu
