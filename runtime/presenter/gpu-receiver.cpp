#include "gpu-receiver.h"
#include "sdl-blit-shaders.h"
#include "pointer-cursor.h"
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {
struct Vertex { float x,y,u,v; UINT color; };
struct SharedSlot {
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Fence> ready, done;
    UINT64 retired = 0;
};
struct CloseHandleWhenDone {
    HANDLE value = nullptr;
    ~CloseHandleWhenDone() { if (value) CloseHandle(value); }
};
LONG load(volatile LONG *value) { return InterlockedCompareExchange(value,0,0); }
UINT64 load64(volatile LONG64 *value) { return static_cast<UINT64>(InterlockedCompareExchange64(value,0,0)); }

HRESULT openSlot(ID3D12Device *lookup, ID3D11Device5 *device, SharedSlot &slot,
                 WCHAR const (&names)[3][64], UINT width, UINT height, DXGI_FORMAT format) {
    for (int i=0; i<3; ++i) {
        if (!names[i][0] || names[i][63]) return E_INVALIDARG;
        CloseHandleWhenDone handle;
        HRESULT hr=lookup->OpenSharedHandleByName(names[i],GENERIC_ALL,&handle.value);
        if (FAILED(hr)) return hr;
        if (!i) {
            ComPtr<ID3D11Texture2D> texture;
            hr=device->OpenSharedResource1(handle.value,IID_PPV_ARGS(&texture));
            if (FAILED(hr)) return hr;
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width!=width || desc.Height!=height || desc.Format!=format || desc.MipLevels!=1 ||
                    desc.ArraySize!=1 || desc.SampleDesc.Count!=1) return E_INVALIDARG;
            hr=device->CreateShaderResourceView(texture.Get(),nullptr,&slot.srv);
        } else {
            auto &fence = i==1 ? slot.ready : slot.done;
            hr=device->OpenSharedFence(handle.value,IID_PPV_ARGS(&fence));
        }
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
HRESULT makeBuffer(ID3D11Device *device, UINT bind, const void *bytes, UINT size, ID3D11Buffer **out) {
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth=size; desc.BindFlags=bind; desc.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=bytes;
    return device->CreateBuffer(&desc,&initial,out);
}
}
struct HostedGpuReceiver::Resources {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> fences;
    ComPtr<ID3D11Texture2D> cpuTexture;
    ComPtr<ID3D11ShaderResourceView> cpuView;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> vertices, cursorVertices, matrices;
    ComPtr<ID3D11ShaderResourceView> cursorView, pointerView[2];
    UINT cursorShape=0;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> scissor;
    ComPtr<ID3D11BlendState> rgbOnly, cursorBlend;
    SharedSlot slots[SDL_GPU_SLOTS];
    UINT width=0, height=0;
    UINT64 latest=0;
    HRESULT createPipeline(ID3D11Device *device, bool flip) {
        HRESULT hr=device->CreateVertexShader(D3D11_VertexShader,sizeof(D3D11_VertexShader),nullptr,&vertex);
        if (FAILED(hr)) return hr;
        hr=device->CreatePixelShader(D3D11_PixelShader_Textures,sizeof(D3D11_PixelShader_Textures),nullptr,&pixel);
        if (FAILED(hr)) return hr;
        const D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        hr=device->CreateInputLayout(elements,ARRAYSIZE(elements),D3D11_VertexShader,sizeof(D3D11_VertexShader),&layout);
        if (FAILED(hr)) return hr;
        const float top=flip?1.f:0.f, bottom=1.f-top;
        const Vertex quad[]={{-1,1,0,top,~0u},{1,1,1,top,~0u},{-1,-1,0,bottom,~0u},{1,-1,1,bottom,~0u}};
        hr=makeBuffer(device,D3D11_BIND_VERTEX_BUFFER,quad,sizeof(quad),&vertices); if (FAILED(hr)) return hr;
        const Vertex cursorQuad[]={{-1,1,0,0,~0u},{1,1,1,0,~0u},{-1,-1,0,1,~0u},{1,-1,1,1,~0u}};
        hr=makeBuffer(device,D3D11_BIND_VERTEX_BUFFER,cursorQuad,sizeof(cursorQuad),&cursorVertices); if (FAILED(hr)) return hr;
        float identity[32]{};
        for (int i=0;i<4;i++) identity[i*5]=identity[16+i*5]=1.f;
        hr=makeBuffer(device,D3D11_BIND_CONSTANT_BUFFER,identity,sizeof(identity),&matrices); if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD=D3D11_FLOAT32_MAX; sampling.ComparisonFunc=D3D11_COMPARISON_NEVER;
        hr=device->CreateSamplerState(&sampling,&sampler); if (FAILED(hr)) return hr;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr=device->CreateBlendState(&blend,&rgbOnly); if (FAILED(hr)) return hr;
        auto &rt=blend.RenderTarget[0];
        rt.BlendEnable=TRUE; rt.SrcBlend=D3D11_BLEND_SRC_ALPHA; rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD; rt.SrcBlendAlpha=D3D11_BLEND_ZERO; rt.DestBlendAlpha=D3D11_BLEND_ONE;
        hr=device->CreateBlendState(&blend,&cursorBlend); if (FAILED(hr)) return hr;
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
        raster.DepthClipEnable=TRUE; raster.ScissorEnable=TRUE;
        return device->CreateRasterizerState(&raster,&scissor);
    }
    HRESULT makeCursor(ID3D11Device *device, UINT w, UINT h, const void *pixels,
                       ID3D11ShaderResourceView **view) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=w; desc.Height=h; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=pixels; data.SysMemPitch=w*4;
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr=device->CreateTexture2D(&desc,&data,&texture);
        return FAILED(hr) ? hr : device->CreateShaderResourceView(texture.Get(),nullptr,view);
    }
    HRESULT prepareCursor(ID3D11Device *device, LONG hostPointer, const SdlGpuCursor &cursor) {
        if (hostPointer >= 0) {
            static constexpr auto idle = FhdPointer::image(false), pressed = FhdPointer::image(true);
            for (unsigned i=0; i<2; ++i) {
                if (!pointerView[i]) {
                    HRESULT hr=makeCursor(device,FhdPointer::size,FhdPointer::size,
                                          i ? pressed.data() : idle.data(),&pointerView[i]);
                    if (FAILED(hr)) return hr;
                }
            }
        } else if (cursor.shape && cursor.shape != cursorShape) {
            if (!cursor.width || !cursor.height || cursor.width>SDL_GPU_CURSOR_SIZE || cursor.height>SDL_GPU_CURSOR_SIZE) return E_INVALIDARG;
            ComPtr<ID3D11ShaderResourceView> view;
            HRESULT hr=makeCursor(device,cursor.width,cursor.height,cursor.rgba,&view);
            if (FAILED(hr)) return hr;
            cursorView=view; cursorShape=cursor.shape;
        }
        return S_OK;
    }
    void draw(ID3D11RenderTargetView *target, UINT w, UINT h, ID3D11ShaderResourceView *source, const SdlGpuCursor &cursor, LONG hostPointer) {
        float scale=(std::min)(float(w)/width,float(h)/height);
        D3D11_VIEWPORT viewport{(w-width*scale)*0.5f,(h-height*scale)*0.5f,width*scale,height*scale,0.f,1.f};
        const float opaqueBlack[]={0,0,0,1};
        context->ClearRenderTargetView(target,opaqueBlack);
        context->OMSetRenderTargets(1,&target,nullptr);
        context->OMSetBlendState(rgbOnly.Get(),nullptr,~0u);
        context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(layout.Get());
        UINT stride=sizeof(Vertex), offset=0;
        context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vertex.Get(),nullptr,0);
        context->VSSetConstantBuffers(0,1,matrices.GetAddressOf());
        context->PSSetShader(pixel.Get(),nullptr,0);
        context->PSSetSamplers(0,1,sampler.GetAddressOf());
        context->PSSetShaderResources(0,1,&source);
        context->Draw(4,0);
        ID3D11ShaderResourceView *overlayView = hostPointer >= 0 ? pointerView[(hostPointer >> 24) & 1].Get() :
                                                cursor.shape ? cursorView.Get() : nullptr;
        if (overlayView) {
            // Clip the cursor to the game's letterboxed viewport if it straddles an edge.
            const D3D11_RECT bounds{static_cast<LONG>(viewport.TopLeftX),static_cast<LONG>(viewport.TopLeftY),
                static_cast<LONG>(viewport.TopLeftX+viewport.Width),static_cast<LONG>(viewport.TopLeftY+viewport.Height)};
            D3D11_VIEWPORT overlay{viewport.TopLeftX + cursor.x*scale, viewport.TopLeftY + cursor.y*scale,
                                   cursor.width*scale,cursor.height*scale,0.f,1.f};
            if (hostPointer >= 0) {
                // Keep the marker at a readable display-pixel size, centered at
                // the SDL hotspot after the same aspect fit used for the game.
                const auto x=FhdPointer::pixel(hostPointer,width), y=FhdPointer::pixel(hostPointer >> 12,height);
                overlay.TopLeftX=viewport.TopLeftX + (x + .5f)*scale - (FhdPointer::center + .5f);
                overlay.TopLeftY=viewport.TopLeftY + (y + .5f)*scale - (FhdPointer::center + .5f);
                overlay.Width=overlay.Height=FhdPointer::size;
            }
            context->RSSetState(scissor.Get());
            context->RSSetScissorRects(1,&bounds); context->RSSetViewports(1,&overlay);
            context->OMSetBlendState(cursorBlend.Get(),nullptr,~0u);
            context->IASetVertexBuffers(0,1,cursorVertices.GetAddressOf(),&stride,&offset);
            context->PSSetShaderResources(0,1,&overlayView);
            context->Draw(4,0);
            context->RSSetState(nullptr);
        }
        source=nullptr; context->PSSetShaderResources(0,1,&source);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};

