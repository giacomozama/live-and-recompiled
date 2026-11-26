// Windows D3D11 backend. Device/swap-chain setup adapted from xboxrecomp's
// src/d3d/d3d8_device.c (MIT, sp00nznet and contributors). See D3D11-NOTICE.txt.
// Conker's own XDK state, vertex/texture decoders and HLSL translators are retained.
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <unordered_map>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include "gpu/backend.h"
#include "core/diag.h"
#include "gpu/draw.h"
#include "gpu/d3d11_shaders.h"
#include "gpu/fsr_hlsl.h"
#include "gpu/hud_split.h"
#include "gpu/interp.h"
#include "gpu/post_effects.h"
#include "gpu/post_hlsl.h"
#include "gpu/targets.h"
#include "gpu/smaa_tex.h"
#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"

namespace gpu {
namespace {
template<class T> class Com {
    T* p_ = nullptr;
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = std::exchange(o.p_, nullptr); } return *this; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() { reset(); return &p_; }
    void reset() { if (p_) p_->Release(); p_ = nullptr; }
};
bool Check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOGE("d3d11: %s failed (0x%08X)", what, unsigned(hr));
    return false;
}
std::string AdapterName(const wchar_t* description) {
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
    return name;
}
Com<IDXGIAdapter1> SelectAdapter() {
    Com<IDXGIAdapter1> selected;
    Adapters().clear();
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put())))) return selected;
    const auto& preferred = app::Get().gpu;
    for (UINT i = 0; ; ++i) {
        Com<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, adapter.put()))) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const std::string name = AdapterName(desc.Description);
        Adapters().push_back(name);
        if (!selected && name == preferred) selected = std::move(adapter);
    }
    if (!preferred.empty() && !selected) LOGR("d3d11: selected adapter unavailable, using the default adapter");
    return selected;
}
D3D11_BLEND Blend(uint32_t v, bool alpha = false) {
    switch (v) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND_OP BlendOp(uint32_t v) {
    switch (v) { case 0x800A: return D3D11_BLEND_OP_SUBTRACT; case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN; case 0x8008: return D3D11_BLEND_OP_MAX; default: return D3D11_BLEND_OP_ADD; }
}
D3D11_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) { case 0: return D3D11_STENCIL_OP_ZERO; case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT; case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT; case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR; default: return D3D11_STENCIL_OP_KEEP; }
}
DXGI_FORMAT HostFormat(Host h) {
    switch (h) { case Host::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Host::BC1: return DXGI_FORMAT_BC1_UNORM; case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
    case Host::BC3: return DXGI_FORMAT_BC3_UNORM; case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
    case Host::R16Unorm: return DXGI_FORMAT_R16_UNORM; default: return DXGI_FORMAT_UNKNOWN; }
}
uint32_t InputBytes(uint32_t format) {
    switch(DXGI_FORMAT(format)) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:return 12;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:return 8;
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM:return 4;
    case DXGI_FORMAT_R16_SNORM:case DXGI_FORMAT_R16_SINT:case DXGI_FORMAT_R8G8_UNORM:return 2;
    case DXGI_FORMAT_R8_UNORM:return 1;
    default:return 0;
    }
}
D3D11_BLEND_DESC CopyBlendDesc(uint8_t mask) {
    D3D11_BLEND_DESC d{};
    for(auto& r:d.RenderTarget) {
        r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;
        r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ZERO;
        r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        r.RenderTargetWriteMask=mask;
    }
    return d;
}
const char* kBlit = R"(
Texture2D picture : register(t0); SamplerState samp : register(s0);
cbuffer Fade : register(b0) { float fade; float gamma; float brightness; float spare; };
struct Out { float4 p:SV_Position; float2 uv:TEXCOORD0; };
Out vs(uint id:SV_VertexID) { Out o; o.uv=float2((id<<1)&2,id&2); o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }
float4 ps(Out i):SV_Target { float3 c=picture.Sample(samp,i.uv).rgb;
 return float4(saturate(pow(saturate(c), gamma)*fade+brightness),1); }
)";
const char* kClearVs = R"(
cbuffer Clear : register(b0) { float4 color; float z; };
float4 main(uint id:SV_VertexID):SV_Position { float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),z,1); }
)";
const char* kClearPs = R"(
cbuffer Clear : register(b0) { float4 color; float z; };
float4 main():SV_Target {return color;}
)";

struct Image {
    enum class Newest { Both, Single, Multisampled };
    Com<ID3D11Resource> resource;
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    Com<ID3D11ShaderResourceView> stencil;
    uint32_t w=0, h=0, data=0;
    uint32_t samples = 1;
    std::unique_ptr<Image> ms;
    Newest newest = Newest::Both;
    GuestExtent guest{};  // Xbox dimensions, independent of the allocated texture
    uint64_t epoch=0, signature=0, frame=0, palette_hash=0;
    uint64_t fur_frame = UINT64_MAX;
    bool depth=false, drawn=false;
    Surface surface{};
};

D3D11_RECT RenderRect(const Image& im, GuestRect rect) {
    auto edge = [](int32_t x, uint32_t guest, uint32_t render) {
        const uint32_t bounded = uint32_t(std::clamp(x, 0, int32_t(guest)));
        return LONG((uint64_t(bounded) * render + guest / 2) / guest);
    };
    return {edge(rect.left, im.guest.w, im.w), edge(rect.top, im.guest.h, im.h),
            edge(rect.right, im.guest.w, im.w), edge(rect.bottom, im.guest.h, im.h)};
}

D3D11_RECT WholeImage(const Image& im) {
    return {0, 0, LONG(im.w), LONG(im.h)};
}

