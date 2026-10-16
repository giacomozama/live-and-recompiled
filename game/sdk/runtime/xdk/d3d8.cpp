// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_DrawIndexedVerticesUP
#define SYM_D3DDevice_DrawIndexedVerticesUP FFFFFE04
#define HAS_D3DDevice_DrawIndexedVerticesUP 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet
uint32_t g_interval = 1;   // D3DPRESENT_INTERVAL of the device's present parameters
uint32_t g_ps_handle = 0;  // the pixel shader SetPixelShader last bound, 0 for the fixed function combiner
// the library's device word at +0x788: the last pixel shader SetPixelShader bound brought its own final combiner
// (its definition's final inputs are not zero), so the fog flush leaves the final combiner alone
bool g_ps_final = false;
constexpr uint32_t kRawCombiner = 0xFFFFFFFF;  // handle of the object standing for a combiner written as raw methods

// The transform engine's program memory (xemu: pg->program_data, 136 instructions of 4 dwords). A title that
// inlines the library uploads its vertex programs straight to the chip with NV097_SET_TRANSFORM_PROGRAM at the
// slot SET_TRANSFORM_PROGRAM_LOAD selects, and starts one with SET_TRANSFORM_PROGRAM_START; it runs from there to
// the instruction marked final. SetVertexShader's library body does that same upload for a D3D shader, so
// whichever wrote last is what the chip runs: SetVertexShader clears g_chip_program, a raw upload or start sets it.
constexpr uint32_t kProgramSlots = 136;
uint32_t g_prog[kProgramSlots][4] = {};
uint32_t g_prog_load = 0, g_prog_start = 0;
bool g_chip_program = false;
bool g_chip_dirty = true;  // g_prog or g_prog_start changed since the program was last gathered

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// the transform-constant load pointer (NV097_SET_TRANSFORM_CONSTANT_LOAD): the register the next constant data loads,
// advanced by every register loaded, the title's raw pushes and the library's own constant setters alike
uint32_t g_vs_const_reg = 0;

// SetVertexShaderConstant: the library pushes LOAD = register then the data, after whatever the title pushed raw
// before the call, and leaves the load pointer past the last register
void LoadConstants(uint32_t reg, uint32_t src, uint32_t count) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    for (uint32_t i = 0; i < count && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    g_vs_const_reg = reg + count;
}

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

// NV097_SET_SPECULAR_ENABLE as the title wrote it raw, which holds until the library's lighting flush pushes the D3D
// state again (dirty 0x1000). xemu, as the chip, zeroes a vertex program's oD1 while it is off.
bool g_spec_raw = false;
uint32_t g_spec_value = 0;

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    // a D3D fog state only marks the fog dirty: the fog unit changes when the library's flush pushes it (ApplyFlush)
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    if (g_spec_raw) g_state.rs[gpu::kRsSpecularEnable] = g_spec_value;
    // the whole [4][32] table is authoritative for a title that inlines the D3D8 library (its state never
    // reaches an API HLE), so read all of it. A title that does call the API has its dedicated SetTextureState_*
    // HLEs write g_state directly, and those do not write the table, so leave their fields alone.
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssCount; i++) {
            if (HAS_D3DDevice_SetTextureState_BumpEnv && i >= gpu::kTssBumpEnvMat00 && i <= gpu::kTssBumpEnvLOffset) continue;
            if (HAS_D3DDevice_SetTextureState_BorderColor && i == gpu::kTssBorderColor) continue;
            if (HAS_D3DDevice_SetTextureState_ColorKeyColor && i == gpu::kTssColorKeyColor) continue;
            g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
        }
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    if (rs == gpu::kRsTextureFactor) g_state.chip_tfactor = 0;
    W32(kRenderState + rs * 4, v);
}

// NV097_SET_CULL_FACE_ENABLE / _CULL_FACE / _FRONT_FACE, which the library's CullMode and FrontFace push
constexpr uint32_t kMCullEnable = 0x308, kMCullFace = 0x39C, kMFrontFace = 0x3A0;
constexpr uint32_t kGlFront = 0x404, kGlBack = 0x405, kCw = 0x900;

// the renderers cull by the render states (the winding culled, D3DCULL), so a raw write lands there as the chip
// reads it: the front face's winding when culling GL_FRONT, the other one when culling GL_BACK
void ChipCull() {
    uint32_t front = g_state.simple[kMFrontFace >> 2] ? g_state.simple[kMFrontFace >> 2] : kCw;
    uint32_t face = g_state.simple[kMCullFace >> 2] ? g_state.simple[kMCullFace >> 2] : kGlBack;
    uint32_t culled = face == kGlFront ? front : front ^ 1;  // 0x900 CW, 0x901 CCW
    g_state.rs[gpu::kRsFrontFace] = front;
    g_state.rs[gpu::kRsCullMode] = g_state.simple[kMCullEnable >> 2] ? culled : 0;
}