HostedGpuReceiver::~HostedGpuReceiver() { Close(); }
UINT64 HostedGpuReceiver::ProducerSequence() const {
    if (!mailbox_) return 0;
    const auto first = load64(&mailbox_->sequence[0]);
    const auto second = load64(&mailbox_->sequence[1]);
    return first > second ? first : second;
}
HRESULT HostedGpuReceiver::Open(const wchar_t *mailboxPath) {
    if (mailbox_ || !mailboxPath) return E_INVALIDARG;
    WCHAR path[1024]; UINT length=0;
    while (mailboxPath[length] && length<ARRAYSIZE(path)-5) {path[length]=mailboxPath[length]; ++length;}
    if (mailboxPath[length]) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    const WCHAR extension[]=L".gpu"; std::memcpy(path+length,extension,sizeof(extension));
    CREATEFILE2_EXTENDED_PARAMETERS flags{}; flags.dwSize=sizeof(flags);
    flags.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY; flags.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE;
    file_=CreateFile2(path,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                      CREATE_NEW,&flags);
    bool created=true;
    if (file_==INVALID_HANDLE_VALUE && GetLastError()==ERROR_FILE_EXISTS) {
        created=false;
        file_=CreateFile2(path,GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,OPEN_EXISTING,nullptr);
    }
    if (file_==INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!created) {
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_,&length) || length.QuadPart!=sizeof(SdlGpuMailbox)) {Close();return E_INVALIDARG;}
    }
    CloseHandleWhenDone mapping;
    mapping.value=CreateFileMappingFromApp(file_,nullptr,PAGE_READWRITE,sizeof(SdlGpuMailbox),nullptr);
    if (mapping.value) mailbox_=static_cast<SdlGpuMailbox*>(MapViewOfFileFromApp(mapping.value,FILE_MAP_WRITE,0,sizeof(SdlGpuMailbox)));
    if (!mailbox_) { auto hr=HRESULT_FROM_WIN32(GetLastError()); Close(); return hr; }
    if (!created && load(&mailbox_->magic)!=SDL_GPU_MAGIC) {Close();return E_INVALIDARG;}
    InterlockedExchange(&mailbox_->supports_cursor,1);
    InterlockedExchange(&mailbox_->magic,SDL_GPU_MAGIC);
    stage_="waiting-for-producer";
    return S_OK;
}
bool HostedGpuReceiver::Active() const {
    return accepted_ && mailbox_ && load(&mailbox_->gpu_active)!=0;
}
void HostedGpuReceiver::Disconnect() {
    if (mailbox_ && accepted_) InterlockedCompareExchange(&mailbox_->accepted_generation,0,accepted_);
    accepted_=0;
    if (resources_) {resources_->~Resources();HeapFree(GetProcessHeap(),0,resources_);resources_=nullptr;}
}
void HostedGpuReceiver::Close() {
    Disconnect();
    if (mailbox_) { UnmapViewOfFile(mailbox_); mailbox_=nullptr; }
    if (file_!=INVALID_HANDLE_VALUE) { CloseHandle(file_); file_=INVALID_HANDLE_VALUE; }
    if (cpu_) {cpu_->~Resources();HeapFree(GetProcessHeap(),0,cpu_);cpu_=nullptr;}
    failedGeneration_=0; stage_="closed";
}

