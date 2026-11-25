#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "../shared/sdl-gpu-protocol.h"
#include "../shared/sdl-host-protocol.h"

class DxgiFramePublisher {
public:
    DxgiFramePublisher() = default;
    ~DxgiFramePublisher();
    DxgiFramePublisher(const DxgiFramePublisher&) = delete;
    DxgiFramePublisher& operator=(const DxgiFramePublisher&) = delete;
    void SetQueue(ID3D12CommandQueue* queue) { queue_ = queue; }
    HRESULT Publish(IDXGISwapChain1* chain, UINT backBuffer);
    bool IsActive() const { return generation_ != 0 && status_ == S_OK; }
    const char* FailureStage() const { return failureStage_; }
    HRESULT Reset();

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11Fence> ready, done;
        Microsoft::WRL::ComPtr<ID3D12Resource> texture12, source12;
        Microsoft::WRL::ComPtr<ID3D12Fence> ready12, done12;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        HANDLE handles[3]{};
        UINT64 queued12 = 0;
        UINT64 submitted = 0;
        void Close();
    };
    HRESULT OpenMailboxes();
    void ClearSlots();
    HRESULT Connect(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc);
    HRESULT Connect(ID3D12Resource* source, const D3D12_RESOURCE_DESC& desc);
    HRESULT CreateSlots(ID3D12Device* share, ID3D11Device5* d3d11, LUID adapter,
        UINT width, UINT height, DXGI_FORMAT format);
    HRESULT Publish11(IDXGISwapChain1* chain);
    HRESULT Publish12(IDXGISwapChain1* chain, UINT backBuffer);
    HRESULT FailedAt(const char* stage, HRESULT error);
    void PublishSequence(unsigned index);
    void ProbeFrame(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc);

    SdlHostMailbox* base_ = nullptr;
    SdlGpuMailbox* gpu_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Slot slots_[SDL_GPU_SLOTS];
    LONG generation_ = 0;
    UINT64 next_ = 1;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    HRESULT status_ = E_PENDING;
    const char* failureStage_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_;
    UINT64 published_ = 0;
};