// what the library's CullMode pushes, so a later raw write to one of the three combines with it
void CullPush(uint32_t cull) {
    uint32_t front = g_state.rs[gpu::kRsFrontFace] ? g_state.rs[gpu::kRsFrontFace] : kCw;
    g_state.simple[kMFrontFace >> 2] = front;
    g_state.simple[kMCullEnable >> 2] = cull != 0;
    if (cull) g_state.simple[kMCullFace >> 2] = cull == front ? kGlFront : kGlBack;
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
    // the stencil ones at least: the chip resets them to 0, and 0 is GL_ZERO. Conker never sets the depth fail op, so
    // its sky, drawn last and failing depth behind the castle, zeroed the stencil that marks the window for the glow.
    constexpr uint32_t kKeep = 0x1E00, kAlways = 0x207;
    g_state.simple[gpu::kMStencilFail >> 2] = kKeep;
    g_state.simple[gpu::kMStencilZFail >> 2] = kKeep;
    g_state.simple[gpu::kMStencilPass >> 2] = kKeep;
    g_state.simple[gpu::kMStencilFunc >> 2] = kAlways;
    g_state.simple[gpu::kMStencilMask >> 2] = 0xFF;
    g_state.simple[gpu::kMStencilWriteMask >> 2] = 0xFF;
    g_state.rs[gpu::kRsStencilFail] = kKeep;
#endif
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

// The library's own device init (0x53E5D0), which this runtime skips because it brings the NV2A hardware up,
// allocates a 0x60-byte block and points three device fields at it: the base at +0x30, base + 0x20 at +0x2490
// and base + 0x40 at +0x248c. Inlined library code the engine calls directly still reads [device+0x30] and
// dereferences it (the UI primitive builder at 0x53AE30 does), so a skipped init leaves a null pointer and
// traps the first time that path runs. Back the block with zeroed guest memory, like the init's allocator.
uint32_t DeviceRecords() {
    static uint32_t records = 0;
    if (!records) {
        records = mem::AllocSystem(0x60);
        std::memset(HostPtr(records), 0, 0x60);
    }
    return records;
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

namespace {
constexpr uint32_t kDirtyFog = 0x2000, kDirtyCombiners = 0x800, kDirtyLights = 0x1000;  // D3D_DirtyFlags bits of this library
constexpr uint32_t kDirtyVertexFormat = 0x10, kDirtyStreams = 0x40, kDirtyNoVertexState = 0x40000000;

// NV097_SET_COMBINER_SPECULAR_FOG_CW0 / _CW1 the library's fog flush (0x5421D0) pushes when no pixel shader with
// its own final combiner is bound: fog on, A fog alpha, B R0 (+ specular), C fog colour; fog off, D R0 (+ specular)
constexpr uint32_t kFinalFogged = 0x130C0300, kFinalPlain = 0x0C, kFinalSpecular = 0x20000, kFinalEfgh = 0x1C80;

// The library's fog flush (0x5421D0): NV097_SET_FOG_ENABLE, and when on _MODE and _PARAMS, worked out from the D3D
// fog states as they are now. The chip keeps them until the next push, so a title that changes the states and then
// draws with its own pushbuffer, with no flush between, still draws with these.
void PushFog() {
    auto rs = [](uint32_t i) { return R32(kRenderState + i * 4); };
    auto rsf = [&](uint32_t i) {
        uint32_t v = rs(i);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    };
    g_state.chip_fog = 1;
    g_state.fog_enable = rs(gpu::kRsFogEnable) != 0;
    if (!g_state.fog_enable) return;
    constexpr float kLinearEqual = 8192.0f, kExpScale = -0.0901680737733841f, kExp2Scale = -0.21233002841472626f;
    switch (rs(gpu::kRsFogTableMode)) {
        case 0:  // D3DFOG_NONE: the vertex's own fog, linear 1:1
            g_state.fog_mode = 0x2601;
            g_state.fog_param[0] = 1.0f;
            g_state.fog_param[1] = 1.0f;
            break;
        case 3: {  // D3DFOG_LINEAR: (end - d) / (end - start)
            float start = rsf(gpu::kRsFogStart), end = rsf(gpu::kRsFogEnd);
            float k = start == end ? kLinearEqual : 1.0f / (end - start);
            g_state.fog_mode = 0x2601;
            g_state.fog_param[0] = end * k + 1.0f;
            g_state.fog_param[1] = -k;
            break;
        }
        default: {  // D3DFOG_EXP 1, D3DFOG_EXP2 2
            bool exp = rs(gpu::kRsFogTableMode) == 1;
            g_state.fog_mode = exp ? 0x800 : 0x801;
            g_state.fog_param[0] = 1.5f;
            g_state.fog_param[1] = rsf(gpu::kRsFogDensity) * (exp ? kExpScale : kExp2Scale);
            break;
        }
    }
}

void ApplyFlush(uint32_t dirty) {
    if (dirty & kDirtyFog) {
        PushFog();
        if (!(g_ps_handle && g_ps_final)) {
            // and the final combiner over the one the title wrote raw: a title whose own final combiner reads the
            // fog factor the other way round (Conker: fog.a * fog + (1 - fog.a) * R0) gets the plain one back
            bool fog = R32(kRenderState + gpu::kRsFogEnable * 4) != 0;
            bool spec = R32(kRenderState + gpu::kRsSpecularEnable * 4) != 0;
            g_state.simple[gpu::kMPsFinalAbcd >> 2] =
                fog ? kFinalFogged | (spec ? kFinalSpecular : 0) : kFinalPlain | (spec ? 2 : 0);
            g_state.simple[gpu::kMPsFinalEfgh >> 2] = kFinalEfgh;
        }
    }
    if (dirty & kDirtyLights) g_spec_raw = false;  // and SPECULAR_ENABLE from D3DRS_SPECULARENABLE
    // with no pixel shader set the library compiles its fixed function combiner from the stage states and pushes it
    if ((dirty & kDirtyCombiners) && !g_ps_handle && g_state.ps && g_state.ps->handle == kRawCombiner) g_state.ps = nullptr;
}

// A D3D draw's library body flushes the deferred state before it pushes the primitive; the HLE'd draws do the part
// of that flush which decides between the D3D states and the title's raw writes, and clear those bits as it would
void FlushForDraw() {
    uint32_t d = R32(kDirtyFlags), mine = d & (kDirtyFog | kDirtyCombiners | kDirtyLights);
    if (!mine) return;
    ApplyFlush(mine);
    W32(kDirtyFlags, d & ~mine);
}
}  // namespace

void DeferredFlush(uint32_t dirty) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // raw commands pushed before the flush came first
    ApplyFlush(dirty);
}

void FlushPending() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
}

}  // namespace d3d

using namespace d3d;

namespace {
std::atomic<uint32_t> g_presents{0};
}  // namespace

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    g_interval = interval;
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();
    // the fields the skipped device init would point at its NV2A record block (see DeviceRecords)
    {
        uint32_t records = DeviceRecords();
        SetDev(0x30, records);
        SetDev(0x2488, records);
        SetDev(0x2490, records + 0x20);
        SetDev(0x248c, records + 0x40);
    }

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state = gpu::State{};
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
#if HAS_D3D_DeviceInit
    // some titles link the library's internals into the game and call them directly; those read the device
    // state this build's own init sets up (the miniport at +0x1C28 among it), so run it if the config names it
    if (SYM(D3D_DeviceInit)) {
#if HAS_D3D_InitSize0
        if (!R32(SYM(D3D_InitSize0))) W32(SYM(D3D_InitSize0), 0x8000);
#endif
#if HAS_D3D_InitSize1
        if (!R32(SYM(D3D_InitSize1))) W32(SYM(D3D_InitSize1), 0x80000);
#endif
        kernel::CallGuest(ctx, SYM(D3D_DeviceInit), {kDevice, pp});
    }
