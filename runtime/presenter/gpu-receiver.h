#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include "../shared/sdl-gpu-protocol.h"

// Use on the thread owning the dedicated presentation context; drawing replaces its pipeline state.
class HostedGpuReceiver {
public:
    HostedGpuReceiver() = default;
    ~HostedGpuReceiver();
    HostedGpuReceiver(const HostedGpuReceiver&) = delete;
    HostedGpuReceiver& operator=(const HostedGpuReceiver&) = delete;
    // Create the sidecar before launching SDL; path excludes the ".gpu" suffix.
    HRESULT Open(const wchar_t *mailboxPath);
    // S_OK drew a unique sequence. S_FALSE has no new frame. A failure leaves CPU delivery enabled.
    HRESULT Draw(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
                 UINT width, UINT height, UINT64 &sequence, LONG hostPointer = -1);
    HRESULT DrawCpu(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
                    UINT width, UINT height, const BYTE *pixels, UINT sourceWidth, UINT sourceHeight, LONG hostPointer = -1);
    UINT SourceWidth() const { return lastWidth_; }
    UINT SourceHeight() const { return lastHeight_; }
    const char *Stage() const { return stage_; }
    bool Active() const;
    UINT64 ProducerSequence() const;
    void Close();
private:
    struct Resources;
    Resources *resources_ = nullptr, *cpu_ = nullptr;
    UINT lastWidth_=0, lastHeight_=0;
    SdlGpuMailbox *mailbox_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    LONG accepted_ = 0, failedGeneration_ = 0;
    const char *stage_ = "idle";
    HRESULT Connect(ID3D11Device *device, ID3D11DeviceContext *context, LONG generation);
    void Disconnect();
};