HRESULT HostedGpuReceiver::Connect(ID3D11Device *device, ID3D11DeviceContext *context, LONG generation) {
    stage_="read-descriptor";
    void *memory=HeapAlloc(GetProcessHeap(),0,sizeof(Resources));
    if (!memory) return E_OUTOFMEMORY;
    resources_=new (memory) Resources;
    auto &r=*resources_;
    r.width=mailbox_->width; r.height=mailbox_->height;
    auto format=static_cast<DXGI_FORMAT>(mailbox_->dxgi_format);
    if (!r.width || !r.height || r.width>16384 || r.height>16384 ||
            (format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return E_INVALIDARG;
    WCHAR names[SDL_GPU_SLOTS][3][64]{};
    std::memcpy(names,mailbox_->names,sizeof(names));
    const bool flip=mailbox_->flip_y!=0;
    const UINT64 luid=mailbox_->adapter_luid;
    if (load(&mailbox_->generation)!=generation) return S_FALSE;
    stage_="d3d11-fence-interface";
    ComPtr<ID3D11Device5> modern; HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&modern));
    if (FAILED(hr)) return hr;
    r.context=context; hr=context->QueryInterface(IID_PPV_ARGS(&r.fences)); if (FAILED(hr)) return hr;
    stage_="match-adapter";
    ComPtr<IDXGIDevice> dxgi; hr=device->QueryInterface(IID_PPV_ARGS(&dxgi)); if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter> adapter; hr=dxgi->GetAdapter(&adapter); if (FAILED(hr)) return hr;
    ComPtr<IDXGIAdapter1> adapterInfo;
    hr=adapter->QueryInterface(IID_PPV_ARGS(&adapterInfo)); if (FAILED(hr)) return hr;
    DXGI_ADAPTER_DESC1 desc{}; hr=adapterInfo->GetDesc1(&desc); if (FAILED(hr)) return hr;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
        stage_="software-adapter";
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (std::memcmp(&desc.AdapterLuid,&luid,sizeof(luid))) return DXGI_ERROR_UNSUPPORTED;
    stage_="d3d12-shared-handle-lookup";
    ComPtr<ID3D12Device> lookup;
    hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&lookup)); if (FAILED(hr)) return hr;
    stage_="open-texture-and-fences";
    for (UINT i=0;i<SDL_GPU_SLOTS;i++) {
        hr=openSlot(lookup.Get(),modern.Get(),r.slots[i],names[i],r.width,r.height,format);
        if (FAILED(hr)) return hr;
    }
    stage_="texture-blit-pipeline";
    hr=r.createPipeline(device,flip); if (FAILED(hr)) return hr;
    if (load(&mailbox_->generation)!=generation) return S_FALSE;
    InterlockedExchange(&mailbox_->heartbeat_ms,static_cast<LONG>(GetTickCount64()));
    InterlockedExchange(&mailbox_->accepted_generation,generation);
    accepted_=generation; stage_="shared-textures-ready";
    return S_OK;
}