#endif
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    if (diag::DumpMethods() && (a[0] || a[1])) {
        static std::unordered_set<uint64_t> seen;
        uint64_t key = uint64_t(a[0]) << 32 | a[1];
        if (seen.insert(key).second)
            std::fprintf(stderr, "SETRT rt=%08X [%08X %08X %08X %08X] zs=%08X [%08X %08X %08X %08X]\n", a[0],
                         a[0] ? R32(a[0]) : 0, a[0] ? R32(a[0] + 4) : 0, a[0] ? R32(a[0] + 12) : 0, a[0] ? R32(a[0] + 16) : 0,
                         a[1], a[1] ? R32(a[1]) : 0, a[1] ? R32(a[1] + 4) : 0, a[1] ? R32(a[1] + 12) : 0, a[1] ? R32(a[1] + 16) : 0);
    }
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget), zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_Swap, 1) {
    x86_profile_frame();
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    {
        // a frame far over its period: say so, the compile/pipeline lines before it say whether the renderer was why
        using clock = std::chrono::steady_clock;
        static clock::time_point last = clock::now();
        auto now = clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (ms > 250 && g_presents > 2) LOGR("d3d: frame %u took %.0f ms", g_presents.load(), ms);
    }
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    // D3DPRESENT_INTERVAL_ONE 1, TWO 2, THREE 4, FOUR 8, IMMEDIATE bit 31; DEFAULT 0 is ONE. The render state, when
    // the title sets one, replaces the device's.
    uint32_t interval = R32(kRenderState + gpu::kRsPresentationInterval * 4);
    if (!interval) interval = g_interval;
    uint32_t vblanks = interval & 0x80000000u ? 1 : interval & 8 ? 4 : interval & 4 ? 3 : interval & 2 ? 2 : 1;
    app::Pace(vblanks);
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (diag::DumpMethods()) {
        static std::unordered_set<uint64_t> seen;
        uint64_t key = uint64_t(g_state.rt.data) << 32 | ((uint64_t(a[2]) << 8) | (a[3] & 0xFF));
        if (seen.insert(key).second)
            std::fprintf(stderr, "CLEARTARGET rt=%08X flags=%X color=%08X\n", g_state.rt.data, a[2], a[3]);
    }
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LOG_FIRST(8, "d3d: CopyRects src %08X dst %08X n %u", a[0], a[3], a[2]);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                                \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) {          \
        std::lock_guard<std::recursive_mutex> lock(g_lock);    \
        Walk(Dev(kDevPush));                                   \
        SetComplex(rs, a[0]);                                  \
        return 0;                                              \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
namespace {
void NoteStencil(const char* how, uint32_t v, uint32_t from);
}  // namespace
HLE_STDCALL(D3DDevice_SetRenderState_StencilEnable, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    NoteStencil("from", a[0], R32(ctx.esp));
    SetComplex(gpu::kRsStencilEnable, a[0]);
    return 0;
}
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
HLE_STDCALL(D3DDevice_SetRenderState_FrontFace, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    SetComplex(gpu::kRsFrontFace, a[0]);
    CullPush(g_state.rs[gpu::kRsCullMode]);  // the library's FrontFace pushes the cull face again
    return 0;
}
HLE_STDCALL(D3DDevice_SetRenderState_CullMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    SetComplex(gpu::kRsCullMode, a[0]);
    CullPush(a[0]);
    return 0;
}
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    bool new_stride = stride != R32(kStreams + n * kStreamSize + XDK_Stream_Stride);
    AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    Release(ctx, old);
    // as the library: the draw re-points the arrays, and pushes the formats again for a new stride
    W32(kDirtyFlags, R32(kDirtyFlags) | (new_stride ? kDirtyVertexFormat | kDirtyStreams | 0x20 : kDirtyStreams));
    if (!g_state.streams[n].fmt) g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X (from %08X)", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type, R32(ctx.esp));
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    g_chip_program = false;  // the library body would have uploaded this shader's program over the chip's
    W32(kDirtyFlags, R32(kDirtyFlags) | kDirtyVertexFormat | kDirtyStreams | 0x20);
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    LoadConstants(a[0], a[1], 1);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    LoadConstants(a[0], a[1], 1);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    LoadConstants(a[0], a[1], 4);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    LoadConstants(a[0], a[1], a[2] / 4);
    return 0;
}

// LoadVertexShader(handle, address) puts a shader's program into the chip's program memory at an instruction
// slot; SelectVertexShader(handle, address) starts the program at that slot with the handle's declaration (a null
// or unknown handle keeps the bound one). Together they are how a title runs a program it uploaded itself.
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(4, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    const auto& prog = it->second->program;
    for (size_t i = 0; i + 4 <= prog.size() && a[1] + i / 4 < kProgramSlots; i += 4)
        std::memcpy(g_prog[a[1] + i / 4], &prog[i], 16);
    g_chip_dirty = true;
    return 0;
}
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        g_state.vs_handle = a[0];
        g_state.vs = it->second;
        for (auto& [reg, v] : it->second->constants)
            if (reg < 192) g_state.vsc[reg] = v;
    } else if (a[0] & 1) {
        LOG_FIRST(4, "d3d: SelectVertexShader with unknown handle %08X at %u", a[0], a[1]);
    }
    if (a[1] < kProgramSlots) g_prog_start = a[1];
    g_chip_program = g_chip_dirty = true;
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // state set around a raw push must not reach the pending commands first
    if (diag::DumpMethods()) std::fprintf(stderr, "SETPS %08X\n", a[0]);
    g_state.ps = nullptr;
    g_ps_handle = a[0];
    // the library marks the fog dirty when the final combiner the fog flush skipped is no longer the shader's own
    bool final = false;
    if (a[0] && XDK_PixelShader_Definition != xdk::kUnknown) {
        uint32_t def = R32(a[0] + XDK_PixelShader_Definition);
        final = (R32(def + 0x20) | R32(def + 0x24)) != 0;
    }
    if (g_ps_final && !final) W32(kDirtyFlags, R32(kDirtyFlags) | kDirtyFog);
    if (a[0]) g_ps_final = final;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    g_state.chip_tfactor = 0;  // the factors are the shader's constants now, not a texture factor
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    if (diag::DumpMethods())
        std::fprintf(stderr, "PSDEF %08X: %08X %08X %08X %08X | %08X %08X %08X %08X | count %u texmode %08X\n", g_state.ps->handle,
                     d[0], d[1], d[2], d[3], d[34], d[35], d[36], d[37], d[53], d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

namespace {
// the chip's current program, start to final instruction, and a hash that tells programs apart
const std::vector<uint32_t>& ChipProgram(uint64_t& hash) {
    static std::vector<uint32_t> prog;
    static uint64_t h = 0;
    if (g_chip_dirty) {
        g_chip_dirty = false;
        prog.clear();
        h = 1469598103934665603ull;
        for (uint32_t i = g_prog_start; i < kProgramSlots; i++) {
            prog.insert(prog.end(), g_prog[i], g_prog[i] + 4);
            for (uint32_t w : g_prog[i]) h = (h ^ w) * 1099511628211ull;
            if (g_prog[i][3] & 1) break;  // FLD_FINAL
        }
        h |= 1;
        // XBE_DUMP_PROGRAM: each distinct chip program once, start slot and words
        static const bool dump = std::getenv("XBE_DUMP_PROGRAM") != nullptr;
        static std::unordered_set<uint64_t> seen;
        if (dump && seen.insert(h).second) {
            std::fprintf(stderr, "CHIPPROG %016llX start %u n %zu\n", (unsigned long long)h, g_prog_start, prog.size() / 4);
            for (size_t i = 0; i < prog.size(); i += 4)
                std::fprintf(stderr, "  %08X %08X %08X %08X\n", prog[i], prog[i + 1], prog[i + 2], prog[i + 3]);
        }
    }
    hash = h;
    return prog;
}

// Draws with the program the chip would run. When the title's own upload is current, the bound shader keeps its
// declaration (the vertex layout is the D3D side's business, the chip program says nothing about it) and the program
// is swapped for the chip's. One object per (bound shader, program): the renderers tell shaders apart by pointer.
// the capture shows each draw's stencil state; who turned the stencil test on or off it can't, so say it
void NoteStencil(const char* how, uint32_t v, uint32_t from) {
    if (!g_backend) return;
    char text[64];
    std::snprintf(text, sizeof(text), "stencil enable %u %s %08X", v, how, from);
    g_backend->Note(text);
}

void BackendDraw(uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!g_backend) return;
    if (!g_chip_program) {
        g_backend->Draw(g_state, prim, start, count, indices);
        return;
    }
    uint64_t hash;
    const auto& prog = ChipProgram(hash);
    if (prog.empty()) {
        g_backend->Draw(g_state, prim, start, count, indices);
        return;
    }
    auto bound = g_state.vs;
    uint32_t handle = g_state.vs_handle;
    static std::map<std::tuple<const void*, uint32_t, uint64_t>, std::shared_ptr<gpu::VertexShader>> made;
    auto& vs = made[{bound.get(), (handle & 1) ? 1u : handle, hash}];
    if (!vs) {
        vs = std::make_shared<gpu::VertexShader>();
        if (handle & 1) {
            if (bound) {
                vs->decl = bound->decl;
                vs->inputs = bound->inputs;
                vs->stream_stride = bound->stream_stride;
                vs->handle = bound->handle;
            }
        } else {
            gpu::FvfInputs(handle, *vs);  // an FVF's layout under the title's program
        }
        vs->program = prog;
        vs->program_type = 0x2078;
        vs->hash = hash;
    }
    g_state.vs = vs;
    g_state.vs_handle = handle | 1;
    g_backend->Draw(g_state, prim, start, count, indices);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}
}  // namespace

