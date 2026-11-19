// Compile representative Conker shader translations with Windows' real HLSL compiler.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include "app/app.h"
#include "gpu/shaders.h"
#include "gpu/d3d11_shaders.h"
namespace app { Settings& Get() { static Settings settings; return settings; } }
// Link fixture for the translator's depth-as-color code (the real mapping from draw.cpp).
namespace gpu { void DepthLayoutOrder(int layout,float* order) {
    static const float values[4][4]={{2,1,0,3},{0,1,2,3},{1,2,3,0},{3,2,1,0}};
    for(int i=0;i<4;i++)order[i]=values[layout&3][i];
} }
template<class T>struct TestCom { T* p=nullptr;~TestCom(){if(p)p->Release();}T** put(){return &p;}T* operator->(){return p;} };
// Exercise the actual point geometry shader on the GPU. Compilation alone did
// not catch the HD 4000's corrupt UVs from the dynamically emitted corner loop.
bool TestPointCoordinates(ID3D11Device* device,ID3D11DeviceContext* context) {
    std::string geometry=gpu::PointHlsl();auto common=geometry.substr(0,geometry.find("[maxvertexcount"));
    auto compile=[](const std::string& source,const char* profile,TestCom<ID3DBlob>& code){
        TestCom<ID3DBlob> error;HRESULT hr=D3DCompile(source.data(),source.size(),"point coordinates",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),error.put());
        if(FAILED(hr)&&error.p)std::printf("point test: %s\n",static_cast<const char*>(error->GetBufferPointer()));return SUCCEEDED(hr);
    };
    TestCom<ID3DBlob> vs_code,gs_code,ps_code;
    if(!compile(common+"VOut main(uint n:SV_VertexID){VOut o=(VOut)0;o.pos=float4(0,0,0.5,1);o.psize=32;return o;}","vs_5_0",vs_code) ||
       !compile(geometry,"gs_5_0",gs_code) ||
       !compile(common+"float4 main(VOut i):SV_Target{return float4(i.tc0.xy/i.tc0.w,0,1);}","ps_5_0",ps_code))return false;
    TestCom<ID3D11VertexShader> vs;TestCom<ID3D11GeometryShader> gs;TestCom<ID3D11PixelShader> ps;
    if(FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(),vs_code->GetBufferSize(),nullptr,vs.put())) ||
       FAILED(device->CreateGeometryShader(gs_code->GetBufferPointer(),gs_code->GetBufferSize(),nullptr,gs.put())) ||
       FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(),ps_code->GetBufferSize(),nullptr,ps.put())))return false;
    gpu::DrawConstants constants{};constants.surf[0]=constants.surf[1]=64;constants.pts[3]=64;constants.ptsk[3]=2;constants.ptpx[0]=constants.ptpx[1]=1;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(constants);bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{&constants,0,0};TestCom<ID3D11Buffer> cb;if(FAILED(device->CreateBuffer(&bd,&initial,cb.put())))return false;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    TestCom<ID3D11Texture2D> target,staging;TestCom<ID3D11RenderTargetView> view;
    if(FAILED(device->CreateTexture2D(&td,nullptr,target.put())) || FAILED(device->CreateRenderTargetView(target.p,nullptr,view.put())))return false;
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;if(FAILED(device->CreateTexture2D(&td,nullptr,staging.put())))return false;
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    TestCom<ID3D11RasterizerState> raster;if(FAILED(device->CreateRasterizerState(&rd,raster.put())))return false;
    context->ClearState();context->GSSetConstantBuffers(1,1,&cb.p);context->VSSetShader(vs.p,nullptr,0);context->GSSetShader(gs.p,nullptr,0);context->PSSetShader(ps.p,nullptr,0);
    context->RSSetState(raster.p);D3D11_VIEWPORT viewport{0,0,64,64,0,1};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    float black[4]={0,0,0,1};context->ClearRenderTargetView(view.p,black);context->OMSetRenderTargets(1,&view.p,nullptr);context->Draw(1,0);context->OMSetRenderTargets(0,nullptr,nullptr);
    context->CopyResource(staging.p,target.p);D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)))return false;
    bool passed=true;
    for(int y:{24,40})for(int x:{24,40}) {
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
        int expected_r=x==24?71:196,expected_g=y==24?71:196;
        std::printf("Point UV at %d,%d: %u,%u (expected %d,%d)\n",x,y,unsigned(pixel[0]),unsigned(pixel[1]),expected_r,expected_g);
        if(std::abs(int(pixel[0])-expected_r)>10 || std::abs(int(pixel[1])-expected_g)>10)passed=false;
    }
    context->Unmap(staging.p,0);context->ClearState();std::printf("%s native point-sprite coordinates\n",passed?"PASS":"FAIL");return passed;
}
// Conker's glow extraction reads stencil as texture alpha. A zero here removes
// the entire glow chain even though its shaders and color buffers are valid.
bool TestDepthStencilBytes(ID3D11Device* device,ID3D11DeviceContext* context) {
    auto compile=[](const char* text,const char* profile,TestCom<ID3DBlob>& code){
        TestCom<ID3DBlob> errors;
        HRESULT hr=D3DCompile(text,std::strlen(text),"depth bytes",nullptr,nullptr,"main",profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),errors.put());
        if(FAILED(hr)&&errors.p)std::printf("depth bytes: %s\n",static_cast<const char*>(errors->GetBufferPointer()));
        return SUCCEEDED(hr);
    };
    TestCom<ID3DBlob> vs_code,ps_code;
    if(!compile("float4 main(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}","vs_5_0",vs_code) ||
       !compile(gpu::d3d11::kDepthBytes,"ps_5_0",ps_code))return false;
    TestCom<ID3D11VertexShader> vs;TestCom<ID3D11PixelShader> ps;
    if(FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(),vs_code->GetBufferSize(),nullptr,vs.put())) ||
       FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(),ps_code->GetBufferSize(),nullptr,ps.put())))return false;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R24G8_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    TestCom<ID3D11Texture2D> depth,target,staging,depth_snapshot;TestCom<ID3D11DepthStencilView> dsv;
    TestCom<ID3D11ShaderResourceView> depth_view,stencil_view,stencil_snapshot,depth_snapshot_view;TestCom<ID3D11RenderTargetView> rtv;
    if(FAILED(device->CreateTexture2D(&td,nullptr,depth.put())))return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    if(FAILED(device->CreateDepthStencilView(depth.p,&dd,dsv.put())))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    sd.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;if(FAILED(device->CreateShaderResourceView(depth.p,&sd,depth_view.put())))return false;
    sd.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;if(FAILED(device->CreateShaderResourceView(depth.p,&sd,stencil_view.put())))return false;
    auto snapshot_desc=td;snapshot_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(device->CreateTexture2D(&snapshot_desc,nullptr,depth_snapshot.put())) || FAILED(device->CreateShaderResourceView(depth_snapshot.p,&sd,stencil_snapshot.put())))return false;
    sd.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if(FAILED(device->CreateShaderResourceView(depth_snapshot.p,&sd,depth_snapshot_view.put())))return false;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(device->CreateTexture2D(&td,nullptr,target.put())) || FAILED(device->CreateRenderTargetView(target.p,nullptr,rtv.put())))return false;
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateTexture2D(&td,nullptr,staging.put())))return false;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    TestCom<ID3D11Buffer> cb;if(FAILED(device->CreateBuffer(&bd,nullptr,cb.put())))return false;
    context->ClearState();context->VSSetShader(vs.p,nullptr,0);context->PSSetShader(ps.p,nullptr,0);
    context->PSSetConstantBuffers(0,1,&cb.p);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,4,4,0,1};context->RSSetViewports(1,&vp);
    bool passed=true;
    for(float z:{0.0f,0.25f,0.5f,1.0f})for(unsigned stencil:{0u,32u,176u,255u})for(int layout=0;layout<4;layout++) {
        context->ClearDepthStencilView(dsv.p,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,z,UINT8(stencil));
        context->CopyResource(depth_snapshot.p,depth.p);
        float mapping[4];gpu::DepthLayoutOrder(layout,mapping);unsigned order[4];for(int i=0;i<4;i++)order[i]=unsigned(mapping[i]);
        context->UpdateSubresource(cb.p,0,nullptr,order,0,0);
        ID3D11ShaderResourceView* views[2]={depth_snapshot_view.p,stencil_snapshot.p};context->PSSetShaderResources(0,2,views);
        context->OMSetRenderTargets(1,&rtv.p,nullptr);context->Draw(3,0);context->OMSetRenderTargets(0,nullptr,nullptr);
        ID3D11ShaderResourceView* empty[2]{};context->PSSetShaderResources(0,2,empty);
        context->CopyResource(staging.p,target.p);D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(context->Map(staging.p,0,D3D11_MAP_READ,0,&mapped)))return false;
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData);
        unsigned iz=z==0?0:z==0.25f?0x400000:z==0.5f?0x800000:0xFFFFFF;
        const unsigned bytes[4]={stencil,iz&255,(iz>>8)&255,(iz>>16)&255};
        bool correct=true;for(int i=0;i<4;i++)correct&=pixel[i]==bytes[order[i]];
        if(!correct)std::printf("FAIL depth/stencil bytes layout %d z %g stencil %u: %u,%u,%u,%u\n",layout,z,stencil,pixel[0],pixel[1],pixel[2],pixel[3]);
        passed&=correct;context->Unmap(staging.p,0);
    }
    context->ClearState();std::printf("%s native depth/stencil byte layouts\n",passed?"PASS":"FAIL");return passed;
}
int main() {
    ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr;
    D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0, actual{};
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&requested,1,
        D3D11_SDK_VERSION,&device,&actual,&context);
    if(FAILED(hr)) {std::printf("Hardware device failed: %08X\n",unsigned(hr));return 1;}
    std::printf("Hardware D3D11 feature level: %X\n",unsigned(actual));
    UINT bgra=0,rgba=0; device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM,&bgra);
    device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM,&rgba);
    std::printf("Vertex color format support: BGRA=%u, RGBA=%u\n",unsigned((bgra&D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER)!=0),unsigned((rgba&D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER)!=0));
    // A tiny BC texture must be expanded to BGRA instead of uploaded as a 2x2 BC resource.
    unsigned char pixels[16]={};D3D11_SUBRESOURCE_DATA upload{pixels,8,0};ID3D11Texture2D* texture=nullptr;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=2;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_BC1_UNORM;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    HRESULT tiny=device->CreateTexture2D(&td,&upload,&texture);if(texture)texture->Release();texture=nullptr;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT expanded=device->CreateTexture2D(&td,&upload,&texture);if(texture)texture->Release();
    std::printf("2x2 BC1=0x%08X, expanded BGRA=0x%08X\n",unsigned(tiny),unsigned(expanded));
    if(!(rgba&D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER)||FAILED(expanded)) {context->Release();device->Release();return 1;}
    unsigned count=0, failures=0;
    if(!TestPointCoordinates(device,context))failures++;
    if(!TestDepthStencilBytes(device,context))failures++;
    // Xbox SHORT3 streams pack attributes on two-byte boundaries. D3D11's
    // eight-byte input formats require four-byte alignment, unlike Vulkan.
    const char* short_source="float4 main(int4 a:ATTR0,int4 b:ATTR1,int4 c:ATTR2):SV_Position { return float4(a.xyz+b.xyz+c.xyz,1); }";
    ID3DBlob* short_code=nullptr;
    HRESULT short_compile=D3DCompile(short_source,std::strlen(short_source),"short3",nullptr,nullptr,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&short_code,nullptr);
    if(SUCCEEDED(short_compile)) {
        D3D11_INPUT_ELEMENT_DESC elements[3]{};
        for(unsigned i=0;i<3;i++)elements[i]={"ATTR",i,DXGI_FORMAT_R16G16B16A16_SINT,0,i*6,D3D11_INPUT_PER_VERTEX_DATA,0};
        ID3D11InputLayout* layout=nullptr;
        HRESULT packed=device->CreateInputLayout(elements,3,short_code->GetBufferPointer(),short_code->GetBufferSize(),&layout);
        if(layout)layout->Release();layout=nullptr;
        for(unsigned i=0;i<3;i++)elements[i].AlignedByteOffset=i*8;
        HRESULT aligned=device->CreateInputLayout(elements,3,short_code->GetBufferPointer(),short_code->GetBufferSize(),&layout);
        if(layout)layout->Release();short_code->Release();
        std::printf("SHORT3 input layout: packed=0x%08X, aligned=0x%08X\n",unsigned(packed),unsigned(aligned));
        if(FAILED(aligned))failures++;
    } else failures++;
    auto test=[&](const char* name,const std::string& source,const char* profile,const char* entry="main") {
        ID3DBlob *code=nullptr,*error=nullptr;
        HRESULT result=D3DCompile(source.data(),source.size(),name,nullptr,nullptr,entry,profile,
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(result)) {std::printf("FAIL %s: %s\n",name,error?static_cast<const char*>(error->GetBufferPointer()):"no error text");failures++;}
        else {std::printf("PASS %s (%zu bytes)\n",name,code->GetBufferSize());}
        if(code)code->Release(); if(error)error->Release(); count++;
    };
    test("fixed transform",gpu::FixedVertexHlsl(0x1F,false),"vs_5_0");
    test("pretransformed",gpu::FixedVertexHlsl(0x1F,true),"vs_5_0");
    test("register program",gpu::VertexProgramHlsl({},0xFFFF),"vs_5_0");
    test("point sprite geometry",gpu::PointHlsl(),"gs_5_0");
    test("scaled color copy", gpu::d3d11::kCopyColor, "ps_5_0");
    test("scaled depth copy", gpu::d3d11::kCopyDepthStencil, "ps_5_0", "copy_depth");
    test("scaled stencil copy", gpu::d3d11::kCopyDepthStencil, "ps_5_0", "copy_stencil");
    for(int kind=0;kind<=3;kind++) {
        gpu::PixelKey key{};key.ff[0][0]=4;key.ff[0][1]=2;key.ff[0][2]=0;
        key.ff[0][4]=2;key.ff[0][5]=2;key.tex[0].kind=uint8_t(kind);
        char name[40];std::snprintf(name,sizeof(name),"fixed pixel texture kind %d",kind);
        test(name,gpu::PixelHlsl(key),"ps_5_0");
    }
    gpu::PixelKey key{};key.programmable=1;key.count=1;key.tex_modes=1;key.tex[0].kind=gpu::kTex2D;
    test("register combiners",gpu::PixelHlsl(key),"ps_5_0");
    key.tex[0].shadow=1;
    test("depth comparison texture",gpu::PixelHlsl(key),"ps_5_0");
    context->Release();device->Release();
    std::printf("%u shader cases, %u failures\n",count,failures);
    return failures?1:0;
}