HRESULT HostedGpuReceiver::Draw(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
                                UINT width, UINT height, UINT64 &sequence, LONG hostPointer) {
    sequence=0;
    if (!mailbox_ || !device || !context || !target || !width || !height) return S_FALSE;
    const LONG generation=load(&mailbox_->generation);
    if (generation!=accepted_) Disconnect();
    if (generation<=0 || (generation&1) || generation==failedGeneration_) return S_FALSE;
    if (!resources_) {
        HRESULT hr=Connect(device,context,generation);
        if (hr!=S_OK) { Disconnect(); if (FAILED(hr)) failedGeneration_=generation; return hr; }
    }
    InterlockedExchange(&mailbox_->heartbeat_ms,static_cast<LONG>(GetTickCount64()));
    auto &r=*resources_;
    UINT64 seq[SDL_GPU_SLOTS]{};
    // Draw the oldest completed frame newer than the last one shown and keep a
    // newer one for the next refresh. When producer and display clocks drift,
    // two frames can complete within one refresh; showing the newest would drop
    // the other and repeat a frame on the following refresh. The producer skips
    // a slot until it is released, so at most one frame waits here.
    int next=-1;
    for (int i=0;i<SDL_GPU_SLOTS;i++) {
        const UINT64 value=load64(&mailbox_->sequence[i]);
        if (!value || value<=r.slots[i].retired) continue;
        // Poll completed producer work; an unresponsive game cannot block this GPU queue.
        const UINT64 ready=r.slots[i].ready->GetCompletedValue();
        if (ready==UINT64_MAX) {
            stage_="producer-device-removed"; failedGeneration_=generation; Disconnect(); return DXGI_ERROR_DEVICE_REMOVED;
        }
        if (ready<value) continue;
        seq[i]=value;
        if (value>r.latest && (next<0 || value<seq[next])) next=i;
    }
    if (load(&mailbox_->generation)!=generation) { Disconnect(); return S_FALSE; }
    const bool active=load(&mailbox_->gpu_active)!=0;
    if (!active) next=-1;
    for (int i=0;i<SDL_GPU_SLOTS;i++) {
        if (!seq[i]) continue;
        // Hold a newer completed frame for the next refresh.
        if (active && i!=next && seq[i]>r.latest) continue;
        auto &slot=r.slots[i];
        if (i==next) {
            HRESULT wait=r.fences->Wait(slot.ready.Get(),seq[i]);
            if (FAILED(wait)) { stage_="consumer-fence-wait"; failedGeneration_=generation; Disconnect(); return wait; }
            const auto &cursor=mailbox_->cursor[i];
            HRESULT cursorResult=r.prepareCursor(device,hostPointer,cursor);
            if (FAILED(cursorResult)) { stage_="cursor-upload"; failedGeneration_=generation; Disconnect(); return cursorResult; }
            r.draw(target,width,height,slot.srv.Get(),cursor,hostPointer);
        }
        HRESULT hr=r.fences->Signal(slot.done.Get(),seq[i]);
        if (FAILED(hr)) { stage_="consumer-fence-signal"; failedGeneration_=generation; Disconnect(); return hr; }
        slot.retired=seq[i];
    }
    if (!seq[0] && !seq[1]) return S_FALSE;
    context->Flush();
    if (next<0) return S_FALSE;
    lastWidth_=r.width; lastHeight_=r.height;
    sequence=r.latest=seq[next]; stage_="gpu-drew-frame";
    return S_OK;
}