// XBE_DUMP_METHODS: where in the title each draw comes from, to map the frame's passes
void LogDrawCaller(const char* kind, uint32_t prim, uint32_t count, uint32_t caller) {
    if (!diag::DumpMethods()) return;
    static std::unordered_map<uint64_t, uint32_t> seen;
    uint64_t key = uint64_t(caller) << 32 | prim;
    if (seen[key]++ == 0)
        std::fprintf(stderr, "DRAWCALL %s prim=%u firstcount=%u vs=%08X blend=%u %X %X rt=%08X from %08X\n", kind, prim,
                     count, g_state.vs_handle, g_state.simple[gpu::kMBlendEnable >> 2], g_state.simple[gpu::kMSrcBlend >> 2],
                     g_state.simple[gpu::kMDestBlend >> 2], g_state.rt.data, caller);
}

// A D3D draw's library body pushes NV097_SET_VERTEX_DATA_ARRAY_FORMAT for all sixteen slots from the bound
// declaration before the primitive, replacing whatever raw binds the title left there. The HLE'd draws skip that
// body, so drop the inline formats the same way: the declaration is the layout again. Without this a D3D draw made
// right after one of the title's raw draws read its vertices through that draw's attribute layout - Conker's
// visibility test boxes then collapsed to a point, counted 0 pixels, and the animated meshes they cull flashed out.
// The library's draw (SetStateVB, 0x543200 in Conker) pushes the vertex attribute formats of the shader's
// declaration only while the vertex format is dirty (SetVertexShader, a SetStreamSource with a new stride), and
// re-points the arrays at the D3D streams when those changed; otherwise the chip keeps the formats and arrays the
// title last bound inline. The bits are cleared as the library does.
void DeclarationBinds() {
    uint32_t d = R32(kDirtyFlags);
    W32(kDirtyFlags, d & ~(kDirtyVertexFormat | kDirtyStreams));
    if (d & kDirtyNoVertexState) return;
    bool format = d & kDirtyVertexFormat;
    if (!format && !(d & kDirtyStreams)) return;
    for (uint32_t n = 0; n < 16; n++) {
        auto& st = g_state.streams[n];
        if (!format && st.fmt) continue;
        st = {R32(kStreams + n * kStreamSize + XDK_Stream_Buffer), R32(kStreams + n * kStreamSize + XDK_Stream_Stride)};
    }
}

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    FlushForDraw();
    DeclarationBinds();
    SyncDeferred();
    LogDrawCaller("verts", a[0], a[2], R32(ctx.esp));
    BackendDraw(a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    FlushForDraw();
    DeclarationBinds();
    SyncDeferred();
    LogDrawCaller("indexed", a[0], a[1], R32(ctx.esp));
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    BackendDraw(a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

// XBE_DUMP_METHODS: the raw NV2A state methods seen since the last draw, from the title's inlined library, to
// compare what it writes with what the runtime decodes into the draw
constexpr uint32_t kRawRing = 96;
uint32_t g_raw_ring[kRawRing][2];
uint32_t g_raw_ring_count = 0;

void NoteRawMethod(uint32_t m, uint32_t v) {
    if (!diag::DumpMethods()) return;
    g_raw_ring[g_raw_ring_count % kRawRing][0] = m;
    g_raw_ring[g_raw_ring_count % kRawRing][1] = v;
    g_raw_ring_count++;
}

// XBE_DUMP_METHODS: the first value seen for each NV2A method the title pushes, so the whole vocabulary is visible
void NoteMethodSeen(uint32_t m, uint32_t v) {
    if (!diag::DumpMethods()) return;
    static std::unordered_map<uint32_t, uint32_t> first;
    if (first.emplace(m, v).second) std::fprintf(stderr, "METHOD %04X = %08X\n", m, v);
    // the vertex stream registers, to tell an init-only block from a per-draw rebind
    if (m == 0x1720 || m == 0x1760 || m == 0x1724) {
        static uint32_t n = 0;
        if (n++ < 120) std::fprintf(stderr, "STREAM %04X = %08X\n", m, v);
    }
    // are the other unhandled ranges live too, or written once at init?
    if (m == 0x0A20 || m == 0x1B00 || m == 0x1B04 || m == 0x384 || m == 0x290) {
        static uint32_t n = 0;
        if (n++ < 60) std::fprintf(stderr, "LIVE %04X = %08X\n", m, v);
    }
}

void DumpRawMethods(const char* what, uint32_t handle, uint32_t prim, uint32_t count) {
    if (!diag::DumpMethods()) return;
    uint32_t n = g_raw_ring_count < kRawRing ? g_raw_ring_count : kRawRing;
    std::fprintf(stderr, "RAWDRAW %s vs=%08X prim=%u n=%u methods=%u blend=%u %X %X tss0 %u %X %X %X\n", what, handle,
                 prim, count, g_raw_ring_count, g_state.simple[gpu::kMBlendEnable >> 2], g_state.simple[gpu::kMSrcBlend >> 2],
                 g_state.simple[gpu::kMDestBlend >> 2], g_state.tss[0][gpu::kTssColorOp], g_state.tss[0][gpu::kTssColorArg1],
                 g_state.tss[0][gpu::kTssColorArg2], g_state.tss[0][gpu::kTssColorArg0]);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_raw_ring_count - n + i) % kRawRing;
        std::fprintf(stderr, "   %04X = %08X\n", g_raw_ring[idx][0], g_raw_ring[idx][1]);
    }
    g_raw_ring_count = 0;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    DumpRawMethods("up", g_state.vs_handle, prim, count);
    // the caller supplies the whole array in its own layout: the inline format binds belong to some other draw,
    // so hide them for this one and let the declaration read the array against the stride given
    auto streams = g_state.streams;
    for (auto& st : g_state.streams) st.fmt = 0;
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    BackendDraw(prim, 0, count, nullptr);
    g_state.streams = streams;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex
std::vector<uint32_t> g_im_raw;         // a whole vertex array pushed through NV2A inline array
std::vector<uint16_t> g_im_indices;     // indices pushed through NV097_ARRAY_ELEMENT16/32
std::vector<std::pair<uint32_t, uint32_t>> g_im_ranges;  // NV097_DRAW_ARRAYS (start, count), contiguous ones joined
std::array<uint32_t, 16> g_vb_header{};  // synthetic vertex buffer headers for the inline stream binds

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) g_state.simple[gpu::kMCountEnable >> 2] = v;
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// The engine rebinds textures inline: NV097_SET_TEXTURE_OFFSET and _FORMAT, in the stage's own 0x40-byte block
// of methods. D3DDevice_SwitchTexture is unwired, so its library body runs and pushes exactly that, which is how
// a stage gets pointed at a freshly rendered surface. Nothing consumed the values, they only reached simple[].
// Build the D3DPixelContainer the renderers already read from them: the offset arrives as a raw physical address
// (surface headers carry the 0x80000000 view), and the format word has the same layout as the D3D one Describe
// decodes - colour bits 15:8, dimensionality 7:4, cube bit 2, mip levels 19:16, log2 sizes from bit 20. One
// header per distinct pair keeps the renderers' caches steady (FindImage keys on the decoded values).
uint32_t InlineTexture(uint32_t offset, uint32_t format) {
    if (!offset || !format) return 0;
    // every header is kept: a live header must stay stable, and a bounded table that forgot entries made a scene
    // with more distinct pairs than it held allocate a header on every rebind until system memory ran out
    static std::unordered_map<uint64_t, uint32_t> headers;
    uint32_t& h = headers[uint64_t(offset) << 32 | format];
    if (h) return h;
    h = mem::AllocSystem(20);
    W32(h + 0, 0x01040001);            // D3DPixelContainer
    W32(h + 4, offset | 0x80000000u);  // Describe() maps the physical view back to the virtual address
    W32(h + 8, 0);
    W32(h + 12, format);
    W32(h + 16, 0);  // no linear size: the log2 dimensions in the format word are what describe the surface
    if (std::getenv("XBE_TRACE_WALK"))
        std::fprintf(stderr, "INLINETEX header=%08X offset=%08X format=%08X\n", h, offset, format);
    return h;
}