bool Empty(const D3D11_RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

struct Shader {
    uint64_t hash=0;
    std::string source;
    Com<ID3DBlob> code;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11GeometryShader> gs;
    Com<ID3D11HullShader> hs;
    Com<ID3D11DomainShader> ds;
};
struct FieldTexture {
    uint64_t version = 0;
    uint32_t width = 0, height = 0;
    Com<ID3D11Texture2D> texture;
    Com<ID3D11ShaderResourceView> view;
};
enum class DepthPass { Original, Color, Depth };
struct Pipeline {
    bool input_needed=false;
    Com<ID3D11InputLayout> input;
    Com<ID3D11BlendState> blend;
    Com<ID3D11DepthStencilState> depth;
    Com<ID3D11RasterizerState> raster;
};
struct SampleQuery {
    Com<ID3D11Query> query;
    double guest_per_sample = 1;
};
using Query = std::shared_ptr<SampleQuery>;
struct Report { uint32_t address; std::vector<Query> queries; };
struct ShaderRecord {uint64_t key, checksum;uint32_t bytes, reserved;};
static_assert(sizeof(ShaderRecord)==24);
constexpr uint64_t kShaderCacheMagic=0x3145484341433344ull; // D3CACHE1, source/profile keyed FXC bytecode
constexpr size_t kPassConstantFloats = 32;  // a full-screen pass's constant buffer: 8 float4, kMotionBlurHlsl's

class D3D11Backend final : public Backend, Targets<D3D11Backend, Image>, PostEffects<D3D11Backend, Image, ID3D11ShaderResourceView*> {
    friend class Targets<D3D11Backend, Image>;
    friend class PostEffects<D3D11Backend, Image, ID3D11ShaderResourceView*>;
    static constexpr const char* kName = "d3d11";
    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<IDXGISwapChain> swap_;
    Com<ID3D11RenderTargetView> screen_;
    bool hdr_active_ = false, hdr_supported_ = false, hdr_failed_ = false;
    std::chrono::steady_clock::time_point output_checked_{};
    Shader hdr_encode_ps_, hdr_mask_ps_;
    Image hdr_scene_, hdr_ui_, hdr_world_, hdr_mask_;
    uint64_t hdr_world_key_ = 0;
    std::array<Image,4> generated_masks_;
    Com<ID3D11Buffer> vc_, dc_, fade_, pass_constants_;
    Com<ID3D11Buffer> vertices_[16], indices_;
    uint32_t vertex_bytes_[16]{}, index_bytes_=0;
    uint32_t vertex_next_[16]{},index_next_=0;
    double driver_timing_[9]{};
    uint32_t driver_frames_=0;
    PerfClock::time_point driver_start_=PerfClock::now();
    std::unordered_map<uint64_t, Image> textures_;
    std::unordered_map<uint64_t, Shader> shaders_;
    std::unordered_map<uint64_t, Shader> vertex_shaders_, pixel_shaders_;
    std::unordered_map<uint32_t, std::pair<Shader, Shader>> tessellation_;
    std::unordered_map<ShadingField (*)(), FieldTexture> fields_;
    std::unordered_map<uint64_t,std::vector<uint8_t>> shader_bytecode_;
    std::filesystem::path shader_cache_path_;
    bool shader_cache_valid_=false;
    std::array<std::array<float,4>,192> last_vsc_{};
    DrawConstants last_dc_{};
    bool last_vsc_valid_=false,last_dc_valid_=false;
    bool draw_state_valid_=false;
    ID3D11VertexShader* bound_vs_=nullptr;
    ID3D11PixelShader* bound_ps_=nullptr;
    ID3D11GeometryShader* bound_gs_=nullptr;
    ID3D11HullShader* bound_hs_=nullptr;
    ID3D11DomainShader* bound_domain_=nullptr;
    ID3D11InputLayout* bound_input_=nullptr;
    ID3D11BlendState* bound_blend_=nullptr;
    ID3D11DepthStencilState* bound_depth_=nullptr;
    ID3D11RasterizerState* bound_raster_=nullptr;
    ID3D11RenderTargetView* bound_rt_=nullptr;
    ID3D11DepthStencilView* bound_ds_=nullptr;
    std::array<ID3D11ShaderResourceView*,4> bound_views_{};
    std::array<ID3D11SamplerState*,4> bound_samplers_{};
    ID3D11Buffer* bound_vertices_[16]{};
    uint32_t bound_strides_[16]{},bound_offsets_[16]{};
    ID3D11Buffer* bound_indices_=nullptr;
    DXGI_FORMAT bound_index_format_=DXGI_FORMAT_UNKNOWN;
    uint32_t bound_index_offset_=0;
    D3D11_PRIMITIVE_TOPOLOGY bound_topology_=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT bound_viewport_{};
    D3D11_RECT bound_scissor_{};
    float bound_blend_factor_[4]{};
    uint32_t bound_stencil_ref_=0;
    std::unordered_map<uint64_t, Pipeline> pipelines_;
    std::unordered_map<uint64_t,bool> traced_effects_;
    bool effect_trace_active_=false;
    uint32_t effect_capture_=0;
    uint32_t point_debug_count_=0,trace_point_material_=0,trace_color_material_=0;
    Image point_debug_image_;
    std::unordered_map<uint64_t, Com<ID3D11SamplerState>> samplers_;
    Shader blit_vs_, blit_ps_;
    Shader copy_ps_, copy_depth_ps_, copy_stencil_ps_;
    Shader resolve_depth_ps_, resolve_stencil_ps_;
    Shader depth_bytes_ps_, depth_bytes_ms_ps_;
    std::map<std::pair<const char*, std::string_view>, Shader> pass_shaders_;  // PostEffects' passes
    HudSplit hud_;  // g_hud_split: the HUD's elements, and the draw's remap (its viewport and scissor)
    Com<ID3D11ShaderResourceView> smaa_area_, smaa_search_;
    std::array<Image, 4> generated_;
    uint32_t taken_ = 0, generated_count_ = 0, next_picture_ = 0;
    int last_picture_ = -1;
    bool replaying_ = false, keeping_pictures_ = false;
    std::chrono::steady_clock::time_point schedule_{};
    std::chrono::steady_clock::duration picture_interval_{};
    Com<ID3D11SamplerState> blit_sampler_, copy_sampler_;
    Com<ID3D11BlendState> blit_blend_;
    Com<ID3D11DepthStencilState> blit_depth_;
    Com<ID3D11RasterizerState> blit_raster_;
    Com<ID3D11BlendState> clear_blend_[2];
    Com<ID3D11DepthStencilState> clear_depth_[4];
    Com<ID3D11DepthStencilState> copy_depth_, copy_stencil_[8];
    std::vector<Query> counted_;
    std::vector<Report> reports_;
    uint64_t shown_=0;
    uint32_t ww_=0, wh_=0;
    bool ui_=false;
    std::chrono::steady_clock::time_point last_{};
    std::chrono::steady_clock::time_point fps_since_{};
    uint32_t fps_pictures_ = 0;
    float fps_=0;
    State sprite_;
    Assembled assembled_;
    std::array<std::vector<uint8_t>,16> color_buffers_;
    void LoadShaderCache() {
        shader_cache_path_=app::CacheDir()/"d3d11-shaders-v1.bin";
        FILE* f=OpenFile(shader_cache_path_,"rb");if(!f)return;
        std::fseek(f,0,SEEK_END);long length=std::ftell(f);std::rewind(f);
        if(length<8 || length>128*1024*1024){std::fclose(f);return;}
        uint64_t magic=0;if(std::fread(&magic,8,1,f)!=1 || magic!=kShaderCacheMagic){std::fclose(f);return;}
        shader_cache_valid_=true;
        for(;;) {
            ShaderRecord r{};size_t got=std::fread(&r,1,sizeof(r),f);
            if(!got && std::feof(f))break;
            if(got!=sizeof(r) || !r.bytes || r.bytes>8*1024*1024 || r.reserved){shader_cache_valid_=false;break;}
            std::vector<uint8_t> bytes(r.bytes);
            if(std::fread(bytes.data(),1,bytes.size(),f)!=bytes.size() || Hash(bytes.data(),bytes.size())!=r.checksum){shader_cache_valid_=false;break;}
            shader_bytecode_[r.key]=std::move(bytes);
        }
        std::fclose(f);
        LOGR("d3d11: loaded %zu cached shader programs",shader_bytecode_.size());
    }
    void SaveShader(const Shader& shader) {
        const auto* data=static_cast<const uint8_t*>(shader.code->GetBufferPointer());size_t size=shader.code->GetBufferSize();
        shader_bytecode_[shader.hash]=std::vector<uint8_t>(data,data+size);
        FILE* f=OpenFile(shader_cache_path_,shader_cache_valid_?"ab":"wb");if(!f)return;
        bool ok=true;
        if(!shader_cache_valid_) {
            ok=std::fwrite(&kShaderCacheMagic,8,1,f)==1;
            // A partial old cache is replaced with all its validated records plus this program.
            for(const auto& [key,bytes]:shader_bytecode_) {
                ShaderRecord r{key,Hash(bytes.data(),bytes.size()),uint32_t(bytes.size()),0};
                ok=ok && std::fwrite(&r,sizeof(r),1,f)==1 && std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
            }
        } else {
            ShaderRecord r{shader.hash,Hash(data,size),uint32_t(size),0};
            ok=std::fwrite(&r,sizeof(r),1,f)==1 && std::fwrite(data,1,size,f)==size;
        }
        ok=std::fclose(f)==0 && ok;shader_cache_valid_=ok;
    }
    Shader Compile(const std::string& source, const char* entry, const char* profile) {
        PerfScope timing{g_perf.compile};
        Shader out;
        out.hash=SourceHash(source,entry,profile);
        if(profile[0]=='v')out.source=source;
        Com<ID3DBlob> error;
        HRESULT hr=E_FAIL;bool hit=false;
        if(auto it=shader_bytecode_.find(out.hash);it!=shader_bytecode_.end()) {
            hr=D3DCreateBlob(it->second.size(),out.code.put());
            if(SUCCEEDED(hr)){std::memcpy(out.code->GetBufferPointer(),it->second.data(),it->second.size());hit=true;}
        }
        if(!hit) {
            g_perf.shaders++;auto start=std::chrono::steady_clock::now();
            hr=D3DCompile(source.data(), source.size(), "conker-d3d11", nullptr, nullptr, entry, profile,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.code.put(), error.put());
            double ms=SinceMs(start);
            if(ms>=100)LOGR("d3d11: compiling %s took %.0f ms (saved for next launch)",profile,ms);
        }
        if (FAILED(hr)) {
            LOGE("d3d11: shader %s/%s: %s", entry, profile, error ? static_cast<const char*>(error->GetBufferPointer()) : "compile failed");
            return out;
        }
        if(!hit)SaveShader(out);
        if (profile[0]=='v') Check(dev_->CreateVertexShader(out.code->GetBufferPointer(),out.code->GetBufferSize(),nullptr,out.vs.put()),"vertex shader");
        if (profile[0]=='p') Check(dev_->CreatePixelShader(out.code->GetBufferPointer(),out.code->GetBufferSize(),nullptr,out.ps.put()),"pixel shader");
        if (profile[0]=='g') Check(dev_->CreateGeometryShader(out.code->GetBufferPointer(),out.code->GetBufferSize(),nullptr,out.gs.put()),"geometry shader");
        if (profile[0]=='h') Check(dev_->CreateHullShader(out.code->GetBufferPointer(),out.code->GetBufferSize(),nullptr,out.hs.put()),"hull shader");
        if (profile[0]=='d') Check(dev_->CreateDomainShader(out.code->GetBufferPointer(),out.code->GetBufferSize(),nullptr,out.ds.put()),"domain shader");
        return out;
    }
    Shader& CachedShader(const std::string& source, const char* profile) {
        uint64_t key=SourceHash(source,"main",profile);
        auto [it, added]=shaders_.try_emplace(key);
        if (added) it->second=Compile(source,"main",profile);
        return it->second;
    }
    bool Buffer(Com<ID3D11Buffer>& b, uint32_t size, uint32_t bind, const void* data=nullptr) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth=(size+15)&~15u; d.BindFlags=bind;
        d.Usage=D3D11_USAGE_DYNAMIC; d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (!Check(dev_->CreateBuffer(&d,nullptr,b.put()),"buffer")) return false;
        return !data || Upload(b.get(),data,size);
    }
    bool Upload(ID3D11Buffer* b,const void* data,size_t size) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!Check(ctx_->Map(b,0,D3D11_MAP_WRITE_DISCARD,0,&m),"map buffer")) return false;
        std::memcpy(m.pData,data,size); ctx_->Unmap(b,0); return true;
    }
    bool Stream(Com<ID3D11Buffer>& buffer,uint32_t& capacity,uint32_t& next,
                uint32_t bind,const void* data,uint32_t bytes,UINT& offset) {
        // Append into unused space. DISCARD only at wrap/growth lets the driver
        // rename a large backing buffer rather than one allocation per draw.
        if(bytes>capacity) {
            uint32_t wanted=std::max(1024u*1024u,(bytes+65535u)&~65535u);
            if(!Buffer(buffer,wanted,bind))return false;
            capacity=wanted;next=0;
        }
        if(uint64_t(next)+bytes>capacity)next=0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        D3D11_MAP mode=next?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD;
        if(!Check(ctx_->Map(buffer.get(),0,mode,0,&mapped),"map vertex/index stream"))return false;
        std::memcpy(static_cast<uint8_t*>(mapped.pData)+next,data,bytes);
        ctx_->Unmap(buffer.get(),0);offset=next;next=(next+bytes+15u)&~15u;
        g_perf.upload+=bytes;return true;
    }
    void Unbind() {
        draw_state_valid_=false;
        ID3D11ShaderResourceView* empty[8]{};
        ctx_->PSSetShaderResources(0,8,empty); ctx_->VSSetShaderResources(0,8,empty);
        if (bound_hs_) {
            ctx_->HSSetShader(nullptr,nullptr,0);
            ctx_->DSSetShader(nullptr,nullptr,0);
            ctx_->DSSetShaderResources(0,8,empty);
            bound_hs_ = nullptr;
            bound_domain_ = nullptr;
        }
        ctx_->OMSetRenderTargets(0,nullptr,nullptr);
    }
    ID3D11ShaderResourceView* GetField(ShadingField (*read)()) {
        if (!read) return nullptr;
        const auto data = read();
        if (!data.data || !data.w || !data.h) return nullptr;
        auto& field = fields_[read];
        if (!field.view || field.width != data.w || field.height != data.h) {
            field = {};
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = data.w;
            desc.Height = data.h;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA initial{data.data, data.w * UINT(sizeof(float)), 0};
            if (!Check(dev_->CreateTexture2D(&desc,&initial,field.texture.put()),"material field") ||
                !Check(dev_->CreateShaderResourceView(field.texture.get(),nullptr,field.view.put()),"material field view"))
                return nullptr;
            field.width = data.w;
            field.height = data.h;
        } else if (field.version != data.version) {
            ctx_->UpdateSubresource(field.texture.get(),0,nullptr,data.data,data.w * UINT(sizeof(float)),0);
        }
        field.version = data.version;
        return field.view.get();
    }
    static uint32_t RenderSize(float size) {
        return uint32_t(std::clamp(std::lround(size), 1L,
                                   long(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)));
    }
    bool CreateTargetImage(Image& im) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=im.w; d.Height=im.h; d.MipLevels=1; d.ArraySize=1;
        d.SampleDesc.Count = im.samples;
        d.Format=im.depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE | (im.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
        Com<ID3D11Texture2D> tex;
        if (!Check(dev_->CreateTexture2D(&d,nullptr,tex.put()),"render target")) return false;
        tex->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(im.resource.put()));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Format=im.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : d.Format; sd.Texture2D.MipLevels=1;
        if (im.samples > 1) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        if (!Check(dev_->CreateShaderResourceView(tex.get(),&sd,im.srv.put()),"target SRV")) return false;
        if (im.depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            if (im.samples > 1) dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
            if (!Check(dev_->CreateDepthStencilView(tex.get(),&dd,im.dsv.put()),"depth view")) return false;
            sd.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            if (!Check(dev_->CreateShaderResourceView(tex.get(),&sd,im.stencil.put()),"stencil SRV")) return false;
            ctx_->ClearDepthStencilView(im.dsv.get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        } else {
            if (!Check(dev_->CreateRenderTargetView(tex.get(),nullptr,im.rtv.put()),"color view")) return false;
        }
        return true;
    }
    Image* SnapshotResource(Image& source) {
        Unbind();
        auto& copy=feedback_[ResourceKey(source.data,source.depth) | (uint64_t(source.samples) << 40)];
        if(!copy.resource || copy.w!=source.w || copy.h!=source.h || copy.samples!=source.samples) {
            copy=Image{};copy.w=source.w;copy.h=source.h;copy.depth=source.depth;copy.data=source.data;
            copy.guest=source.guest;
            copy.samples=source.samples;
            D3D11_TEXTURE2D_DESC d{};d.Width=copy.w;d.Height=copy.h;d.MipLevels=d.ArraySize=1;
            d.SampleDesc.Count=copy.samples;
            d.Format=source.depth?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            Com<ID3D11Texture2D> tex;
            if(!Check(dev_->CreateTexture2D(&d,nullptr,tex.put()),"feedback texture"))return nullptr;
            tex->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(copy.resource.put()));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=source.depth?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:d.Format;
            sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            if (copy.samples > 1) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            if(!Check(dev_->CreateShaderResourceView(tex.get(),&sd,copy.srv.put()),"feedback SRV"))return nullptr;
            if(source.depth) {
                sd.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                if(!Check(dev_->CreateShaderResourceView(tex.get(),&sd,copy.stencil.put()),"feedback stencil SRV"))return nullptr;
            }
        }
        ctx_->CopyResource(copy.resource.get(),source.resource.get());return &copy;
    }
    Image* DepthBytes(Image& source,uint32_t format, Image* furred = nullptr) {
        uint64_t key=uint64_t(source.data)<<32|format;auto& out=depth_bytes_[key];
        if(!out.resource || out.w!=source.w || out.h!=source.h) {
            out=Image{};out.w=source.w;out.h=source.h;out.guest=source.guest;
            D3D11_TEXTURE2D_DESC d{};d.Width=out.w;d.Height=out.h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Com<ID3D11Texture2D> tex;
            if(!Check(dev_->CreateTexture2D(&d,nullptr,tex.put()),"depth bytes texture"))return nullptr;
            tex->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(out.resource.put()));
            if(!Check(dev_->CreateShaderResourceView(tex.get(),nullptr,out.srv.put()),"depth bytes SRV") ||
               !Check(dev_->CreateRenderTargetView(tex.get(),nullptr,out.rtv.put()),"depth bytes RTV"))return nullptr;
        }
        // HD 4000 can return zero stencil from an attachment even when a CPU
        // readback contains the correct bits. Copying to an SRV-only resource
        // resolves that storage before sampling, entirely on the GPU.
        const bool multisampled = source.ms && source.newest == Image::Newest::Multisampled;
        Image* sampled = SnapshotResource(multisampled ? *source.ms : source);
        if (!sampled) return nullptr;
        if (furred && !Resolve(*furred)) furred = nullptr;
        static const std::string single = std::string("#define DEPTH_STENCIL_GREEN\n") + kDepthBytesHlsl;
        static const std::string multi = std::string("#define DEPTH_MULTISAMPLED\n") + kDepthBytesHlsl;
        float constants[8]{};
        DepthByteOrder(format,constants);
        constants[7] = g_outlines.load(std::memory_order_relaxed) ? 1.0f : 0.0f;  // the marks left out
        if (furred) {
            constants[4] = 1;
            constants[5] = float(furred->w) / source.w;
            constants[6] = float(source.w) / source.guest.w;
        }
        const D3D11_VIEWPORT viewport{0,0,float(out.w),float(out.h),0,1};
        if (!FullScreenPass(multisampled?depth_bytes_ms_ps_:depth_bytes_ps_,
                multisampled?multi.c_str():single.c_str(),"psmain",out.rtv.get(),viewport,
                {sampled->srv.get(),sampled->stencil.get(),furred?furred->srv.get():nullptr},constants))
            return nullptr;
        return &out;
    }
    void BeginTransfer(ID3D11PixelShader* ps, const D3D11_VIEWPORT& viewport,
                       ID3D11RenderTargetView* color, ID3D11DepthStencilView* depth) {
        Unbind();
        ctx_->OMSetRenderTargets(color ? 1 : 0, color ? &color : nullptr, depth);
        ctx_->RSSetViewports(1, &viewport);
        ctx_->RSSetState(blit_raster_.get());
        ctx_->OMSetBlendState(blit_blend_.get(), nullptr, ~0u);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(blit_vs_.vs.get(), nullptr, 0);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->GSSetShader(nullptr, nullptr, 0);
        auto* cb = fade_.get();
        ctx_->PSSetConstantBuffers(0, 1, &cb);
    }
    void BeginTransfer(ID3D11PixelShader* ps, const D3D11_RECT& rect,
                       ID3D11RenderTargetView* color, ID3D11DepthStencilView* depth) {
        const D3D11_VIEWPORT viewport{float(rect.left), float(rect.top),
                                     float(rect.right - rect.left), float(rect.bottom - rect.top), 0, 1};
        BeginTransfer(ps, viewport, color, depth);
    }
    bool BlitColor(const Image& source, const D3D11_RECT& from, Image& destination,
                   const D3D11_RECT& to, ID3D11SamplerState* sampler) {
        const float uv[] = {float(from.left) / source.w, float(from.top) / source.h,
                            float(from.right - from.left) / source.w, float(from.bottom - from.top) / source.h};
        if (!Upload(fade_.get(), uv, sizeof(uv))) return false;
        BeginTransfer(copy_ps_.ps.get(), to, destination.rtv.get(), nullptr);
        ctx_->OMSetDepthStencilState(blit_depth_.get(), 0);
        auto* view = source.srv.get();
        ctx_->PSSetShaderResources(0, 1, &view);
        ctx_->PSSetSamplers(0, 1, &sampler);
        ctx_->Draw(3, 0);
        Unbind();
        return true;
    }
    bool PreserveDepth(Image& source, Image& destination) {
        Shader* depth = &copy_depth_ps_;
        Shader* stencil = &copy_stencil_ps_;
        if (source.samples > 1) {
            static const std::string hlsl = std::string("#define COPY_MULTISAMPLED\n") + d3d11::kCopyDepthStencil;
            if (!resolve_depth_ps_.hash) resolve_depth_ps_ = Compile(hlsl, "copy_depth", "ps_5_0");
            if (!resolve_stencil_ps_.hash) resolve_stencil_ps_ = Compile(hlsl, "copy_stencil", "ps_5_0");
            depth = &resolve_depth_ps_;
            stencil = &resolve_stencil_ps_;
            if (!depth->ps || !stencil->ps) return false;
        }
        // Stencil returned by HD 4000 is reliable from the SRV-only snapshot.
        auto* sampled = SnapshotResource(source);
        if (!sampled) return false;
        ID3D11ShaderResourceView* views[] = {sampled->srv.get(), sampled->stencil.get()};
        ctx_->ClearDepthStencilView(destination.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        BeginTransfer(depth->ps.get(), WholeImage(destination), nullptr, destination.dsv.get());
        ctx_->PSSetShaderResources(0, 2, views);
        ctx_->OMSetDepthStencilState(copy_depth_.get(), 0);
        ctx_->Draw(3, 0);

        // FL11_0 has no shader stencil export. Copy one bit per pass into the cleared stencil.
        ctx_->PSSetShader(stencil->ps.get(), nullptr, 0);
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t constants[4] = {1u << i};
            if (!Upload(fade_.get(), constants, sizeof(constants))) {
                Unbind();
                return false;
            }
            ctx_->OMSetDepthStencilState(copy_stencil_[i].get(), 255);
            ctx_->Draw(3, 0);
        }
        Unbind();
        return true;
    }
    bool InitTransfers() {
        copy_ps_ = Compile(d3d11::kCopyColor, "main", "ps_5_0");
        copy_depth_ps_ = Compile(d3d11::kCopyDepthStencil, "copy_depth", "ps_5_0");
        copy_stencil_ps_ = Compile(d3d11::kCopyDepthStencil, "copy_stencil", "ps_5_0");
        if (!copy_ps_.ps || !copy_depth_ps_.ps || !copy_stencil_ps_.ps) return false;

        D3D11_SAMPLER_DESC sampler{};
        blit_sampler_->GetDesc(&sampler);
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (!Check(dev_->CreateSamplerState(&sampler, copy_sampler_.put()), "surface copy sampler")) return false;

        D3D11_DEPTH_STENCIL_DESC state{};
        state.DepthEnable = TRUE;
        state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        state.DepthFunc = D3D11_COMPARISON_ALWAYS;
        state.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                           D3D11_COMPARISON_ALWAYS};
        state.BackFace = state.FrontFace;
        if (!Check(dev_->CreateDepthStencilState(&state, copy_depth_.put()), "depth copy state")) return false;
        state.DepthEnable = FALSE;
        state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        state.StencilEnable = TRUE;
        state.StencilReadMask = 255;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            state.StencilWriteMask = UINT8(1u << bit);
            if (!Check(dev_->CreateDepthStencilState(&state, copy_stencil_[bit].put()), "stencil copy state"))
                return false;
        }
        return true;
    }
    D3D11_VIEWPORT PictureViewport() const {
        const float aspect = app::FrameAspect();
        const float width = std::min(float(ww_), wh_ * aspect);
        const float height = width / aspect;
        return {(ww_ - width) / 2, (wh_ - height) / 2, width, height, 0, 1};
    }
    bool FullScreenPass(Shader& shader, const char* source, const char* entry,
                        ID3D11RenderTargetView* destination, const D3D11_VIEWPORT& viewport,
                        std::initializer_list<ID3D11ShaderResourceView*> views,
                        std::span<const float> constants) {
        if (!shader.hash) shader = Compile(source, entry, "ps_5_0");
        // the whole buffer written: a discarded map's bytes past the constants are undefined, and the shaders read
        // fields some passes leave out as zero (kHdrHlsl's r, kFsrHlsl's con2)
        float padded[kPassConstantFloats]{};
        if (constants.size() > kPassConstantFloats) return false;
        std::copy(constants.begin(), constants.end(), padded);
        if (!shader.ps || !Upload(pass_constants_.get(), padded, sizeof(padded))) return false;
        BeginTransfer(shader.ps.get(), viewport, destination, nullptr);
        auto* cb = pass_constants_.get();
        ctx_->PSSetConstantBuffers(1, 1, &cb);
        ID3D11SamplerState* samplers[] = {blit_sampler_.get(), copy_sampler_.get()};
        ctx_->PSSetSamplers(0, 2, samplers);
        ctx_->PSSetShaderResources(0, UINT(views.size()), views.begin());
        ctx_->OMSetDepthStencilState(blit_depth_.get(), 0);
        ctx_->Draw(3, 0);
        Unbind();
        return true;
    }
    bool SmaaLookup(Com<ID3D11ShaderResourceView>& view, DXGI_FORMAT format, uint32_t width,
                    uint32_t height, const uint8_t* pixels, uint32_t pitch) {
        if (view) return true;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
        Com<ID3D11Texture2D> texture;
        return Check(dev_->CreateTexture2D(&desc, &data, texture.put()), "SMAA lookup texture") &&
               Check(dev_->CreateShaderResourceView(texture.get(), nullptr, view.put()), "SMAA lookup view");
    }
    // what Targets asks of a backend, beside the images' own making and copying
    static bool HasTexture(const Image& image) { return bool(image.resource); }
    void Fill(const Image& image, const std::vector<uint8_t>& pixels, uint32_t pitch) {
        ctx_->UpdateSubresource(image.resource.get(), 0, nullptr, pixels.data(), pitch, 0);
    }
    bool BlitWhole(const Image& source, Image& destination, bool linear) {
        return BlitColor(source, WholeImage(source), destination, WholeImage(destination),
                         linear ? blit_sampler_.get() : copy_sampler_.get());
    }
    void ResolveColor(const Image& source, Image& destination) {
        Unbind();
        ctx_->ResolveSubresource(destination.resource.get(), 0, source.resource.get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    bool CopyPixels(const Image& source, GuestRect source_rect, Image& destination, GuestRect destination_rect) {
        const auto from = RenderRect(source, source_rect);
        const auto to = RenderRect(destination, destination_rect);
        if (Empty(from) || Empty(to)) return false;
        if (from.right - from.left != to.right - to.left || from.bottom - from.top != to.bottom - to.top)
            return BlitColor(source, from, destination, to, copy_sampler_.get());
        const D3D11_BOX box{UINT(from.left), UINT(from.top), 0, UINT(from.right), UINT(from.bottom), 1};
        ctx_->CopySubresourceRegion(destination.resource.get(), 0, UINT(to.left), UINT(to.top), 0,
                                    source.resource.get(), 0, &box);
        return true;
    }
    void BeforeResize() { Unbind(); }
    // what PostEffects asks of a backend
    static D3D11_VIEWPORT ViewportOf(uint32_t width, uint32_t height) {
        return {0, 0, float(width), float(height), 0, 1};
    }
    static std::pair<float, float> ViewportSize(const D3D11_VIEWPORT& viewport) {
        return {viewport.Width, viewport.Height};
    }
    static ID3D11ShaderResourceView* In(const Image& image) { return image.srv.get(); }
    static ID3D11ShaderResourceView* StencilIn(const Image& image) { return image.stencil.get(); }
    bool FullScreenPass(const char* source, const char* entry, ID3D11RenderTargetView* destination,
                        const D3D11_VIEWPORT& viewport, std::initializer_list<ID3D11ShaderResourceView*> views,
                        std::span<const float> constants) {
        return FullScreenPass(pass_shaders_[{source, entry}], source, entry, destination, viewport, views, constants);
    }
    bool FullScreenPass(const char* source, const char* entry, Image& destination, const D3D11_VIEWPORT& viewport,
                        std::initializer_list<ID3D11ShaderResourceView*> views, std::span<const float> constants) {
        return FullScreenPass(source, entry, destination.rtv.get(), viewport, views, constants);
    }
    bool SmaaLookups(ID3D11ShaderResourceView*& area, ID3D11ShaderResourceView*& search) {
        if (!SmaaLookup(smaa_area_, DXGI_FORMAT_R8G8_UNORM, kSmaaAreaW, kSmaaAreaH, kSmaaArea, kSmaaAreaW * 2) ||
            !SmaaLookup(smaa_search_, DXGI_FORMAT_R8_UNORM, kSmaaSearchW, kSmaaSearchH, kSmaaSearch, kSmaaSearchW))
            return false;
        area = smaa_area_.get();
        search = smaa_search_.get();
        return true;
    }
    void ClearColor(Image& image) {
        const float zero[4] = {};
        ctx_->ClearRenderTargetView(image.rtv.get(), zero);
    }
    Image* StencilSnapshot(Image& depth) {
        Image* z = Snapshot(depth);
        return z && z->stencil ? z : nullptr;
    }
    static const char* OutlineSource() {
        static const std::string source = std::string("#define STENCIL_GREEN\n") + kOutlineHlsl;
        return source.c_str();
    }
    bool HdrHighlights() const { return hdr_active_ && app::Get().hdr_highlights > 0; }
    void KeepHdrWorld(Image& target) {
        if (!HdrHighlights() || !Resolve(target) || !ScratchColor(hdr_world_,target.w,target.h)) return;
        Unbind();
        ctx_->CopyResource(hdr_world_.resource.get(),target.resource.get());
        hdr_world_key_ = ResourceKey(target.data,false);
    }
    bool MakeHdrMask(Image& target, Image& mask) {
        if (!HdrHighlights() || hdr_world_key_ != ResourceKey(target.data,false) ||
            hdr_world_.w != target.w || hdr_world_.h != target.h ||
            !ScratchColor(mask,target.w,target.h)) return false;
        const D3D11_VIEWPORT viewport{0,0,float(target.w),float(target.h),0,1};
        const float unused[4]{};
        return FullScreenPass(hdr_mask_ps_,kHdrHlsl,"mask",mask.rtv.get(),viewport,
                              {target.srv.get(),hdr_world_.srv.get()},unused);
    }
    Image* Texture(const State& s,int stage,TexInfo& info,bool& target,bool sampled) {
        PerfScope timing{g_perf.bind};
        TextureDesc d; info={}; target=false;
        if (!s.textures[stage] || !Describe(s.textures[stage],d)) return nullptr;
        const auto& fi=Format(d.format);
        info.kind=TextureKind(d); info.rect=d.linear;
        auto ti=targets_.find(ResourceKey(d.data,fi.depth));
        auto surface_format=[](uint32_t f){switch(f){case 0x02:case 0x03:case 0x05:case 0x06:case 0x07:
            case 0x10:case 0x11:case 0x12:case 0x1C:case 0x1E:return true;default:return false;}};
        if(ti!=targets_.end() && !fi.depth && frame_>ti->second.frame+8 &&
           (ti->second.guest.w!=d.width || ti->second.guest.h!=d.height || !surface_format(d.format))) {
            targets_.erase(ti);ti=targets_.end();
        }
        if (ti!=targets_.end() && !d.cube && d.depth==1 && !fi.compressed &&
            ti->second.guest.w==d.width && ti->second.guest.h==d.height) {
            info.shadow=fi.depth ? ShadowDepthKind(d.format) : 0;target=true;
            if(!sampled)return nullptr;
            CpuWrite(ti->second);
            return Resolve(ti->second) ? &ti->second : nullptr;
        }
        if(!fi.depth && !fi.compressed && fi.bits==32 && !d.cube && d.depth==1) {
            auto z=targets_.find(ResourceKey(d.data,true));
            if(z!=targets_.end() && z->second.guest.w==d.width && z->second.guest.h==d.height) {
                info.kind=kTex2D;info.flags=uint8_t(kTexDepthBytes|(DepthByteLayout(d.format)<<kTexDepthLayoutShift));
                auto color = targets_.find(ResourceKey(s.rt.data,false));
                Image* furred = color != targets_.end() && color->second.fur_frame == frame_ ? &color->second : nullptr;
                target=true;return sampled?DepthBytes(z->second,d.format,furred):nullptr;
            }
        }
        info.sign=OffsetSigned(d.format,s.tss[stage][kTssColorSign]);
        info.shadow=fi.depth ? ShadowDepthKind(d.format) : 0;
        if(!sampled)return nullptr;
        StagePalette pal; ReadStagePalette(s,stage,d,pal);
        const uint32_t layout[]={d.data,d.format,d.width,d.height,d.depth,d.levels,d.pitch,uint32_t(d.cube),uint32_t(d.linear)};
        uint64_t key=Hash(layout,sizeof(layout)); key=Hash(&pal.va,sizeof(pal.va),key);key=Hash(&info.sign,sizeof(info.sign),key);
        auto& im=textures_[key];uint64_t epoch=DirtyEpoch(d.data);
        if(im.resource && im.frame==frame_ && im.epoch==epoch && im.palette_hash==pal.hash) return &im;
        im.frame=frame_;
        auto check_start=PerfClock::now();
        uint32_t texture_bytes=TotalSize(d);
        uint64_t signature=MemSig(d.data,texture_bytes)^pal.hash;
        g_perf.sig+=PerfMs(check_start);g_perf.sigs++;g_perf.sig_bytes+=texture_bytes;
        if (im.resource && im.signature==signature && im.epoch==epoch) return &im;
        PerfScope uploading{g_perf.texup};g_perf.texups++;g_perf.texup_bytes+=texture_bytes;
        bool reuse=im.resource && im.srv;
        im.frame=frame_; im.signature=signature; im.epoch=DirtyEpoch(d.data); im.w=d.width; im.h=d.height;im.palette_hash=pal.hash;
        DXGI_FORMAT fmt=HostFormat(fi.host);
        if (fmt==DXGI_FORMAT_UNKNOWN) return nullptr;
        // D3D11 requires the top BC mip to span complete 4x4 blocks. Xbox/Vulkan
        // also accept 1x1 and 2x2 BC textures; decode those without changing their UVs.
        bool expand=fi.compressed && (d.width<4 || d.height<4 || d.depth>1);
        if(expand)fmt=DXGI_FORMAT_B8G8R8A8_UNORM;
        uint32_t levels=std::min(d.levels,uint32_t(std::bit_width(std::max({d.width,d.height,d.depth}))));
        std::vector<std::vector<uint8_t>> bytes(levels*(d.cube?6:1));
        std::vector<D3D11_SUBRESOURCE_DATA> sub(bytes.size());
        const auto* guest=static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        for (uint32_t face=0;face<(d.cube?6u:1u);face++) for(uint32_t level=0;level<levels;level++) {
            uint32_t n=face*levels+level,pitch=0;
            DecodeLevel(d,level,face,guest,pal.va?pal.colors:nullptr,bytes[n],pitch);
            if (bytes[n].empty()) return nullptr;
            if(expand) {
                uint32_t w=std::max(d.width>>level,1u),h=std::max(d.height>>level,1u),slices=std::max(d.depth>>level,1u);
                size_t block_slice=size_t(pitch)*std::max((h+3)/4,1u);std::vector<uint8_t> all,slice;
                for(uint32_t z=0;z<slices;z++){DecodeBlocks(fi.host,bytes[n].data()+z*block_slice,w,h,slice);AppendBytes(all,slice.data(),slice.size());}
                bytes[n]=std::move(all);pitch=w*4;
            }
            if(info.sign) {uint8_t mask[4]={uint8_t(info.sign&4?128:0),uint8_t(info.sign&2?128:0),uint8_t(info.sign&1?128:0),uint8_t(info.sign&8?128:0)};
                for(size_t i=0;i<bytes[n].size();i++)bytes[n][i]^=mask[i&3];}
            sub[n]={bytes[n].data(),pitch,uint32_t(bytes[n].size()/std::max(1u,d.depth>>level))};
        }
        if(reuse) {
            for(uint32_t n=0;n<sub.size();n++)ctx_->UpdateSubresource(im.resource.get(),n,nullptr,sub[n].pSysMem,sub[n].SysMemPitch,sub[n].SysMemSlicePitch);
            return &im;
        }
        if (d.depth>1 && !d.cube) {
            D3D11_TEXTURE3D_DESC td{}; td.Width=d.width; td.Height=d.height; td.Depth=d.depth; td.MipLevels=levels;
            td.Format=fmt; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            Com<ID3D11Texture3D> tex;
            if (!Check(dev_->CreateTexture3D(&td,sub.data(),tex.put()),"volume texture")) return nullptr;
            tex->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(im.resource.put()));
        } else {
            D3D11_TEXTURE2D_DESC td{}; td.Width=d.width; td.Height=d.height; td.MipLevels=levels;
            td.ArraySize=d.cube?6:1; td.Format=fmt; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT;
            td.BindFlags=D3D11_BIND_SHADER_RESOURCE; td.MiscFlags=d.cube?D3D11_RESOURCE_MISC_TEXTURECUBE:0;
            Com<ID3D11Texture2D> tex;
            HRESULT hr=dev_->CreateTexture2D(&td,sub.data(),tex.put());
            if (!Check(hr,"texture")) {LOG_FIRST(8,"d3d11: texture %08X Xbox %X DXGI %u %ux%u levels %u cube %u",d.data,d.format,unsigned(fmt),d.width,d.height,levels,unsigned(d.cube));return nullptr;}
            tex->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(im.resource.put()));
        }
        if (!Check(dev_->CreateShaderResourceView(im.resource.get(),nullptr,im.srv.put()),"texture SRV")) return nullptr;
        return &im;
    }
    ID3D11SamplerState* Sampler(const State& s,int stage,bool shadow,bool clamp=false) {
        const auto& t=s.tss[stage]; D3D11_SAMPLER_DESC d{};
        bool min=t[kTssMinFilter]>=2,mag=t[kTssMagFilter]>=2,mip=t[kTssMipFilter]>=2;
        d.Filter=D3D11_FILTER((min?0x10:0)|(mag?4:0)|(mip?1:0)|(shadow?0x80:0));
        uint32_t aniso=app::Get().aniso ? app::Get().aniso : t[kTssMaxAnisotropy];
        d.MaxAnisotropy=std::clamp(aniso,1u,16u);
        if (aniso>1 && t[kTssMinFilter]==3 && !shadow) d.Filter=D3D11_FILTER_ANISOTROPIC;
        auto address=[](uint32_t a){ return D3D11_TEXTURE_ADDRESS_MODE(a==5?3:std::clamp(a,1u,4u)); };
        auto unit=[&](uint32_t mode){return clamp && (mode==1 || mode==4)?3u:mode;};
        d.AddressU=address(unit(t[kTssAddressU])); d.AddressV=address(unit(t[kTssAddressV])); d.AddressW=address(t[kTssAddressW]);
        d.MipLODBias=std::clamp(Bits(t[kTssMipLodBias]),-16.0f,15.99f);
        d.MinLOD=float(t[kTssMaxMipLevel]); d.MaxLOD=t[kTssMipFilter]?D3D11_FLOAT32_MAX:0;
        d.MaxLOD=std::max(d.MaxLOD,d.MinLOD); d.ComparisonFunc=D3D11_COMPARISON_FUNC(CmpIndex(s.rs[kRsShadowFunc])+1);
        Color(t[kTssBorderColor],d.BorderColor);
        auto& sampler=samplers_[Hash(&d,sizeof(d))];
        if (!sampler) Check(dev_->CreateSamplerState(&d,sampler.put()),"sampler");
        return sampler.get();
    }
    void TraceEffect(const State& s,const Assembled& a,uint32_t prim,uint32_t count,
                     const Shader& vs,const Shader& ps,const PixelKey& pk,const DrawConstants& c,bool clamp,
                     Image* const images[4],const bool target[4]) {
        static const wchar_t* destination=diag::WEnv(L"XBE_D3D11_EFFECT_TRACE");
        if(!destination || !effect_trace_active_ || traced_effects_.size()>=512)return;
        PsoKey key{};key.vs=vs.hash;key.ps=ps.hash;key.topo=uint32_t(a.topology);
        KeyBlend(s,key);KeyDepthStencil(s,a,key);KeyRasterizer(s,a,key);
        uint64_t id=Hash(&key,sizeof(key));
        // Inline texture headers can be transient. Identify the material by
        // the sampled image, avoiding repeated dumps of the same pixels.
        for(int stage=0;stage<4;stage++)if((SampledStages(s)>>stage)&1) {
            TextureDesc d;if(!s.textures[stage] || !Describe(s.textures[stage],d))continue;
            const uint32_t layout[]={d.data,d.format,d.width,d.height,d.depth,d.levels,d.pitch};id=Hash(layout,sizeof(layout),id);
        }
        id=Hash(&s.rs[kRsPointSpriteEnable],sizeof(uint32_t),id);
        if(!traced_effects_.try_emplace(id,true).second)return;
        std::filesystem::path dir=std::filesystem::path(destination)/("capture-"+std::to_string(effect_capture_));
        std::error_code ec;std::filesystem::create_directories(dir,ec);
        char name[96];std::snprintf(name,sizeof(name),"effect-%03zu-%016llX.txt",traced_effects_.size(),static_cast<unsigned long long>(id));
        FILE* f=OpenFile(dir/name,"w");if(!f)return;
        CaptureDrawLine(f,uint32_t(traced_effects_.size()),s.zs.data,s,prim,count,key,c);
        std::fprintf(f,"\nframe %llu points %u sprite %u size %g limits %g %g stage %g two-sided %u sampled %X alpha-test %g ref %g\n",
            static_cast<unsigned long long>(frame_),a.topology==Topology::Points,s.rs[kRsPointSpriteEnable],c.pts[0],c.pts[2],c.pts[3],c.ptsk[3],pk.two_sided,SampledStages(s),c.misc[1],c.misc[0]);
        std::fprintf(f,"shaders vs-%016llX.hlsl ps-%016llX.hlsl\n",static_cast<unsigned long long>(vs.hash),static_cast<unsigned long long>(ps.hash));
        std::fprintf(f,"fog enabled %u chip %u chip-enabled %u mode %u values %g %g %g %g color %g %g %g\n",s.rs[kRsFogEnable],s.chip_fog,s.fog_enable,s.rs[kRsFogTableMode],c.fog[0],c.fog[1],c.fog[2],c.fog[3],c.fog_color[0],c.fog_color[1],c.fog_color[2]);
        for(int stage=0;stage<4;stage++) {
            TextureDesc d;if(!s.textures[stage] || !Describe(s.textures[stage],d))continue;
            const auto& t=s.tss[stage];const auto& fmt=Format(d.format);
            std::fprintf(f,"texture %d header %08X data %08X format %s/%X size %ux%u levels %u kind %u address %u %u filter %u %u %u colorop %u alphaop %u args %u %u alpha-kill %u\n",
                stage,s.textures[stage],d.data,fmt.name,d.format,d.width,d.height,d.levels,pk.tex[stage].kind,t[kTssAddressU],t[kTssAddressV],t[kTssMinFilter],t[kTssMagFilter],t[kTssMipFilter],t[kTssColorOp],t[kTssAlphaOp],t[kTssAlphaArg1],t[kTssAlphaArg2],t[kTssAlphaKill]);
            uint32_t method=0x1B00+uint32_t(stage)*0x40;
            std::fprintf(f,"chip-texture %d offset %08X format %08X control0 %08X control1 %08X\n",stage,
                s.simple[method>>2],s.simple[(method+4)>>2],s.simple[(method+0xC)>>2],s.simple[(method+0x10)>>2]);
            if(!((SampledStages(s)>>stage)&1))continue;
            D3D11_SAMPLER_DESC sampler{};
            if(auto* bound=Sampler(s,stage,pk.tex[stage].shadow!=0,clamp))bound->GetDesc(&sampler);
            std::fprintf(f,"sampler %d filter %X min-lod %g max-lod %g bias %g original-min %u original-bias %g\n",stage,unsigned(sampler.Filter),sampler.MinLOD,sampler.MaxLOD,sampler.MipLODBias,t[kTssMaxMipLevel],Bits(t[kTssMipLodBias]));
            if(!d.width || !d.height || d.width>256 || d.height>256 || d.depth!=1 || d.cube || fmt.depth)continue;
            StagePalette palette;ReadStagePalette(s,stage,d,palette);std::vector<uint8_t> pixels;uint32_t pitch=0;
            DecodeLevel(d,0,0,static_cast<const uint8_t*>(kernel::HostPtr(d.data)),palette.va?palette.colors:nullptr,pixels,pitch);
            if(pixels.empty())continue;
            if(fmt.compressed){std::vector<uint8_t> decoded;DecodeBlocks(fmt.host,pixels.data(),d.width,d.height,decoded);pixels=std::move(decoded);pitch=d.width*4;}
            if((fmt.host!=Host::BGRA8 && !fmt.compressed) || pixels.size()<size_t(pitch)*d.height || pitch<d.width*4)continue;
            uint32_t transparent=0,opaque=0,min=255,max=0;
            for(uint32_t y=0;y<d.height;y++)for(uint32_t x=0;x<d.width;x++) {
                uint32_t alpha=pixels[size_t(y)*pitch+x*4+3];transparent+=alpha==0;opaque+=alpha==255;min=std::min(min,alpha);max=std::max(max,alpha);
            }
            std::fprintf(f,"alpha %d min %u max %u transparent %u opaque %u total %u\n",stage,min,max,transparent,opaque,d.width*d.height);
            std::vector<uint8_t> packed(size_t(d.width)*d.height*4);
            for(uint32_t y=0;y<d.height;y++)std::memcpy(packed.data()+size_t(y)*d.width*4,pixels.data()+size_t(y)*pitch,d.width*4);
            std::snprintf(name,sizeof(name),"texture-%08X-%X.bmp",d.data,d.format);WriteBmp(dir/name,packed,d.width,d.height);
        }
        if(count<=64)CaptureInputs(f,s,a,c);
        static const char* selected_ps=diag::Env("XBE_D3D11_TRACE_PS");
        bool selected=selected_ps && (!std::strcmp(selected_ps,"ALL") || ps.hash==std::strtoull(selected_ps,nullptr,16));
        if(selected_ps && !std::strcmp(selected_ps,"ALL")) {
            // Guest RAM does not contain pixels most recently written by the
            // GPU. Retain the actual target inputs for post-process replay.
            for(int stage=0;stage<4;stage++)if(target[stage] && ((SampledStages(s)>>stage)&1) && images[stage] && !images[stage]->depth) {
                std::snprintf(name,sizeof(name),"draw-%03zu-gpu-%d.bmp",traced_effects_.size(),stage);
                SaveColorImage(*images[stage],dir/name);
            }
            if(c.surf[0]<=256 && c.surf[1]<=256)trace_color_material_=uint32_t(traced_effects_.size());
        }
        if(a.topology==Topology::Points || selected) {
            std::fprintf(f,"\nassembled regs %04X decode %016llX vertices %u draw %u indices %u base %d\n",a.regs,static_cast<unsigned long long>(a.decode),a.vertex_count,a.draw_count,a.index_count,a.base_vertex);
            auto binary=[&](const char* suffix,const void* bytes,size_t length){std::snprintf(name,sizeof(name),"draw-%03zu-%s.bin",traced_effects_.size(),suffix);if(FILE* out=OpenFile(dir/name,"wb")){std::fwrite(bytes,1,length,out);std::fclose(out);}};
            binary("constants",&c,sizeof(c));binary("vsc",s.vsc.data(),sizeof(s.vsc.v));binary("elements",a.elements.data(),sizeof(a.elements));
            for(uint32_t buffer=0;buffer<a.buffer_count;buffer++) {
                std::fprintf(f,"buffer %u stride %u bytes %u\n",buffer,a.buffers[buffer].stride,a.buffers[buffer].bytes);
                char suffix[24];std::snprintf(suffix,sizeof(suffix),"vertices-%u",buffer);binary(suffix,a.buffers[buffer].src,a.buffers[buffer].bytes);
            }
            if(a.index_count)binary(a.index16?"indices16":"indices32",a.index_src,size_t(a.index_count)*(a.index16?2:4));
        }
        std::fclose(f);
        for(int stage=0;stage<2;stage++) {
            const auto& shader=stage?ps:vs;std::snprintf(name,sizeof(name),"%s-%016llX.hlsl",stage?"ps":"vs",static_cast<unsigned long long>(shader.hash));
            if(FILE* source=OpenFile(dir/name,"w")){std::string text=stage?PixelHlsl(pk):vs.source;std::fwrite(text.data(),1,text.size(),source);std::fclose(source);}
        }
        if(a.topology==Topology::Points && count>=16 && point_debug_count_<4 && (!selected_ps || std::strcmp(selected_ps,"ALL"))){trace_point_material_=uint32_t(traced_effects_.size());point_debug_count_++;}
    }
    void SaveColorImage(Image& image,const std::filesystem::path& path) {
        Image* front=&image;if(!front->resource || !Resolve(image))return;
        Unbind();D3D11_TEXTURE2D_DESC desc{};desc.Width=front->w;desc.Height=front->h;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;if(!Check(dev_->CreateTexture2D(&desc,nullptr,staging.put()),"capture texture"))return;
        ctx_->CopyResource(staging.get(),front->resource.get());D3D11_MAPPED_SUBRESOURCE mapped{};
        if(!Check(ctx_->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped),"capture map"))return;
        std::vector<uint8_t> pixels(size_t(front->w)*front->h*4);
        for(uint32_t y=0;y<front->h;y++)std::memcpy(pixels.data()+size_t(y)*front->w*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,front->w*4);
        ctx_->Unmap(staging.get(),0);WriteBmp(path,pixels,front->w,front->h);
    }
    void BeginEffectCapture(Image* front) {
        const wchar_t* destination=diag::WEnv(L"XBE_D3D11_EFFECT_TRACE");
        if(!destination)return;
        effect_trace_active_=true;traced_effects_.clear();effect_capture_++;point_debug_count_=0;
        const auto dir=std::filesystem::path(destination)/("capture-"+std::to_string(effect_capture_));
        std::error_code ec;std::filesystem::create_directories(dir,ec);
        LOGR("d3d11: effect capture %u started",effect_capture_);
        if(front)SaveColorImage(*front,dir/"frame.bmp");
    }
    void DebugPointRaster(const Assembled& a,const DrawConstants& constants, const Image& rendered,
                          ID3D11ShaderResourceView* const* views,ID3D11SamplerState* const* samplers) {
        if(!trace_point_material_)return;
        const wchar_t* destination=diag::WEnv(L"XBE_D3D11_EFFECT_TRACE");if(!destination)return;
        auto dir=std::filesystem::path(destination)/("capture-"+std::to_string(effect_capture_));
        auto& image=point_debug_image_;uint32_t w=rendered.w,h=rendered.h;
        if(!image.resource || image.w!=w || image.h!=h) {
            image=Image{};image.w=w;image.h=h;
            D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            Com<ID3D11Texture2D> texture;if(!Check(dev_->CreateTexture2D(&desc,nullptr,texture.put()),"point debug target"))return;
            texture->QueryInterface(__uuidof(ID3D11Resource),reinterpret_cast<void**>(image.resource.put()));
            if(!Check(dev_->CreateRenderTargetView(texture.get(),nullptr,image.rtv.put()),"point debug RTV"))return;
        }
        Com<ID3D11Buffer> live;ctx_->GSGetConstantBuffers(1,1,live.put());
        LOGR("d3d11: point capture %u, GS constants match %u, sprite stage %.0f",trace_point_material_,live.get()==dc_.get(),constants.ptsk[3]);
        std::string common=PointHlsl();common.resize(common.find("[maxvertexcount"));
        int stage=constants.ptsk[3]==2?0:3;std::string uv="i.tc"+std::to_string(stage)+".xy / i.tc"+std::to_string(stage)+".w";
        auto& uv_shader=CachedShader(common+"float4 main(VOut i):SV_Target{return float4("+uv+",0,1);}","ps_5_0");
        auto& alpha_shader=CachedShader(common+"Texture2D image:register(t0);SamplerState sample_image:register(s0);float4 main(VOut i):SV_Target{float a=image.Sample(sample_image,"+uv+").a;return float4(a,a,a,1);}","ps_5_0");
        auto& flat_shader=CachedShader("Texture2D image:register(t0);SamplerState sample_image:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{float a=image.Sample(sample_image,uv).a;return float4(a,a,a,1);}","ps_5_0");
        if(!uv_shader.ps || !alpha_shader.ps || !flat_shader.ps)return;
        for(int mode=0;mode<4;mode++) {
            auto* target=image.rtv.get();ctx_->OMSetRenderTargets(1,&target,nullptr);float black[4]={0,0,0,1};ctx_->ClearRenderTargetView(target,black);
            ctx_->OMSetDepthStencilState(blit_depth_.get(),0);ctx_->OMSetBlendState(blit_blend_.get(),nullptr,~0u);ctx_->RSSetState(blit_raster_.get());
            D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};ctx_->RSSetViewports(1,&viewport);
            if(mode==1){ID3D11Buffer* cb[2]={vc_.get(),dc_.get()};ctx_->VSSetConstantBuffers(0,2,cb);ctx_->GSSetConstantBuffers(0,2,cb);ctx_->PSSetConstantBuffers(0,2,cb);}
            ctx_->PSSetShader(mode<2?uv_shader.ps.get():mode==2?alpha_shader.ps.get():flat_shader.ps.get(),nullptr,0);
            if(mode>=2){ctx_->PSSetShaderResources(0,1,&views[stage]);ctx_->PSSetSamplers(0,1,&samplers[stage]);}
            if(mode==3){ctx_->VSSetShader(blit_vs_.vs.get(),nullptr,0);ctx_->GSSetShader(nullptr,nullptr,0);ctx_->IASetInputLayout(nullptr);ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx_->Draw(3,0);}
            else if(a.index_count)ctx_->DrawIndexed(a.index_count,0,a.base_vertex);else ctx_->Draw(a.draw_count,0);
            char name[64];std::snprintf(name,sizeof(name),"native-%03u-%s.bmp",trace_point_material_,mode==0?"uv":mode==1?"rebound-uv":mode==2?"alpha":"texture-alpha");
            SaveColorImage(image,dir/name);
        }
        Unbind();
    }
    Pipeline& StateObjects(const State& s,const Assembled& a,Shader& vs, uint32_t samples,
                           DepthPass pass = DepthPass::Original, bool fur_mask = false) {
        PerfScope timing{g_perf.d_pso};
        PsoKey k{}; k.vs=vs.hash; k.regs=a.regs;k.topo=uint32_t(a.topology);
        k.samples = samples;
        KeyBlend(s,k); KeyDepthStencil(s,a,k); KeyRasterizer(s,a,k);
        if (fur_mask && (k.wmask & 7)) k.wmask |= 8 | kWmaskAlphaMax;
        if (pass != DepthPass::Original) k.zwrite = pass == DepthPass::Depth;
        if (pass == DepthPass::Depth) k.blend = k.wmask = k.sten = 0;
        for(int r=0;r<16;r++) k.elements[r]=a.elements[r].format|(uint32_t(a.elements[r].buffer)<<8)|(uint32_t(a.elements[r].offset)<<16);
        auto [it,added]=pipelines_.try_emplace(Hash(&k,sizeof(k))); auto& p=it->second;
        if (!added) return p;
        g_perf.psos++;
        std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
        Com<ID3D11ShaderReflection> reflection;
        std::vector<std::string> semantics;
        if(SUCCEEDED(D3DReflect(vs.code->GetBufferPointer(),vs.code->GetBufferSize(),IID_ID3D11ShaderReflection,reinterpret_cast<void**>(reflection.put())))) {
            D3D11_SHADER_DESC desc{};reflection->GetDesc(&desc);semantics.reserve(desc.InputParameters);
            for(UINT i=0;i<desc.InputParameters;i++) {
                D3D11_SIGNATURE_PARAMETER_DESC in{};reflection->GetInputParameterDesc(i,&in);
                if(in.SystemValueType!=D3D_NAME_UNDEFINED)continue;
                uint32_t reg=in.SemanticIndex;
                if(std::strcmp(in.SemanticName,"ATTR") || reg>=16 || !((a.regs>>reg)&1)) {
                    LOGE("d3d11: unexpected input %s%u type %u mask %X, registers %04X",in.SemanticName,in.SemanticIndex,unsigned(in.ComponentType),in.Mask,a.regs);
                    continue;
                }
                semantics.emplace_back(in.SemanticName);
                const auto& e=a.elements[reg];
                elems.push_back({semantics.back().c_str(),reg,DXGI_FORMAT(e.format),e.buffer,e.offset,D3D11_INPUT_PER_VERTEX_DATA,0});
            }
        } else {
            for(int r=0;r<16;r++) if((a.regs>>r)&1) elems.push_back({"ATTR",UINT(r),DXGI_FORMAT(a.elements[r].format),
                a.elements[r].buffer,a.elements[r].offset,D3D11_INPUT_PER_VERTEX_DATA,0});
        }
        p.input_needed=!elems.empty();
        if (p.input_needed && !Check(dev_->CreateInputLayout(elems.data(),UINT(elems.size()),vs.code->GetBufferPointer(),vs.code->GetBufferSize(),p.input.put()),"input layout")) {
            LOGR("d3d11: failed layout VS %016llX, registers %04X",static_cast<unsigned long long>(vs.hash),a.regs);
            for(const auto& e:elems)LOGR("d3d11: input %s%u format %u buffer %u offset %u",e.SemanticName,e.SemanticIndex,unsigned(e.Format),e.InputSlot,e.AlignedByteOffset);
            char name[96];std::snprintf(name,sizeof(name),"d3d11-layout-%016llX.hlsl",static_cast<unsigned long long>(vs.hash));
            if(FILE* f=OpenFile(app::StateDir()/"logs"/name,"wb")){std::fwrite(vs.source.data(),1,vs.source.size(),f);std::fclose(f);}
        }
        D3D11_BLEND_DESC bd{}; auto& b=bd.RenderTarget[0]; b.BlendEnable=k.blend;
        b.SrcBlend=Blend(k.src); b.DestBlend=Blend(k.dst); b.BlendOp=BlendOp(k.op);
        b.SrcBlendAlpha=Blend(k.src,true); b.DestBlendAlpha=Blend(k.dst,true); b.BlendOpAlpha=BlendOp(k.op); b.RenderTargetWriteMask=UINT8(k.wmask&15);
        if (k.wmask & kWmaskAlphaMax) {
            if (!k.blend) {
                b.BlendEnable = TRUE;
                b.SrcBlend = D3D11_BLEND_ONE;
                b.DestBlend = D3D11_BLEND_ZERO;
                b.BlendOp = D3D11_BLEND_OP_ADD;
            }
            b.SrcBlendAlpha = b.DestBlendAlpha = D3D11_BLEND_ONE;
            b.BlendOpAlpha = D3D11_BLEND_OP_MAX;
        }
        Check(dev_->CreateBlendState(&bd,p.blend.put()),"blend state");
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=k.zen; dd.DepthWriteMask=k.zwrite?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc=D3D11_COMPARISON_FUNC(CmpIndex(k.zfunc)+1); dd.StencilEnable=k.sten; dd.StencilReadMask=UINT8(k.smask); dd.StencilWriteMask=UINT8(k.swmask);
        dd.FrontFace={StencilOp(k.sfail),StencilOp(k.szfail),StencilOp(k.spass),D3D11_COMPARISON_FUNC(CmpIndex(k.sfunc)+1)}; dd.BackFace=dd.FrontFace;
        Check(dev_->CreateDepthStencilState(&dd,p.depth.put()),"depth state");
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=k.fill==kFillWireframe?D3D11_FILL_WIREFRAME:D3D11_FILL_SOLID;
        // Expanded points have the geometry shader's winding, independent of
        // the last triangle cull mode. Match the original renderer's front face.
        if(a.topology==Topology::Points)rd.FillMode=D3D11_FILL_SOLID;
        rd.CullMode=k.cull?D3D11_CULL_BACK:D3D11_CULL_NONE; rd.FrontCounterClockwise=a.topology==Topology::Points?FALSE:k.ccw;
        rd.DepthBias=k.zbias; rd.SlopeScaledDepthBias=k.zslope; rd.DepthClipEnable=TRUE; rd.ScissorEnable=TRUE;
        rd.MultisampleEnable = samples > 1;
        Check(dev_->CreateRasterizerState(&rd,p.raster.put()),"raster state");
        return p;
    }
    void IssueGeometry(const Assembled& a) {
        if (a.index_count) ctx_->DrawIndexed(a.index_count,0,a.base_vertex);
        else ctx_->Draw(a.draw_count,0);
    }
    void DrawFurShells(const State& s, const Assembled& a, uint32_t shading, bool beyond) {
        if (!shading) return;
        const auto& fur = g_program_shading[shading - 1];
        if (fur.top < 0 || fur.top >= int(s.vsc.size())) return;
        const auto& top = s.vsc[size_t(fur.top)];
        const float* reg = fur.fill_register >= 0 && fur.fill_register < int(s.vsc.size()) ?
            s.vsc[size_t(fur.fill_register)].data() : nullptr;
        int count;
        float from, to;
        if (beyond) {
            if (!fur.beyond_of || !fur.top_layers || top[0] <= 0 || int(top[3]) != fur.top_layers - 1)
                return;
            float reach = 0;
            count = fur.beyond_of(reg,top.data(),&reach);
            from = top[0];
            to = from * (1 + reach);
        } else {
            count = fur.between_live ? int(*fur.between_live) : fur.between;
            if (fur.between_of) {
                const int picked = fur.between_of(reg,top.data());
                if (picked >= 0) count = picked;
            }
            from = ShadingBefore(shading);
            to = top[0];
        }
        if (count <= 0 || from < 0 || to <= from) return;
        auto vertices = s.vsc.v;
        if (fur.between_mark >= 0 && fur.between_mark < int(s.vsc.size()))
            vertices[size_t(fur.between_mark)][1] = -1;
        for (int shell = 1; shell <= count; ++shell) {
            vertices[size_t(fur.top)][0] = from + (to - from) * float(shell) / float(count + !beyond);
            if (!Upload(vc_.get(),vertices.data(),sizeof(vertices))) break;
            IssueGeometry(a);
        }
        last_vsc_valid_ = Upload(vc_.get(),s.vsc.data(),sizeof(s.vsc.v));
    }
    void PollReports(bool wait=false) {
        for(auto it=reports_.begin();it!=reports_.end();) {
            double samples=0; bool ready=true;
            for(const auto& q:it->queries) {
                UINT64 n=0; HRESULT hr;
                do { hr=ctx_->GetData(q->query.get(),&n,sizeof(n),wait?0:D3D11_ASYNC_GETDATA_DONOTFLUSH); if(wait && hr==S_FALSE) std::this_thread::yield(); } while(wait && hr==S_FALSE);
                if(hr==S_FALSE) {ready=false;break;}
                if(SUCCEEDED(hr)) samples+=double(n) * q->guest_per_sample;
            }
            if(!ready) { ++it; continue; }
            kernel::W32(it->address,uint32_t(frame_)); kernel::W32(it->address+4,0);
            kernel::W32(it->address+8,uint32_t(std::min(std::ceil(samples),double(UINT32_MAX))));
            std::atomic_thread_fence(std::memory_order_release); kernel::W32(it->address+12,0);
            it=reports_.erase(it);
        }
    }
    bool ReplaceSwapChain(bool hdr) {
        Com<IDXGIDevice> device;
        Com<IDXGIAdapter> adapter;
        Com<IDXGIFactory2> factory;
        if (FAILED(dev_->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(device.put()))) ||
            FAILED(device->GetAdapter(adapter.put())) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(factory.put())))) return false;
        Unbind();
        screen_.reset();
        swap_.reset();
        ctx_->Flush();
        const auto window = static_cast<HWND>(app::NativeWindow());
        const auto format = hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = hdr ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
        Com<IDXGISwapChain1> chain;
        if (!Check(factory->CreateSwapChainForHwnd(dev_.get(),window,&desc,nullptr,nullptr,chain.put()),"create display swap chain"))
            return false;
        if (hdr) {
            Com<IDXGISwapChain3> color;
            constexpr auto space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            UINT support = 0;
            if (FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(color.put()))) ||
                FAILED(color->CheckColorSpaceSupport(space,&support)) ||
                !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
                FAILED(color->SetColorSpace1(space))) return false;
        }
        if (!Check(chain->QueryInterface(__uuidof(IDXGISwapChain),reinterpret_cast<void**>(swap_.put())),"display swap chain"))
            return false;
        hdr_active_ = hdr;
        LOGR("d3d11: display output %s",hdr ? "HDR10" : "SDR");
        return true;
    }
    void UpdateOutput() {
        const auto now = std::chrono::steady_clock::now();
        if (now - output_checked_ >= std::chrono::seconds(1)) {
            output_checked_ = now;
            Com<IDXGIOutput> output;
            Com<IDXGIOutput6> modern;
            DXGI_OUTPUT_DESC1 desc{};
            const bool supported = swap_ && SUCCEEDED(swap_->GetContainingOutput(output.put())) &&
                SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6),reinterpret_cast<void**>(modern.put()))) &&
                SUCCEEDED(modern->GetDesc1(&desc)) && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            if (hdr_supported_ != supported) hdr_failed_ = false;
            hdr_supported_ = supported;
        }
        const bool wanted = hdr_supported_ && app::Get().hdr && !hdr_failed_;
        if (wanted != hdr_active_ || !swap_) {
            if (!ReplaceSwapChain(wanted) && wanted) {
                hdr_failed_ = true;
                ReplaceSwapChain(false);
            }
        }
        SetHdrAvailable(hdr_supported_ && !hdr_failed_);
    }
    bool Resize() {
        uint32_t w=0,h=0; app::PixelSize(w,h);
        if(!w || !h) return false;
        UpdateOutput();
        if (!swap_) return false;
        if(screen_ && w==ww_ && h==wh_) return true;
        Unbind(); screen_.reset();
        if(!Check(swap_->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0),"resize swap chain")) return false;
        Com<ID3D11Texture2D> back;
        if(!Check(swap_->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(back.put())),"back buffer")) return false;
        if(!Check(dev_->CreateRenderTargetView(back.get(),nullptr,screen_.put()),"screen view")) return false;
        ww_=w; wh_=h; return true;
    }
    void Display(Image* picture = nullptr, Image* mask = nullptr, bool count = true) {
        if(!ctx_ || !Resize()) return;
        const bool hdr = hdr_active_;
        if (hdr && (!ScratchColor(hdr_scene_,ww_,wh_) || !ScratchColor(hdr_ui_,ww_,wh_))) return;
        Unbind();
        auto* rt = hdr ? hdr_scene_.rtv.get() : screen_.get();
        ctx_->OMSetRenderTargets(1,&rt,nullptr);
        float black[4]={0,0,0,1}; ctx_->ClearRenderTargetView(rt,black);
        if (!picture) {
            auto it = targets_.find(shown_);
            if (it != targets_.end()) picture = &it->second;
        }
        const auto vp = PictureViewport();
        if(picture && picture->srv) {
            if (!app::Get().fsr || !DrawFsr(*picture, rt, vp)) {
            ctx_->OMSetRenderTargets(1, &rt, nullptr);
            ctx_->RSSetViewports(1, &vp);
            ctx_->RSSetState(blit_raster_.get()); ctx_->OMSetBlendState(blit_blend_.get(),nullptr,~0u); ctx_->OMSetDepthStencilState(blit_depth_.get(),0);
            ctx_->IASetInputLayout(nullptr); ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->VSSetShader(blit_vs_.vs.get(),nullptr,0); ctx_->PSSetShader(blit_ps_.ps.get(),nullptr,0); ctx_->GSSetShader(nullptr,nullptr,0);
            auto* srv=picture->srv.get(); ctx_->PSSetShaderResources(0,1,&srv);
            auto* sampler=blit_sampler_.get(); ctx_->PSSetSamplers(0,1,&sampler);
            float constants[4]={g_fade.load(),float(app::Get().gamma)/220.0f,float(app::Get().brightness)/100.0f,0};
            Upload(fade_.get(),constants,sizeof(constants)); auto* cb=fade_.get(); ctx_->PSSetConstantBuffers(0,1,&cb);
            ctx_->Draw(3,0);
            }
        }
        if(ui_) {
            std::lock_guard lock(app::UiLock());
            app::UiNewFrame(); app::UiFontResize(); ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
            if(app::Welcoming()) app::DrawWelcome(); else if(app::kGame.ui) app::kGame.ui(fps_);
            ImGui::Render();
            if (hdr) {
                const float transparent[4]{};
                auto* overlay = hdr_ui_.rtv.get();
                ctx_->ClearRenderTargetView(overlay,transparent);
                ctx_->OMSetRenderTargets(1,&overlay,nullptr);
            } else ctx_->OMSetRenderTargets(1,&rt,nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        if (hdr) {
            const auto& set = app::Get();
            const float white = float(set.hdr_paper_white);
            const bool highlights = mask && mask->srv && HdrHighlights();
            const float constants[] = {white/10000.0f,2.2f,std::max(float(set.hdr_peak)/white,1.0f),
                1.0f-0.5f*set.hdr_highlights/100.0f,vp.TopLeftX,vp.TopLeftY,1.0f/vp.Width,1.0f/vp.Height,
                highlights?1.0f:0.0f,ui_?1.0f:0.0f,float(frame_%1024),0.0f};
            const D3D11_VIEWPORT output{0,0,float(ww_),float(wh_),0,1};
            if (!FullScreenPass(hdr_encode_ps_,kHdrHlsl,"encode",screen_.get(),output,
                    {hdr_scene_.srv.get(),highlights ? mask->srv.get() : nullptr,ui_ ? hdr_ui_.srv.get() : nullptr},constants))
                return;
        }
        PerfScope timing{g_perf.swap};
        HRESULT hr=swap_->Present(app::Get().vsync?1:0,0);
        if(FAILED(hr)) { Check(hr,"present"); Check(dev_->GetDeviceRemovedReason(),"device removed reason"); }
        if (hr == S_OK && count) {
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
    void FinishPicture(Image& image,Image& mask) {
        if (!post_done_ && post_target_ == ResourceKey(image.data,false)) {
            if (PostWanted()) ApplyPost(image);
            KeepHdrWorld(image);
        }
        Resolve(image);
        if (!MakeHdrMask(image,mask)) mask = {};
        hdr_world_key_ = 0;
        post_target_ = 0;
        post_done_ = occlusion_done_ = view_ok_ = many_views_ = false;
    }
    void KeepPicture(const Surface& front, uint32_t slot) {
        Image* picture = Target(front,false);
        if (!picture) return;
        CpuWrite(*picture);
        FinishPicture(*picture,generated_masks_[slot]);
        auto& copy = generated_[slot];
        if (!ScratchColor(copy,picture->w,picture->h)) return;
        Unbind();
        ctx_->CopyResource(copy.resource.get(),picture->resource.get());
        copy.fur_frame = picture->fur_frame;
        taken_ |= 1u << slot;
    }
public:
    ~D3D11Backend() override {
        if(ui_) {std::lock_guard lock(app::UiLock()); ImGui_ImplDX11_Shutdown();}
        if(ctx_) {ctx_->ClearState();ctx_->Flush();}
    }
    bool Init(uint32_t width,uint32_t height) override {
        g_dlss_state = 4;  // no DLSS here: the menu says so and keeps the render size and FSR 1 (ui.cpp)
        g_d3d11 = true;
        LoadShaderCache();
        if (!width || !height) return false;
        guest_frame_ = {width, height};
        // Explicit FL11_0 keeps this usable on Ivy Bridge; never silently fall back to CPU/WARP.
        DXGI_SWAP_CHAIN_DESC sc{}; sc.BufferDesc.Width=width; sc.BufferDesc.Height=height;
        sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount=2; sc.OutputWindow=static_cast<HWND>(app::NativeWindow()); sc.SampleDesc.Count=1;
        sc.Windowed=TRUE; sc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL wanted=D3D_FEATURE_LEVEL_11_0,got{};
        auto preferred = SelectAdapter();
        auto create = [&](IDXGIAdapter* adapter) {
            return D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                nullptr, 0, &wanted, 1, D3D11_SDK_VERSION, &sc, swap_.put(), dev_.put(), &got, ctx_.put());
        };
        HRESULT created = create(preferred.get());
        if (FAILED(created) && preferred) {
            LOGR("d3d11: selected adapter failed (0x%08X), using the default adapter", unsigned(created));
            created = create(nullptr);
        }
        if (!Check(created, "create hardware device")) return false;
        Com<IDXGIDevice> dxdev; Com<IDXGIAdapter> adapter;
        if(SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(dxdev.put()))) && SUCCEEDED(dxdev->GetAdapter(adapter.put()))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                const auto name = AdapterName(desc.Description);
                if (std::find(Adapters().begin(), Adapters().end(), name) == Adapters().end())
                    Adapters().push_back(name);
                LOGR("d3d11: %s, feature level %X", name.c_str(), unsigned(got));
            }
        }
        SetHdrAvailable(false);
        for (uint32_t samples : {2u, 4u, 8u}) {
            UINT color_levels = 0, depth_levels = 0;
            if (SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &color_levels)) &&
                SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, samples, &depth_levels)) &&
                color_levels && depth_levels) supported_samples_ |= samples;
        }
        if(!Buffer(vc_,192*4*sizeof(float),D3D11_BIND_CONSTANT_BUFFER) || !Buffer(dc_,sizeof(DrawConstants),D3D11_BIND_CONSTANT_BUFFER) ||
           !Buffer(fade_,16,D3D11_BIND_CONSTANT_BUFFER) || !Buffer(pass_constants_,sizeof(float)*kPassConstantFloats,D3D11_BIND_CONSTANT_BUFFER)) return false;
        blit_vs_=Compile(kBlit,"vs","vs_5_0"); blit_ps_=Compile(kBlit,"ps","ps_5_0");
        if(!blit_vs_.vs || !blit_ps_.ps) return false;
        D3D11_SAMPLER_DESC sm{}; sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD=D3D11_FLOAT32_MAX; sm.MaxAnisotropy=1; sm.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
        if(!Check(dev_->CreateSamplerState(&sm,blit_sampler_.put()),"blit sampler")) return false;
        D3D11_BLEND_DESC bd=CopyBlendDesc(15);
        if(!Check(dev_->CreateBlendState(&bd,blit_blend_.put()),"blit blend state"))return false;
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;
        if(!Check(dev_->CreateDepthStencilState(&dd,blit_depth_.put()),"blit depth state"))return false;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
        if(!Check(dev_->CreateRasterizerState(&rd,blit_raster_.put()),"blit raster state"))return false;
        if (!InitTransfers()) return false;
        {std::lock_guard lock(app::UiLock()); ui_=ImGui_ImplDX11_Init(dev_.get(),ctx_.get());}
        if (!ui_ || !Resize()) return false;
        ApplyRenderScale();
        return true;
    }
    // a rectangle clear's blend (bi 1: color written) and depth state (di: 1 depth, 2 stencil)
    bool ClearStates(unsigned bi, unsigned di) {
        if(!clear_blend_[bi]) {D3D11_BLEND_DESC b=CopyBlendDesc(bi?15:0);if(!Check(dev_->CreateBlendState(&b,clear_blend_[bi].put()),"clear blend state"))return false;}
        if(!clear_depth_[di]) {D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=di&1;d.DepthFunc=D3D11_COMPARISON_ALWAYS;
            d.DepthWriteMask=(di&1)?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;d.StencilEnable=di&2;d.StencilReadMask=d.StencilWriteMask=255;
            d.FrontFace={D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};d.BackFace=d.FrontFace;
            if(!Check(dev_->CreateDepthStencilState(&d,clear_depth_[di].put()),"clear depth state"))return false;}
        return true;
    }
    // g_hud_split: the sides a frame wide HUD draw no longer covers, black (kHudInPlaceBars), on color (target's
    // attachment); over the draw's own edge by a console pixel: its outermost texels blend with what lies under, a
    // hairline where the frame's edge hid it
    void HudBars(Image& color, const Image& target) {
        const float W = float(target.w), sc = W / float(std::max<uint32_t>(target.guest.w, 1));
        const float inset = std::ceil(hud_.scale * sc);
        const float l = std::ceil(hud_.shift * sc + inset), r = std::floor(hud_.shift * sc + W * hud_.scale - inset);
        auto& vs=CachedShader(kClearVs,"vs_5_0");auto& ps=CachedShader(kClearPs,"ps_5_0");
        if(!vs.vs || !ps.ps || !ClearStates(1,0)) return;
        Unbind();
        const float constants[8]={0,0,0,1,0,0,0,0};Upload(dc_.get(),constants,sizeof(constants));last_dc_valid_=false;
        ID3D11Buffer* cb=dc_.get();ctx_->VSSetConstantBuffers(0,1,&cb);ctx_->PSSetConstantBuffers(0,1,&cb);
        ctx_->VSSetShader(vs.vs.get(),nullptr,0);ctx_->PSSetShader(ps.ps.get(),nullptr,0);ctx_->GSSetShader(nullptr,nullptr,0);
        ctx_->IASetInputLayout(nullptr);ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx_->RSSetState(blit_raster_.get());
        ctx_->OMSetBlendState(clear_blend_[1].get(),nullptr,~0u);ctx_->OMSetDepthStencilState(clear_depth_[0].get(),0);
        auto* view=color.rtv.get();ctx_->OMSetRenderTargets(1,&view,nullptr);
        const float H = float(target.h);
        if (l > 0) {const D3D11_VIEWPORT vp{0,0,std::min(l,W),H,0,1};ctx_->RSSetViewports(1,&vp);ctx_->Draw(3,0);}
        if (r < W) {const float x=std::max(r,0.0f);const D3D11_VIEWPORT vp{x,0,W-x,H,0,1};ctx_->RSSetViewports(1,&vp);ctx_->Draw(3,0);}
    }
    void Clear(const State& s,uint32_t count,const uint8_t* rects,uint32_t flags,uint32_t color,float z,uint32_t stencil) override {
        PerfScope timing{g_perf.clear};
        if(!ctx_) return; Unbind();
        Image* rt=(flags&kClearTarget)?Target(s.rt,false):nullptr;
        Image* ds=(flags&(kClearZ|kClearStencil))?Target(s.zs,true):nullptr;
        if(count && rects && rt && ds && (rt->w!=ds->w || rt->h!=ds->h)) {
            Clear(s,count,rects,flags&kClearTarget,color,z,stencil);
            Clear(s,count,rects,flags&(kClearZ|kClearStencil),color,z,stencil);return;
        }
        const bool multi = (!rt || rt->ms) && (!ds || ds->ms);
        const bool partial = count && rects;
        Image* color_target = rt && multi && !partial ? rt->ms.get() : Attachment(rt, multi);
        const bool both_depth = (flags & (kClearZ | kClearStencil)) == (kClearZ | kClearStencil);
        Image* depth_target = ds && multi && !partial && both_depth ? ds->ms.get() : Attachment(ds, multi);
        if ((rt && !color_target) || (ds && !depth_target)) return;
        float c[4]; Color(color,c);
        if(count && rects) {
            auto& vs=CachedShader(kClearVs,"vs_5_0");auto& ps=CachedShader(kClearPs,"ps_5_0");if(!vs.vs||!ps.ps)return;
            unsigned bi=(flags&kClearTarget)?1:0,di=((flags&kClearZ)?1:0)|((flags&kClearStencil)?2:0);
            if(!ClearStates(bi,di))return;
            float constants[8]={c[0],c[1],c[2],c[3],z,0,0,0};Upload(dc_.get(),constants,sizeof(constants));last_dc_valid_=false;
            ID3D11Buffer* cb=dc_.get();ctx_->VSSetConstantBuffers(0,1,&cb);ctx_->PSSetConstantBuffers(0,1,&cb);
            ctx_->VSSetShader(vs.vs.get(),nullptr,0);ctx_->PSSetShader(ps.ps.get(),nullptr,0);ctx_->GSSetShader(nullptr,nullptr,0);
            ctx_->IASetInputLayout(nullptr);ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx_->RSSetState(blit_raster_.get());
            ctx_->OMSetBlendState(clear_blend_[bi].get(),nullptr,~0u);ctx_->OMSetDepthStencilState(clear_depth_[di].get(),stencil&255);
            auto* view=color_target?color_target->rtv.get():nullptr;
            ctx_->OMSetRenderTargets(rt?1:0,&view,depth_target?depth_target->dsv.get():nullptr);
            if (!rt && !ds) return;
            const Image& target = rt ? *rt : *ds;
            for (uint32_t i = 0; i < count; ++i) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * 16, 16);
                const auto area = RenderRect(target, {r[0], r[1], r[2], r[3]});
                if (Empty(area)) continue;
                const D3D11_VIEWPORT vp{float(area.left), float(area.top), float(area.right - area.left),
                                       float(area.bottom - area.top), 0, 1};
                ctx_->RSSetViewports(1, &vp);
                ctx_->Draw(3, 0);
            }
            if(rt) MarkWritten(*rt, multi);
            if(ds) MarkWritten(*ds, multi);
            return;
        }
        if(rt && (flags&kClearTarget)) {
            ctx_->ClearRenderTargetView(color_target->rtv.get(),c);
            rt->fur_frame = UINT64_MAX;
            MarkWritten(*rt, multi);
        }
        uint32_t mask=((flags&kClearZ)?D3D11_CLEAR_DEPTH:0)|((flags&kClearStencil)?D3D11_CLEAR_STENCIL:0);
        if(ds && mask) {ctx_->ClearDepthStencilView(depth_target->dsv.get(),mask,z,UINT8(stencil));MarkWritten(*ds, multi);}
    }
    void Draw(const State& original,uint32_t prim,uint32_t start,uint32_t count,const uint8_t* indices) override {
        PerfScope timing{g_perf.draw};g_perf.draws++;g_perf.verts+=count;
        if(!ctx_) return;
        hud_.Reset();
        trace_point_material_=trace_color_material_=0;
        auto step_start=PerfClock::now();
        auto step=[&](unsigned part){driver_timing_[part]+=PerfMs(step_start);step_start=PerfClock::now();};
        const State* sp=&original;
        if(FixedSprite(original,prim)) {sprite_=original;SpriteStage(sprite_);sp=&sprite_;}
        const State& s=*sp; PollReports();
        if (!replaying_) WatchDraw(s,start,count,indices);
        auto assembly_start=PerfClock::now();
        const bool patches = TessellatedDraw(s,prim);
        Assembled& a=assembled_; if(!Assemble(s,prim,start,count,indices,a,patches)) return;
        g_perf.assemble+=PerfMs(assembly_start);
        // D3D11 aligns an element to min(4, its format size). Xbox SHORT3
        // streams can have six-byte offsets/strides; repack those locally.
        for(uint32_t slot=0;slot<a.buffer_count;slot++) {
            auto& b=a.buffers[slot];auto& storage=color_buffers_[slot];
            bool repack=false,copied=false;
            for(int r=0;r<16;r++)if((a.regs>>r)&1) {
                const auto& e=a.elements[r];if(e.buffer!=slot)continue;
                uint32_t align=std::min(4u,InputBytes(e.format));
                if(align && (e.offset%align || b.stride%align))repack=true;
            }
            if(repack && b.stride) {
                uint32_t offsets[16]{},stride=0;
                for(int r=0;r<16;r++)if(((a.regs>>r)&1) && a.elements[r].buffer==slot) {
                    uint32_t size=InputBytes(a.elements[r].format),align=std::min(4u,size);
                    if(!align)return;
                    stride=(stride+align-1)&~(align-1);offsets[r]=stride;stride+=size;
                }
                stride=(stride+3)&~3u;
                uint32_t vertices=b.bytes/b.stride;
                storage.assign(size_t(vertices)*stride,0);
                for(int r=0;r<16;r++)if(((a.regs>>r)&1) && a.elements[r].buffer==slot) {
                    auto& e=a.elements[r];uint32_t size=InputBytes(e.format);
                    for(uint32_t v=0;v<vertices;v++) {
                        size_t source=size_t(v)*b.stride+e.offset;
                        if(source<b.bytes)std::memcpy(storage.data()+size_t(v)*stride+offsets[r],b.src+source,std::min(size_t(size),size_t(b.bytes)-source));
                    }
                    e.offset=uint16_t(offsets[r]);
                }
                b.src=storage.data();b.bytes=uint32_t(storage.size());b.stride=stride;copied=true;
            }
            // Normalize packed colors without changing guest memory.
            for(int r=0;r<16;r++)if((a.regs>>r)&1) {
                auto& e=a.elements[r];if(e.buffer!=slot || e.format!=DXGI_FORMAT_B8G8R8A8_UNORM)continue;
                if(!copied){storage.assign(b.src,b.src+b.bytes);b.src=storage.data();copied=true;}
                if(b.stride)for(size_t at=e.offset;at+3<b.bytes;at+=b.stride)std::swap(storage[at],storage[at+2]);
                e.format=DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }
        step(0);
        Image* rt=Target(s.rt,false);
        Image* ds=(s.rs[kRsZEnable] || s.rs[kRsStencilEnable])?Target(s.zs,true):nullptr;
        if(rt && ds && (rt->w!=ds->w || rt->h!=ds->h))ds=nullptr;
        if(!rt && !ds) return;
        const Image& target = rt ? *rt : *ds;
        if(rt) CpuWrite(*rt);
        // g_hud_split: a HUD draw onto the frame drawn narrowed to its place
        const bool hud = rt && hud_.Place(s,a,start,count,indices,PosW(s),rt->guest.w,rt->guest.h,
                                          guest_frame_.w,guest_frame_.h,replaying_);
        step(1);
        ShaderChoice sh; ClassifyShaders(s,a,count,sh); if(sh.prog) EnsureProgramHash(s);
        TrackPost(s, a, sh.prog, rt, ds);
        sh.shading=DrawShading(s,sh);
        const ProgramShading* shading = sh.shading ? &g_program_shading[sh.shading-1] : nullptr;
        // fur's shells (depth_alpha's, top's x above 0) start the occlusion if nothing has yet: their depth, in only
        // where a strand is opaque enough, read by it speckled the fur with shadow
        if (rt && shading && shading->depth_alpha > 0 && !occlusion_done_ && !post_done_ &&
            app::Get().ambient_occlusion && post_target_ == ResourceKey(rt->data, false) &&
            (shading->top < 0 || s.vsc[size_t(shading->top)][0] > 0))
            ApplyOcclusion(*rt);
        ID3D11HullShader* hull = nullptr;
        ID3D11DomainShader* domain = nullptr;
        if (a.topology == Topology::Patches && shading) {
            auto [it, inserted] = tessellation_.try_emplace(sh.shading);
            if (inserted) {
                const auto src = TessHlsl(sh.shading);
                it->second.first = Compile(src,"hs","hs_5_0");
                it->second.second = Compile(src,"ds","ds_5_0");
            }
            hull = it->second.first.hs.get();
            domain = it->second.second.ds.get();
            if (!hull || !domain) { hull = nullptr; domain = nullptr; }
        }
        auto [vit,new_vs]=vertex_shaders_.try_emplace(VertexKey(s,a,sh));
        if(new_vs) {auto ve=MakeVertexEntry(s,a,sh);vit->second=Compile(VertexHlsl(ve,DrawProgram(s,sh)),"main","vs_5_0");}
        Shader& vs=vit->second; if(!vs.vs) return;
        step(2);
        bool clamp=UnitTexcoords(s,a);
        TexInfo info[4]{}; bool is_rt[4]{}, exact[4]{}; float scale[4]={1,1,1,1},wide[4]{};
        ID3D11ShaderResourceView* views[4]{}; ID3D11SamplerState* samplers[4]{};
        Image* sampled_images[4]{};
        uint32_t sampled=SampledStages(s);
        for(int i=0;i<4;i++) {
            Image* tx=Texture(s,i,info[i],is_rt[i],((sampled>>i)&1)!=0);
            if(tx && (tx==rt || tx==ds)) tx=Snapshot(*tx);
            sampled_images[i]=tx;if(tx) views[i]=tx->srv.get();
            exact[i]=(info[i].flags&kTexDepthBytes)!=0;
            if (tx && is_rt[i]) {
                scale[i] = float(tx->w) / tx->guest.w;
                const float sy = float(tx->h) / tx->guest.h;
                // Keep scaled downsample passes from skipping bright console texels.
                if (!tx->depth && !exact[i] && std::max(scale[i], sy) > 1.1f &&
                    (tx->guest.w > target.guest.w || tx->guest.h > target.guest.h)) {
                    wide[i] = std::max(scale[i], sy);
                    info[i].flags |= kTexWide;
                }
            }
            if((sampled>>i)&1)samplers[i]=Sampler(s,i,info[i].shadow!=0,clamp);
        }
        step(3);
        ID3D11ShaderResourceView* material[2]{};
        if (shading) {
            material[0] = GetField(shading->field);
            if (rt && shading->screen)
                if (Image* screen = Snapshot(*rt)) material[1] = screen->srv.get();
        }
        PixelKey pk=MakePixelKey(s,info); pk.quad=sh.quad; pk.shading=sh.shading;
        auto [pit,new_ps]=pixel_shaders_.try_emplace(Hash(&pk,sizeof(pk)));
        if(new_ps) pit->second=Compile(PixelHlsl(pk),"main","ps_5_0");
        Shader& ps=pit->second; if(!ps.ps) return;
        const bool multi = (!rt || rt->ms) && (!ds || ds->ms);
        Image* color_target = Attachment(rt, multi);
        Image* depth_target = Attachment(ds, multi);
        if ((rt && !color_target) || (ds && !depth_target)) return;
        const uint32_t samples = (color_target ? color_target : depth_target)->samples;
        const bool blend_enabled = SimpleState(s,kMBlendEnable) != 0;
        const bool depth_write = SimpleState(s,kMZWriteEnable) != 0;
        Shader* depth_ps = nullptr;
        Pipeline* depth_p = nullptr;
        if (ds && s.rs[kRsZEnable] && blend_enabled) {
            float alpha = 0;
            if (depth_write && shading && shading->depth_alpha > 0 &&
                (shading->top < 0 || s.vsc[size_t(shading->top)][0] > 0))
                alpha = shading->depth_alpha;
            else if (!depth_write && SurfaceDepth(s,start,count,indices))
                alpha = g_surface_depth.alpha;
            if (alpha > 0) {
                PixelKey key = pk;
                key.depth_pass = std::bit_cast<uint32_t>(alpha);
                auto [it, added] = pixel_shaders_.try_emplace(Hash(&key,sizeof(key)));
                if (added) it->second = Compile(PixelHlsl(key),"main","ps_5_0");
                auto& depth = StateObjects(s,a,vs,samples,DepthPass::Depth);
                if (it->second.ps && depth.blend && depth.depth) {
                    depth_ps = &it->second;
                    depth_p = &depth;
                }
            }
        }
        const bool fur_mask = rt && shading && shading->unsharpened && (app::Get().fsr || DepthOfFieldLately());
        Pipeline& p=StateObjects(s,a,vs,samples,depth_p?DepthPass::Color:DepthPass::Original,fur_mask);
        if(!p.blend || !p.depth || !p.raster || (p.input_needed && !p.input)) return;
        step(4);
        auto record_start=PerfClock::now();
        for(uint32_t i=0;i<a.buffer_count;i++) {
            if(!a.buffers[i].bytes) continue;
            UINT off=0;
            if(!Stream(vertices_[i],vertex_bytes_[i],vertex_next_[i],D3D11_BIND_VERTEX_BUFFER,a.buffers[i].src,a.buffers[i].bytes,off))return;
            ID3D11Buffer* vb=vertices_[i].get(); UINT stride=a.buffers[i].stride;
            if(!draw_state_valid_ || bound_vertices_[i]!=vb || bound_strides_[i]!=stride || bound_offsets_[i]!=off){ctx_->IASetVertexBuffers(i,1,&vb,&stride,&off);bound_vertices_[i]=vb;bound_strides_[i]=stride;bound_offsets_[i]=off;}
        }
        if(a.index_count) {
            uint32_t bytes=a.index_count*(a.index16?2:4);
            UINT offset=0;if(!Stream(indices_,index_bytes_,index_next_,D3D11_BIND_INDEX_BUFFER,a.index_src,bytes,offset))return;
            DXGI_FORMAT format=a.index16?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT;
            if(!draw_state_valid_ || bound_indices_!=indices_.get() || bound_index_format_!=format || bound_index_offset_!=offset){ctx_->IASetIndexBuffer(indices_.get(),format,offset);bound_indices_=indices_.get();bound_index_format_=format;bound_index_offset_=offset;}
        }
        step(5);
        DrawConstants c{}; const GuestExtent extent = target.guest;
        FillSurfaceConstants(s,extent,c); if(app::Get().fog!=app::Fog::Disabled) FillFogConstants(s,sh.prog,c);
        FillCombinerConstants(s,c); FillTextureConstants(s,is_rt,exact,scale,wide,c);
        if (sh.shading) FillShadingConstants(s,sh.shading,c);
        if (!sh.shading && blend_enabled && (pk.tex[0].flags & kTexDepthBytes)) {
            PsoKey blend_key{};
            KeyBlend(s,blend_key);
            NoteDepthOfField(pk,c.pc,blend_key.dst);
        }
        for (int i = 0; i < 4; ++i) {
            const Image* tx = sampled_images[i];
            if (!tx || !is_rt[i] || exact[i]) continue;
            c.texsize[i][3] = -0.5f / tx->h;
            if (wide[i] > 0) c.spread[i][1] = c.spread[i][0] * tx->w / tx->h;
        }
        if(sh.fixed_transform) {FillTransformConstants(s,extent,c);FillLightConstants(s,sh.fk,c);}
        if (a.topology == Topology::Points) {
            c.surf[3] = 1;
            c.ptpx[0] = float(target.w) / extent.w;
            c.ptpx[1] = float(target.h) / extent.h;
            FillPointConstants(s,sh.prog,sp==&sprite_,c);
        }
        TraceEffect(s,a,prim,count,vs,ps,pk,c,clamp,sampled_images,is_rt);
        auto constants_start=PerfClock::now();
        if(!last_vsc_valid_ || std::memcmp(last_vsc_.data(),s.vsc.data(),sizeof(s.vsc.v))) {
            if(!Upload(vc_.get(),s.vsc.data(),sizeof(s.vsc.v)))return;
            last_vsc_=s.vsc.v;last_vsc_valid_=true;g_perf.upload+=sizeof(s.vsc.v);
        }
        if(!last_dc_valid_ || std::memcmp(&last_dc_,&c,sizeof(c))) {
            if(!Upload(dc_.get(),&c,sizeof(c)))return;
            last_dc_=c;last_dc_valid_=true;g_perf.upload+=sizeof(c);
        }
        g_perf.d_consts+=PerfMs(constants_start);
        step(6);
        if(!draw_state_valid_){ID3D11Buffer* cb[2]={vc_.get(),dc_.get()};ctx_->VSSetConstantBuffers(0,2,cb);ctx_->PSSetConstantBuffers(0,2,cb);ctx_->GSSetConstantBuffers(0,2,cb);}
        if(!draw_state_valid_ || bound_vs_!=vs.vs.get()){ctx_->VSSetShader(vs.vs.get(),nullptr,0);bound_vs_=vs.vs.get();}
        if(!draw_state_valid_ || bound_ps_!=ps.ps.get()){ctx_->PSSetShader(ps.ps.get(),nullptr,0);bound_ps_=ps.ps.get();}
        ID3D11GeometryShader* geometry=nullptr;
        if(a.topology==Topology::Points) {auto& gs=CachedShader(PointHlsl(),"gs_5_0");if(!gs.gs)return;geometry=gs.gs.get();}
        if(!draw_state_valid_ || bound_gs_!=geometry){ctx_->GSSetShader(geometry,nullptr,0);bound_gs_=geometry;}
        if (bound_hs_ != hull || bound_domain_ != domain) {
            if (!domain) {
                ID3D11ShaderResourceView* empty[6]{};
                ctx_->DSSetShaderResources(0,6,empty);
            }
            ctx_->HSSetShader(hull,nullptr,0);
            ctx_->DSSetShader(domain,nullptr,0);
            bound_hs_ = hull;
            bound_domain_ = domain;
        }
        if (shading && (shading->field || shading->screen)) {
            ID3D11SamplerState* extras[2] = {blit_sampler_.get(),blit_sampler_.get()};
            ctx_->VSSetShaderResources(4,2,material);
            ctx_->VSSetSamplers(4,2,extras);
            ctx_->PSSetShaderResources(4,2,material);
            ctx_->PSSetSamplers(4,2,extras);
            if (hull && domain) {
                ID3D11Buffer* cb[2]={vc_.get(),dc_.get()};
                ctx_->HSSetConstantBuffers(0,2,cb);
                ctx_->DSSetConstantBuffers(0,2,cb);
                ctx_->DSSetShaderResources(0,4,views);
                ctx_->DSSetShaderResources(4,2,material);
                ctx_->DSSetSamplers(0,4,samplers);
                ctx_->DSSetSamplers(4,2,extras);
            }
        }
        ID3D11RenderTargetView* rtv=color_target?color_target->rtv.get():nullptr;
        auto* dsv=depth_target?depth_target->dsv.get():nullptr;
        if(!draw_state_valid_ || bound_rt_!=rtv || bound_ds_!=dsv){
            ID3D11ShaderResourceView* empty[4]{};ctx_->PSSetShaderResources(0,4,empty);bound_views_.fill(nullptr);
            ctx_->OMSetRenderTargets(rt?1:0,&rtv,dsv);bound_rt_=rtv;bound_ds_=dsv;
        }
        if(!draw_state_valid_ || std::memcmp(bound_views_.data(),views,sizeof(views))){ctx_->PSSetShaderResources(0,4,views);std::copy(std::begin(views),std::end(views),bound_views_.begin());}
        if(!draw_state_valid_ || std::memcmp(bound_samplers_.data(),samplers,sizeof(samplers))){ctx_->PSSetSamplers(0,4,samplers);std::copy(std::begin(samplers),std::end(samplers),bound_samplers_.begin());}
        float blend[4]; Color(SimpleState(s,kMBlendColor),blend);
        if(!draw_state_valid_ || bound_blend_!=p.blend.get() || std::memcmp(bound_blend_factor_,blend,sizeof(blend))){ctx_->OMSetBlendState(p.blend.get(),blend,~0u);bound_blend_=p.blend.get();std::memcpy(bound_blend_factor_,blend,sizeof(blend));}
        uint32_t stencil_ref=SimpleState(s,kMStencilRef)&255;
        if(!draw_state_valid_ || bound_depth_!=p.depth.get() || bound_stencil_ref_!=stencil_ref){ctx_->OMSetDepthStencilState(p.depth.get(),stencil_ref);bound_depth_=p.depth.get();bound_stencil_ref_=stencil_ref;}
        if(!draw_state_valid_ || bound_raster_!=p.raster.get()){ctx_->RSSetState(p.raster.get());bound_raster_=p.raster.get();}
        // Shared HLSL already applies the Xbox viewport; the host viewport covers the entire target.
        D3D11_VIEWPORT vp{0,0,float(target.w),float(target.h),0,1};
        // g_hud_split's remap: the target narrowed to the draw's place, the scissor with it
        const float hud_sc = float(target.w) / float(std::max<uint32_t>(target.guest.w, 1));
        if (hud) {vp.TopLeftX = hud_.shift * hud_sc; vp.Width = float(target.w) * hud_.scale;}
        if(!draw_state_valid_ || std::memcmp(&bound_viewport_,&vp,sizeof(vp))){ctx_->RSSetViewports(1,&vp);bound_viewport_=vp;}
        GuestRect clip{int32_t(s.viewport.x), int32_t(s.viewport.y),
                       int32_t(s.viewport.x+s.viewport.w), int32_t(s.viewport.y+s.viewport.h)};
        if (s.scissor_count) {
            clip.left = std::max(clip.left, int32_t(s.scissors[0]));
            clip.top = std::max(clip.top, int32_t(s.scissors[1]));
            clip.right = std::min(clip.right, int32_t(s.scissors[2]));
            clip.bottom = std::min(clip.bottom, int32_t(s.scissors[3]));
        }
        auto rect = RenderRect(target, clip);
        if (hud && (hud_.scale != 1 || hud_.shift != 0)) {
            const float vx = hud_.shift * hud_sc;
            rect.left = LONG(std::clamp(std::floor(float(rect.left) * hud_.scale + vx), 0.0f, float(target.w)));
            rect.right = LONG(std::clamp(std::ceil(float(rect.right) * hud_.scale + vx), 0.0f, float(target.w)));
        }
        rect.right = std::max(rect.right, rect.left);
        rect.bottom = std::max(rect.bottom, rect.top);
        if(!draw_state_valid_ || std::memcmp(&bound_scissor_,&rect,sizeof(rect))){ctx_->RSSetScissorRects(1,&rect);bound_scissor_=rect;}
        if(!draw_state_valid_ || bound_input_!=p.input.get()){ctx_->IASetInputLayout(p.input.get());bound_input_=p.input.get();}
        auto topo=hull && domain?D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
            a.topology==Topology::Points?D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:a.topology==Topology::Lines?
            (a.strip?D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:D3D11_PRIMITIVE_TOPOLOGY_LINELIST):
            (a.strip?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(!draw_state_valid_ || bound_topology_!=topo){ctx_->IASetPrimitiveTopology(topo);bound_topology_=topo;}
        step(7);
        DrawFurShells(s,a,sh.shading,false);
        Query q;
        if(!replaying_ && SimpleState(s,kMCountEnable)) {
            q=std::make_shared<SampleQuery>(); D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
            q->guest_per_sample = (double(extent.w) * extent.h) / (double(target.w) * target.h * samples);
            if(SUCCEEDED(dev_->CreateQuery(&desc,q->query.put()))) ctx_->Begin(q->query.get()); else q.reset();
        }
        IssueGeometry(a);
        if(q) {ctx_->End(q->query.get());counted_.push_back(std::move(q));}
        DrawFurShells(s,a,sh.shading,true);
        if (depth_p) {
            ctx_->PSSetShader(depth_ps->ps.get(),nullptr,0);
            ctx_->OMSetBlendState(depth_p->blend.get(),blend,~0u);
            ctx_->OMSetDepthStencilState(depth_p->depth.get(),stencil_ref);
            IssueGeometry(a);
            ctx_->PSSetShader(ps.ps.get(),nullptr,0);
            ctx_->OMSetBlendState(p.blend.get(),blend,~0u);
            ctx_->OMSetDepthStencilState(p.depth.get(),stencil_ref);
        }
        step(8);
        draw_state_valid_=true;g_perf.d_record+=PerfMs(record_start);
        if (hud_.bars && color_target) HudBars(*color_target, target);
        if (rt) {
            if (fur_mask) rt->fur_frame = frame_;
            MarkWritten(*rt, multi);
        }
        if (ds && (depth_write || depth_p || s.rs[kRsStencilEnable])) MarkWritten(*ds, multi);
        DebugPointRaster(a,c,target,views,samplers);
        if(trace_color_material_ && rt) {
            const auto dir=std::filesystem::path(diag::WEnv(L"XBE_D3D11_EFFECT_TRACE"))/("capture-"+std::to_string(effect_capture_));
            char name[64];std::snprintf(name,sizeof(name),"draw-%03u-output.bmp",trace_color_material_);SaveColorImage(*rt,dir/name);
        }
    }
    void Copy(const Surface& src,const Surface& dst,uint32_t count,uint32_t rects,uint32_t points) override {
        PerfScope timing{g_perf.copy};
        if(!ctx_) return; Unbind();
        CopySurfaces(src,dst,count,rects,points);
    }
    void Present(const Surface& front) override {
        if(!ctx_)return;
        if (!std::exchange(hud_.rolled, false)) hud_.Roll(guest_frame_.w); Image* image=Target(front,false);if(image) {CpuWrite(*image);shown_=ResourceKey(front.data,false);}
        if (!keeping_pictures_ && next_picture_ < generated_count_ && last_picture_ >= 0)
            Display(&generated_[last_picture_],&generated_masks_[last_picture_]);
        last_picture_ = keeping_pictures_ && (taken_ & (1u << (generated_count_ - 1))) ? int(generated_count_ - 1) : -1;
        keeping_pictures_ = false;
        if (image && last_picture_ < 0) FinishPicture(*image,hdr_mask_);
        if(g_capture.exchange(false))BeginEffectCapture(last_picture_ < 0 ? image : &generated_[last_picture_]);
        else if(effect_trace_active_ && diag::Env("XBE_D3D11_TRACE_FRAME")) {
            LOGR("d3d11: captured one frame, %zu materials",traced_effects_.size());effect_trace_active_=false;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto pictures = last_picture_ < 0 ? 1 : std::popcount(taken_);
        using Duration = std::chrono::steady_clock::duration;
        const auto elapsed = last_ == decltype(last_){} ? Duration(std::chrono::milliseconds(16)) : now - last_;
        picture_interval_ = std::clamp(elapsed, Duration(std::chrono::milliseconds(4)),
                                       Duration(std::chrono::milliseconds(50))) / pictures;
        last_ = now;
        {
            PerfScope timing{g_perf.present};
            if (last_picture_ < 0) {
                next_picture_ = generated_count_;
                Display(image,&hdr_mask_);
            } else {
                next_picture_ = 0;
                schedule_ = now;
                Show();
            }
            EndShadingFrame();
            PollReports();
            ApplyRenderScale();
        }
        if(diag::Perf()) {
            LogPerf();driver_frames_++;
            if(PerfMs(driver_start_)>=1000) {
                double n=driver_frames_;
                LOGR("d3d11 timing ms/frame: prepare %.2f targets %.2f vs %.2f textures/samplers %.2f ps/pipeline %.2f stream %.2f constants %.2f bind %.2f submit/queries %.2f",
                    driver_timing_[0]/n,driver_timing_[1]/n,driver_timing_[2]/n,driver_timing_[3]/n,driver_timing_[4]/n,
                    driver_timing_[5]/n,driver_timing_[6]/n,driver_timing_[7]/n,driver_timing_[8]/n);
                std::fill(std::begin(driver_timing_),std::end(driver_timing_),0.0);driver_frames_=0;driver_start_=PerfClock::now();
            }
        }
        frame_++;
        if(frame_%120==0) {for(auto it=textures_.begin();it!=textures_.end();) if(it->second.frame+120<frame_) it=textures_.erase(it); else ++it;}
    }
    void Redisplay(bool=true) override {
        next_picture_ = generated_count_;
        Display(last_picture_ < 0 ? nullptr : &generated_[last_picture_],
                last_picture_ < 0 ? &hdr_mask_ : &generated_masks_[last_picture_],false);
        ApplyRenderScale();
        PollReports();
    }
    uint32_t Pictures() override {
        if (!swap_ || effect_trace_active_ || g_capture.load() || IsIconic(static_cast<HWND>(app::NativeWindow()))) return 1;
        const auto& set = app::Get();
        const uint32_t wanted = std::clamp(set.frame_generation,1u,4u);
        if (!set.vsync || wanted == 1) return wanted;
        // A 60 Hz display with VSync can present one picture per 60 Hz game frame.
        return std::min(wanted,std::max(1u,(app::MonitorRefresh()+1)/60));
    }
    // own is always true here: no frame is drawn light (Light) and the last picture time is the frame's own
    void Keep(const Surface& front, bool /*own*/) override {
        if (next_picture_ < generated_count_ && last_picture_ >= 0)
            Display(&generated_[last_picture_],&generated_masks_[last_picture_]);
        generated_count_ = std::clamp(app::Get().frame_generation,2u,4u);
        if (app::Get().vsync)
            generated_count_ = std::clamp((app::MonitorRefresh()+1)/60,2u,generated_count_);
        next_picture_ = generated_count_;
        taken_ = 0;
        keeping_pictures_ = true;
        KeepPicture(front,generated_count_ - 1);
    }
    void Between(const Surface& front, uint32_t pass) override {
        if (pass < generated_count_) KeepPicture(front,pass - 1);
    }
    void Replaying(bool on) override {
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
        const auto now = std::chrono::steady_clock::now();
        while (next_picture_ < generated_count_ && schedule_ <= now) {
            const uint32_t slot = next_picture_++;
            schedule_ += picture_interval_;
            if ((taken_ & (1u << slot)) && (schedule_ > now || next_picture_ == generated_count_)) {
                Display(&generated_[slot],&generated_masks_[slot]);
                break;
            }
        }
    }
    void Sync(uint32_t data,uint32_t) override {
        PerfScope timing{g_perf.sync};
        if(!ctx_)return;
        auto it=targets_.find(ResourceKey(data,false));
        if (it == targets_.end() || !it->second.drawn) return;
        Image& im = it->second;
        if (!Resolve(im)) return;
        TextureDesc guest;
        if (!DescribeSurface(im.surface, guest) || Format(guest.format).host != Host::BGRA8 || !guest.linear)
            return;
        g_perf.syncs++;
        Unbind();
        const Image* source = &im;
        if (im.w != im.guest.w || im.h != im.guest.h) {
            if (!NativeColor(im.guest) ||
                !BlitColor(im, WholeImage(im), native_color_, WholeImage(native_color_), copy_sampler_.get()))
                return;
            source = &native_color_;
        }
        D3D11_TEXTURE2D_DESC d{};d.Width=im.guest.w;d.Height=im.guest.h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;if(!Check(dev_->CreateTexture2D(&d,nullptr,staging.put()),"readback"))return;
        ctx_->CopyResource(staging.get(),source->resource.get());D3D11_MAPPED_SUBRESOURCE m{};
        if(!Check(ctx_->Map(staging.get(),0,D3D11_MAP_READ,0,&m),"readback map"))return;
        const uint32_t pitch = guest.pitch ? guest.pitch : GuestPitch(im.guest.w);
        auto* out = static_cast<uint8_t*>(kernel::HostPtr(guest.data));
        for (uint32_t y = 0; y < im.guest.h; ++y)
            std::memcpy(out + size_t(y) * pitch, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                        im.guest.w * 4);
        ctx_->Unmap(staging.get(),0);im.drawn=false;im.epoch=DirtyEpoch(im.data);
        im.signature=MemSig(guest.data,TotalSize(guest));
    }
    void SyncLate(uint32_t data) override {Sync(data,0);}
    void Flush() override {if(ctx_) {ctx_->Flush();PollReports(true);}}
    void SetGamma(const uint8_t*) override {} // Presentation gamma comes from app::Settings.
    void ClearReport() override {counted_.clear();}
    void GetReport(uint32_t report) override {
        if(!ctx_)return;
        reports_.push_back({report,counted_}); PollReports();
    }
};
} // namespace
std::unique_ptr<Backend> CreateD3D11Backend() {return std::make_unique<D3D11Backend>();}
} // namespace gpu
#endif