HRESULT HostedGpuReceiver::DrawCpu(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target,
                                   UINT width, UINT height, const BYTE *pixels, UINT sourceWidth, UINT sourceHeight, LONG hostPointer) {
    if (!pixels || !sourceWidth || !sourceHeight || sourceWidth>1920 || sourceHeight>1080) return E_INVALIDARG;
    if (!cpu_) {
        void *memory=HeapAlloc(GetProcessHeap(),0,sizeof(Resources)); if (!memory) return E_OUTOFMEMORY;
        cpu_=new (memory) Resources;
        cpu_->context=ctx;
        HRESULT hr=cpu_->createPipeline(device,false);
        if (FAILED(hr)) {cpu_->~Resources();HeapFree(GetProcessHeap(),0,cpu_);cpu_=nullptr;return hr;}
    }
    auto &r=*cpu_;
    if (!r.cpuView || r.width!=sourceWidth || r.height!=sourceHeight) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=sourceWidth;desc.Height=sourceHeight;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture; HRESULT hr=device->CreateTexture2D(&desc,nullptr,&texture);if (FAILED(hr))return hr;
        ComPtr<ID3D11ShaderResourceView> view; hr=device->CreateShaderResourceView(texture.Get(),nullptr,&view);if (FAILED(hr))return hr;
        r.cpuTexture=texture;r.cpuView=view;r.width=sourceWidth;r.height=sourceHeight;
    }
    ctx->UpdateSubresource(r.cpuTexture.Get(),0,nullptr,pixels,sourceWidth*4,0);
    static const SdlGpuCursor hidden{};
    HRESULT cursorResult=r.prepareCursor(device,hostPointer,hidden);
    if (FAILED(cursorResult)) return cursorResult;
    r.draw(target,width,height,r.cpuView.Get(),hidden,hostPointer);
    lastWidth_=sourceWidth;lastHeight_=sourceHeight;return S_OK;
}