// A stage's sampler registers the title wrote raw (NV097_SET_TEXTURE_ADDRESS, _CONTROL0, _FILTER, _BORDER_COLOR,
// _SET_BUMP_ENV_*), as the D3D texture stage states the renderers read. They go into the title's own deferred stage
// table as well, which SyncDeferred re-reads every draw, so a raw push and an inlined SetTextureStageState simply
// overwrite each other in the order the title issued them - the same last-writer rule the chip has.
void RawTextureState(uint32_t stage, uint32_t sub, uint32_t v) {
    auto put = [&](uint32_t field, uint32_t value) {
        g_state.tss[stage][field] = value;
        W32(kTextureState + (stage * kStageStates + field) * 4, value);
    };
    auto putf = [&](uint32_t field, float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        put(field, bits);
    };
    switch (sub) {
        case 0x08:  // ADDRESS: U 3:0, V 11:8, P 19:16; the NV2A codes are D3DTADDRESS's
            put(gpu::kTssAddressU, v & 0xF);
            put(gpu::kTssAddressV, (v >> 8) & 0xF);
            put(gpu::kTssAddressW, (v >> 16) & 0xF);
            break;
        case 0x0C:  // CONTROL0: colour key 1:0 (D3DTCOLORKEYOP), alpha kill bit 2, anisotropy log2 5:4
            put(gpu::kTssColorKeyOp, v & 3);
            put(gpu::kTssAlphaKill, v & 4 ? 4 : 0);  // D3DTALPHAKILL_ENABLE
            put(gpu::kTssMaxAnisotropy, 1u << ((v >> 4) & 3));
            break;
        case 0x14: {  // FILTER: lod bias 12:0 (signed 5.8), min 23:16, mag 27:24, channel signs 31:28
            static const uint8_t kMin[8][2] = {{2, 0}, {1, 0}, {2, 0}, {1, 1}, {2, 1}, {1, 2}, {2, 2}, {2, 2}};  // D3DTEXF min, mip
            uint32_t mn = (v >> 16) & 7, mag = (v >> 24) & 0xF;
            put(gpu::kTssMinFilter, kMin[mn][0]);
            put(gpu::kTssMipFilter, kMin[mn][1]);
            put(gpu::kTssMagFilter, mag == 1 ? 1 : 2);
            put(gpu::kTssColorSign, v & 0xF0000000);
            putf(gpu::kTssMipLodBias, float(int32_t(v << 19) >> 19) / 256.0f);
            break;
        }
        case 0x24: put(gpu::kTssBorderColor, v); break;
        case 0x28: put(gpu::kTssBumpEnvMat00, v); break;  // xemu: the matrix is 00 01 10 11
        case 0x2C: put(gpu::kTssBumpEnvMat01, v); break;
        case 0x30: put(gpu::kTssBumpEnvMat10, v); break;
        case 0x34: put(gpu::kTssBumpEnvMat11, v); break;
        case 0x38: put(gpu::kTssBumpEnvLScale, v); break;
        case 0x3C: put(gpu::kTssBumpEnvLOffset, v); break;
        default: break;
    }
}

