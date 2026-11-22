#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>
#include "../sdl-gpu-protocol.h"
#include "../sdl-host-protocol.h"

class DxgiFramePublisher {
public:
    DxgiFramePublisher() = default;
    ~DxgiFramePublisher();
    DxgiFramePublisher(const DxgiFramePublisher&) = delete;
    DxgiFramePublisher& operator=(const DxgiFramePublisher&) = delete;
    HRESULT Publish(IDXGISwapChain1* chain);
    bool IsActive() const { return generation_ != 0 && status_ == S_OK; }
    void Reset();

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11Fence> ready, done;
        HANDLE handles[3]{};
        UINT64 submitted = 0;
        void Close();
    };
    HRESULT OpenMailboxes();
    HRESULT Connect(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc);

    SdlHostMailbox* base_ = nullptr;
    SdlGpuMailbox* gpu_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Slot slots_[SDL_GPU_SLOTS];
    LONG generation_ = 0;
    UINT64 next_ = 1;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    HRESULT status_ = E_PENDING;
};