// the pixel shader object standing for a combiner the title wrote as raw methods: no D3D definition, constants left
// as the title wrote them (every PSC mapping 0xF)
std::shared_ptr<gpu::PixelShader> RawCombiner() {
    static std::shared_ptr<gpu::PixelShader> raw;
    if (!raw) {
        raw = std::make_shared<gpu::PixelShader>();
        raw->handle = kRawCombiner;
        raw->def[57] = raw->def[58] = raw->def[59] = 0xFFFFFFFF;
    }
    return raw;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, the rest is skipped
void Walk(uint32_t end) {
    // the two silent exits below drop every command that follows, so make them observable
    static const bool trace = std::getenv("XBE_TRACE_WALK") != nullptr;
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) {
        if (trace) {
            static uint32_t seen = 0;
            if (seen++ < 40)
                std::fprintf(stderr, "WALKBAIL end=%08X walk=%08X base=%08X top=%08X reason=%s\n", end, g_walk,
                             g_scratch_push, g_scratch_push + g_scratch_size,
                             !g_walk ? "no-base" : end < g_walk ? "backwards" : "past-end");
        }
        return;
    }
    // Command streams chain: the title's inline push buffers link blocks with jumps, and xemu follows them
    // (pfifo.c: bit 1 of the header is a jump, bits 1:0 == 2 a call, the word 0x00020000 a return, and
    // (word & 0xe0000003) == 0x20000000 an old jump). Jump targets are physical addresses in the console's RAM,
    // so they carry the 0x80000000 view the rest of this file uses for guest data. Without this, the first jump
    // ended the walk and every command after it in that buffer was silently dropped.
    uint32_t p = g_walk, limit = end, ret[16], sp = 0, guard = 0, jumps = 0, home = 0, resyncs = 0;
    // jump targets land on real blocks (vertex-program words, prebuilt stream bindings), so following them is the
    // xemu behaviour; XBE_NO_WALK_JUMPS restores the old "stop at the first jump" for comparison
    static const bool follow = std::getenv("XBE_NO_WALK_JUMPS") == nullptr;
    auto aim = [&](uint32_t t) {
        // a target inside the buffer we were handed stays bounded by it, anything else is read as physical RAM.
        // The scratch buffer is a ring: a jump to the put pointer is the end of the stream (the chip stops there),
        // and one past it lands in what an earlier frame left, which a block jumping "back" with a stale return
        // address does. Following either used to run on through stale memory into the zeroed low RAM and its ring
        // wrap, spinning until the packet guard - a ~300 ms stall a few times a minute.
        if (t == end) {
            p = limit = end;
            return true;
        }
        if (t >= g_scratch_push && t < g_scratch_push + g_scratch_size && t > end) return false;
        if (t >= g_scratch_push && t < end) limit = end;
        else if (t >= 0x80000000u && t < 0x84000000u) limit = 0x84000000u;
        else return false;
        p = t;
        jumps++;
        return true;
    };
    // a jump the walk cannot follow: carry on after the last jump site in this buffer, where the block would have
    // returned, else stop
    auto resume = [&]() {
        if (!home || resyncs >= 64) return false;
        resyncs++;
        p = home;
        limit = end;
        home = 0;
        return true;
    };
    // the last few jumps, so a stop can show how the walk got there
    static uint32_t jring[8][2];
    static uint32_t jn = 0;
    while (p + 4 <= limit && guard++ < (1u << 22)) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if (follow && (head & 0xE0000003) == 0x20000000) {  // old jump
            uint32_t t = (head & 0x1FFFFFFF) | 0x80000000u, s = jn++ & 7;
            jring[s][0] = p - 4;
            jring[s][1] = t;
            if (p - 4 >= g_scratch_push && p - 4 < end) home = p;  // where this buffer resumes
            if (!aim(t) && !resume()) break;
            continue;
        }
        if (follow && (head & 3) == 1) {  // jump
            uint32_t t = (head & 0xFFFFFFFC) | 0x80000000u, s = jn++ & 7;
            jring[s][0] = p - 4;
            jring[s][1] = t;
            if (p - 4 >= g_scratch_push && p - 4 < end) home = p;  // where this buffer resumes
            if (!aim(t) && !resume()) break;
            continue;
        }
        if (follow && head == 0x00020000) {  // return
            if (trace) {
                static uint32_t rn = 0;
                if (rn++ < 20) std::fprintf(stderr, "RET       %08X -> %08X\n", p - 4, sp >= 2 ? ret[sp - 1] : 0);
            }
            if (sp < 2) break;
            limit = ret[--sp];
            p = ret[--sp];
            continue;
        }
        if (follow && (head & 3) == 2) {  // call
            if (sp + 2 > 16) break;
            uint32_t rp = p, rl = limit;
            if (!aim((head & 0xFFFFFFFC) | 0x80000000u)) {
                if (!resume()) break;
                continue;
            }
            ret[sp++] = rp;
            ret[sp++] = rl;
            continue;
        }
        if ((head & 0xA0030003) || n * 4 > limit - p) {
            // A jump chain that ran off its block lands in memory that is not a command stream at all — very
            // often stale bytes of this same scratch buffer past the push pointer. Rather than throw away the
            // rest of the buffer that jumped out, resume at the word after the jump site, which is where the
            // block would have returned to.
            bool inmain = (p - 4) >= g_scratch_push && (p - 4) < end;
            if (!inmain && home && resyncs < 64) {
                resyncs++;
                if (trace) {
                    static uint32_t rt = 0;
                    if (rt++ < 4 || rt % 2000 == 0)
                        std::fprintf(stderr, "RESYNC %u at %08X head=%08X -> %08X\n", rt, p - 4, head, home);
                }
                p = home;
                limit = end;
                home = 0;
                continue;
            }
            if (trace) {
                static uint32_t total = 0, last = 0;
                total++;
                last = head;
                if (total <= 8) {
                    std::fprintf(stderr,
                                 "WALKSTOP head=%08X at %08X n=%u reason=%s end=%08X walk=%08X inmain=%u\n", head, p - 4,
                                 n, (head & 0xA0030003) ? "header" : "count", end, g_walk,
                                 p - 4 >= g_scratch_push && p - 4 < end);
                    uint32_t have = jn < 8 ? jn : 8;
                    std::fprintf(stderr, "  last jumps:");
                    for (uint32_t k = 0; k < have; k++) {
                        uint32_t idx = (jn - have + k) & 7;
                        std::fprintf(stderr, " %08X->%08X", jring[idx][0], jring[idx][1]);
                    }
                    std::fprintf(stderr, "\n");
                    uint32_t back = std::min<uint32_t>(128, (p - 4) - g_walk);
                    for (uint32_t q = (p - 4) - back; q < p - 4 + 32; q += 4) {
                        if ((q - ((p - 4) - back)) % 32 == 0) std::fprintf(stderr, "\n  %08X:", q);
                        std::fprintf(stderr, " %08X", R32(q));
                    }
                    std::fprintf(stderr, "\n");
                } else if (total % 100 == 0) {
                    std::fprintf(stderr, "WALKTOT stops=%u jumps=%u lasthead=%08X\n", total, jumps, last);
                }
            }
            break;  // anything but a command header
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            NoteMethodSeen(m, v);
            ApplyVisibilityMethod(m, v);
            if (m == 0x1800 || m == 0x1808 || method == 0x1800 || method == 0x1808) {
                // NV097_ARRAY_ELEMENT16/32. The title indexes its geometry, and xemu marks both methods
                // non-incrementing (pgraph.c DEF_METHOD_NON_INC), so every value in the packet is an index.
                // 16 packs two, low half first; 32 carries one, and anything above 16 bits is not an index.
                // Without this an indexed pass pushes no vertices and the draw at END had nothing to emit.
                if (g_im_prim) {
                    if ((m | method) == 0x1808) {
                        if (v <= 0xFFFF) g_im_indices.push_back(uint16_t(v));
                    } else {
                        g_im_indices.push_back(uint16_t(v & 0xFFFF));
                        g_im_indices.push_back(uint16_t(v >> 16));
                    }
                } else if (trace) {
                    static uint32_t n = 0;
                    if (n++ < 20) std::fprintf(stderr, "INDEXOUT m=%04X v=%08X prim=%u raw=%zu verts=%zu\n", m, v,
                                               g_im_prim, g_im_raw.size(), g_im_vertices.size());
                }
                continue;
            }
            if (m == 0x17FC) {  // begin/end, 0 ends
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                    g_im_raw.clear();
                    g_im_indices.clear();
                    g_im_ranges.clear();
                } else if (!g_im_ranges.empty()) {
                    // runs of the bound vertex arrays, each its own primitive
                    uint32_t open = g_im_prim;
                    g_im_prim = 0;
                    if (open) {
                        SyncDeferred();
                        LOG_FIRST(4, "d3d: inline draw arrays, prim %u, %zu runs", open, g_im_ranges.size());
                        uint32_t base = std::exchange(g_state.base_vertex, 0);  // the chip has no base vertex
                        for (auto [first, n] : g_im_ranges) BackendDraw(open, first, n, nullptr);
                        g_state.base_vertex = base;
                    }
                    g_im_ranges.clear();
                } else if (!g_im_raw.empty()) {
                    uint32_t bytes = uint32_t(g_im_raw.size()) * 4, stride = VertexBytes();
                    if (stride && bytes % stride == 0) {
                        DrawUp(g_im_prim, bytes / stride, g_im_raw.data(), stride);
                    } else if (g_state.vs && bytes % 28 == 0) {
                        // the title pushes whole vertex arrays through the inline array method in a fixed 7 dword
                        // layout (position xyzw, D3DCOLOR, texcoord xy). Its program's declaration reads the same
                        // stride with an XYZ position, so give the draw the layout the array really has.
                        static std::shared_ptr<gpu::VertexShader> raw;
                        if (!raw) {
                            raw = std::make_shared<gpu::VertexShader>();
                            raw->inputs[0] = {0, 0, 0x42};
                            raw->inputs[1] = {0, 16, 0x40};
                            raw->inputs[2] = {0, 20, 0x22};
                            raw->stream_stride[0] = 28;
                        }
                        raw->program = g_state.vs->program;
                        raw->program_type = g_state.vs->program_type;
                        auto bound = g_state.vs;
                        g_state.vs = raw;
                        g_state.vs_handle |= 1;
                        DrawUp(g_im_prim, bytes / 28, g_im_raw.data(), 28);
                        g_state.vs = bound;
                    } else {
                        LOG_FIRST(4, "d3d: inline array primitive %u dropped, %u bytes of vertices of %u", g_im_prim, bytes, stride);
                    }
                    g_im_prim = 0;
                    g_im_raw.clear();
                } else if (!g_im_indices.empty()) {
                    // an indexed pass: the vertices come from the streams bound by 0x1720/0x1760, the indices
                    // were pushed inline. The backend takes 16 bit indices, start is the first vertex.
                    uint32_t open = g_im_prim;
                    g_im_prim = 0;
                    if (g_backend && open) {
                        SyncDeferred();
                        LOG_FIRST(4, "d3d: inline indexed draw, prim %u, %zu indices", open, g_im_indices.size());
                        DumpRawMethods("walkidx", g_state.vs_handle, open, uint32_t(g_im_indices.size()));
                        uint32_t base = std::exchange(g_state.base_vertex, 0);  // the chip has no base vertex
                        BackendDraw(open, 0, uint32_t(g_im_indices.size()),
                                    reinterpret_cast<const uint8_t*>(g_im_indices.data()));
                        g_state.base_vertex = base;
                    }
                    g_im_indices.clear();
                } else {
                    EndPrimitive();
                }
                continue;
            } else if (m == 0x1810) {
                // NV097_DRAW_ARRAYS: start in bits 23:0, count less one in 31:24. xemu joins a run that continues
                // the previous one (a long strip arrives split into 256 vertex pieces)
                uint32_t first = v & 0xFFFFFF, n = (v >> 24) + 1;
                if (g_im_prim) {
                    if (!g_im_ranges.empty() && g_im_ranges.back().first + g_im_ranges.back().second == first)
                        g_im_ranges.back().second += n;
                    else
                        g_im_ranges.push_back({first, n});
                }
                continue;
            } else if (m == 0x1818) {  // the inline array method: raw vertex data, not a state
                if (g_im_prim) g_im_raw.push_back(v);
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1900 && m < 0x1940) {
                // NV097_SET_VERTEX_DATA2S: two signed shorts (xemu pgraph.c), z = 0 and w = 1, 4 bytes a slot.
                // Slot 0 completes the vertex. The title uses this for the pass that fills the scene's textures.
                uint32_t slot = (m - 0x1900) / 4;
                a += slot * 4;
                a[0] = float(int16_t(v & 0xFFFF));
                a[1] = float(int16_t(v >> 16));
                a[2] = 0;
                a[3] = 1;
                last = slot == 0;
            } else if (m >= 0x1980 && m < 0x1A00) {
                // NV097_SET_VERTEX_DATA4S_M: four signed shorts, two methods a slot, completes on slot 0 part 1
                uint32_t i = (m - 0x1980) / 4, slot = i / 2, part = i & 1;
                a += slot * 4;
                a[part * 2 + 0] = float(int16_t(v & 0xFFFF));
                a[part * 2 + 1] = float(int16_t(v >> 16));
                last = slot == 0 && part == 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else if (m == 0x1EA4) {  // SET_TRANSFORM_CONSTANT_LOAD: the register the data that follows loads
                g_vs_const_reg = v;
                continue;
            } else if (m >= 0xB80 && m < 0xC00) {
                // SET_TRANSFORM_CONSTANT: one float per method, the load pointer advances after a register's fourth
                // (xemu pgraph.c), so a long upload continues across packets
                uint32_t slot = (m - 0xB80) / 4;
                if (g_vs_const_reg < 192) g_state.vsc[g_vs_const_reg][slot & 3] = f;
                if ((slot & 3) == 3) g_vs_const_reg++;
                continue;
            } else if (m >= 0xB00 && m < 0xB80) {
                // SET_TRANSFORM_PROGRAM: program words at the load pointer, which advances every fourth word
                uint32_t slot = (m - 0xB00) / 4;
                if (g_prog_load < kProgramSlots) g_prog[g_prog_load][slot & 3] = v;
                if ((slot & 3) == 3) g_prog_load++;
                g_chip_program = g_chip_dirty = true;
                continue;
            } else if (m == 0x1E9C) {  // SET_TRANSFORM_PROGRAM_LOAD
                g_prog_load = v;
                continue;
            } else if (m == 0x1EA0) {  // SET_TRANSFORM_PROGRAM_START
                if (v < kProgramSlots) g_prog_start = v;
                g_chip_program = g_chip_dirty = true;
                continue;
            } else if (m >= 0x1720 && m < 0x1760) {
                // NV097_SET_VERTEX_DATA_ARRAY_OFFSET: the title binds vertex buffers inline, 16 slots 0x40 apart.
                // bits 30:0 are the data address (xemu: offset = parameter & 0x7fffffff, bit 31 selects DMA ctx).
                uint32_t slot = (m - 0x1720) / 4, &h = g_vb_header[slot];
                if (!h) h = mem::AllocSystem(16, 16);
                W32(h + 4, v & 0x7FFFFFFF);
                g_state.streams[slot].vb = h;
                continue;
            } else if (m >= 0x1760 && m < 0x17A0) {
                // NV097_SET_VERTEX_DATA_ARRAY_FORMAT: type bits 3:0, component count bits 7:4, byte stride 31:8.
                // Keep the type and count too: the title sets its vertex program inline, so for these draws the
                // stream bind is the only description of the layout there is.
                uint32_t slot = (m - 0x1760) / 4;
                g_state.streams[slot].stride = v >> 8;
                g_state.streams[slot].fmt = v & 0xFF;
                continue;
            } else if (m >= 0x1B00 && m < 0x1C00) {
                // NV097_SET_TEXTURE_*: 0x40 bytes a stage. Offset (sub 0) and format (sub 4) are rebinds the
                // renderers have to see, so route them through a D3DPixelContainer; the rest stays plain state.
                NoteRawMethod(m, v);
                g_state.simple[m >> 2] = v;
                uint32_t stage = (m - 0x1B00) / 0x40, sub = (m - 0x1B00) % 0x40;
                if (sub == 0 || sub == 4) {
                    static uint32_t off[4] = {}, fmt[4] = {};
                    (sub == 0 ? off : fmt)[stage] = v;
                    if (uint32_t h = InlineTexture(off[stage], fmt[stage])) g_state.textures[stage] = h;
                } else {
                    RawTextureState(stage, sub, v);
                }
                continue;
            } else {
                // the engine pushes most state as raw NV2A methods (the d3d api is inlined away); the backend
                // reads them by method number out of simple[]
                NoteRawMethod(m, v);
                g_state.simple[m >> 2] = v;
                if (m == 0x1E70) g_state.rs[gpu::kRsPsTextureModes] = v;  // NV097_SET_SHADER_STAGE_PROGRAM
                if (m == gpu::kMPsConstant0) g_state.chip_tfactor = 1;     // NV097_SET_COMBINER_FACTOR0, stage 0
                if (m == 0x3B8) {  // NV097_SET_SPECULAR_ENABLE
                    g_spec_raw = true;
                    g_spec_value = v;
                    g_state.rs[gpu::kRsSpecularEnable] = v;
                }
                // NV097_SET_STENCIL_TEST_ENABLE / _OP_FAIL: the renderers take these two from the render states, and
                // the title's material blocks set them raw (Conker's glow marks its surfaces in the stencil)
                if (m == gpu::kMStencilTestEnable) {
                    g_state.rs[gpu::kRsStencilEnable] = v;
                    NoteStencil("raw at", v, p);
                }
                if (m == gpu::kMStencilFail) g_state.rs[gpu::kRsStencilFail] = v;
                if (m == kMCullEnable || m == kMCullFace || m == kMFrontFace) ChipCull();
                // NV097_SET_FOG_ENABLE / _MODE / _PARAMS: the title runs the fog unit itself
                if (m == 0x2A4 || m == 0x29C || m == 0x9C0 || m == 0x9C4) {
                    g_state.chip_fog = 1;
                    if (m == 0x2A4) g_state.fog_enable = v;
                    else if (m == 0x29C) g_state.fog_mode = v;
                    else std::memcpy(&g_state.fog_param[(m - 0x9C0) / 4], &v, 4);
                }
                // the combiner's inputs, outputs, final stage and stage count written by the title itself: it now
                // runs that combiner, whatever pixel shader the D3D side last bound (the renderers read the
                // combiner out of simple[], the object only says "programmable")
                if ((m >= 0x260 && m < 0x280) || m == 0x288 || m == 0x28C || (m >= 0xAA0 && m < 0xAE0) ||
                    (m >= 0x1E40 && m <= 0x1E60))
                    g_state.ps = RawCombiner();
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
    }
    if (guard >= (1u << 22)) {
        static uint32_t n = 0;
        if (n++ < 8) {
            std::fprintf(stderr, "WALKGUARD at %08X end=%08X walk=%08X jumps=%u sp=%u last jumps:", p, end, g_walk, jumps, sp);
            uint32_t have = jn < 8 ? jn : 8;
            for (uint32_t k = 0; k < have; k++) {
                uint32_t idx = (jn - have + k) & 7;
                std::fprintf(stderr, " %08X->%08X", jring[idx][0], jring[idx][1]);
            }
            std::fprintf(stderr, "\n  at:");
            for (uint32_t q = p - 32; q < p + 32; q += 4) std::fprintf(stderr, " %08X", R32(q));
            std::fprintf(stderr, "\n");
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    FlushForDraw();
    LogDrawCaller("vertsup", a[0], a[1], R32(ctx.esp));
    // args 2/3 are a real pointer and byte stride (checked: they look like D005xxxx scratch and 0x18-0x30), so
    // HostPtr(a[2]) is right even though the library body also adds a base from CDevice+0xbcc (zero for these)
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// (PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, VertexStreamZeroStride). The library body copies
// each indexed vertex inline, in batches sized from the vertex shader at CDevice+0x794, which SetVertexShader's HLE
// never points at the shader set: every attribute reads disabled, the stride is zero and the batch size divides by
// it. The indices are expanded into a plain array here instead.
HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    FlushForDraw();
    LogDrawCaller("idxup", a[0], a[1], R32(ctx.esp));
    uint32_t count = a[1], stride = a[4];
    if (!count || !stride) return 0;
    static std::vector<uint8_t> vertices;
    vertices.resize(size_t(count) * stride);
    for (uint32_t i = 0; i < count; i++)
        std::memcpy(&vertices[size_t(i) * stride], HostPtr(a[3] + uint32_t(R16(a[2] + i * 2)) * stride), stride);
    DrawUp(a[0], count, vertices.data(), stride);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LogDrawCaller("endpush", 0, a[0], R32(ctx.esp));
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818, kElement16 = 0x1800, kElement32 = 0x1808;
    static std::vector<uint32_t> vertices;
    static std::vector<uint16_t> indices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            NoteMethodSeen(m, v);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (method == kElement16 || method == kElement32 || m == kElement16 || m == kElement32) {
                // NV097_ARRAY_ELEMENT16/32: the title's geometry is indexed and arrives here, not through
                // Walk. xemu marks both methods non-incrementing (pgraph.c DEF_METHOD_NON_INC), 16 packs two
                // indices low half first, 32 carries one and anything above 16 bits is not an index.
                if (prim) {
                    if (method == kElement32 || m == kElement32) {
                        if (v <= 0xFFFF) indices.push_back(uint16_t(v));
                    } else {
                        indices.push_back(uint16_t(v & 0xFFFF));
                        indices.push_back(uint16_t(v >> 16));
                    }
                }
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
                indices.clear();
            } else if (prim) {
                if (!indices.empty()) {
                    // the vertices come from the streams bound by 0x1720/0x1760; start is the first vertex
                    if (g_backend) {
                        LOG_FIRST(4, "d3d: EndPush indexed draw, prim %u, %zu indices", prim, indices.size());
                        BackendDraw(prim, 0, uint32_t(indices.size()), reinterpret_cast<const uint8_t*>(indices.data()));
                    }
                } else {
                    uint32_t stride = VertexBytes(), bytes = uint32_t(vertices.size()) * 4;
                    if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
                    else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                }
                prim = 0;
                indices.clear();
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
