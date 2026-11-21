// A live video source for the primary WWA view. The existing receiver converts
// both the shared GPU texture and CPU SDL mailbox into the same BGRA target.
#include "gpu-receiver.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

extern "C" HRESULT FhdClaimMedia(HSTRING, LONG*);
extern "C" void FhdReleaseMedia(LONG);
extern "C" HRESULT FhdDrawMedia(LONG, ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*, bool, UINT64&, bool&);

namespace {
using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::MediaProperties;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr unsigned width = 1920, height = 1080;

struct Slot {
    com_ptr<ID3D11RenderTargetView> target;
    IDirect3DSurface surface{nullptr};
    bool busy = false;
};

struct Stream : std::enable_shared_from_this<Stream> {
    LONG token = 0;
    bool nativePad = false;
    std::mutex lock;
    bool closed = false;
    HRESULT error = S_OK;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    std::array<Slot, 3> slots;
    MediaStreamSource source{nullptr};
    uint64_t gpuCount = 0, cpuCount = 0, rendered = 0, startCount = 0, measured = 0;
    int64_t sampleUs = 0;
    Clock::time_point firstSample{}, firstRender{}, measurement{};

    ~Stream() { FhdReleaseMedia(token); }

    void initialize(HSTRING path, bool pad) {
        nativePad = pad;
        check_hresult(FhdClaimMedia(path, &token));
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        auto multithread = context.try_as<ID3D11Multithread>();
        if (multithread) multithread->SetMultithreadProtected(TRUE);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& slot : slots) {
            com_ptr<ID3D11Texture2D> texture;
            check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
            check_hresult(device->CreateRenderTargetView(texture.get(), nullptr, slot.target.put()));
            com_ptr<::IInspectable> wrapped;
            check_hresult(CreateDirect3D11SurfaceFromDXGISurface(texture.as<IDXGISurface>().get(), wrapped.put()));
            slot.surface = wrapped.as<IDirect3DSurface>();
        }
        auto props = VideoEncodingProperties::CreateUncompressed(MediaEncodingSubtypes::Bgra8(), width, height);
        props.FrameRate().Numerator(60); props.FrameRate().Denominator(1);
        props.PixelAspectRatio().Numerator(1); props.PixelAspectRatio().Denominator(1);
        source = MediaStreamSource(VideoStreamDescriptor(props));
        source.IsLive(true); source.CanSeek(false); source.BufferTime(0ms);
        source.Starting([](auto const&, auto const& args) { args.Request().SetActualStartPosition(TimeSpan{}); });
        auto weak = weak_from_this();
        source.SampleRequested([weak](auto const&, auto const& args) {
            if (auto self = weak.lock()) {
                auto request = args.Request();
                provide(std::move(self), request, request.GetDeferral());
            }
        });
        source.SampleRendered([weak](auto const&, auto const&) {
            if (auto self = weak.lock()) self->onRendered();
        });
        source.Closed([weak](auto const&, auto const&) {
            if (auto self = weak.lock()) self->close();
        });
    }

    void onRendered() {
        std::lock_guard guard(lock);
        if (closed || rendered >= cpuCount + gpuCount) return;
        ++rendered;
        auto now = Clock::now();
        if (rendered == 1) firstRender = now;
        if (!startCount && now - firstRender >= 5s) { startCount = rendered; measurement = now; }
        if (startCount && !sampleUs && now - measurement >= 30s) {
            measured = rendered - startCount;
            sampleUs = std::chrono::duration_cast<std::chrono::microseconds>(now - measurement).count();
        }
    }

    void close() {
        LONG release = 0;
        {
            std::lock_guard guard(lock);
            closed = true;
            std::swap(release, token);
        }
        FhdReleaseMedia(release);
    }

    // Each SDL mailbox acquisition represents a distinct producer frame. MF
    // may drop one; only SampleRendered contributes to the presentation count.
    MediaStreamSample next() {
        std::lock_guard guard(lock);
        if (closed) throw hresult_error(RO_E_CLOSED);
        for (size_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            if (slot.busy) continue;
            bool gpu = false;
            UINT64 sequence = 0;
            auto hr = FhdDrawMedia(token, device.get(), context.get(), slot.target.get(), nativePad, sequence, gpu);
            if (hr == S_FALSE) return nullptr;
            check_hresult(hr);
            context->Flush();
            auto now = Clock::now();
            if (!cpuCount && !gpuCount) firstSample = now;
            auto sample = MediaStreamSample::CreateFromDirect3D11Surface(slot.surface,
                std::chrono::duration_cast<TimeSpan>(now - firstSample));
            sample.Duration(TimeSpan{10000000 / 60});
            sample.KeyFrame(true);
            sample.Processed([weak = weak_from_this(),index](auto const&, auto const&) {
                if (auto self = weak.lock()) {
                    std::lock_guard guard(self->lock);
                    self->slots[index].busy = false;
                }
            });
            slot.busy = true;
            if (gpu) ++gpuCount; else ++cpuCount;
            return sample;
        }
        return nullptr;
    }

    static fire_and_forget provide(std::shared_ptr<Stream> self, MediaStreamSourceSampleRequest request,
                                   MediaStreamSourceSampleRequestDeferral deferral) {
        try {
            co_await resume_background();
            for (;;) {
                if (auto frame = self->next()) { request.Sample(frame); break; }
                co_await resume_after(4ms);
            }
        } catch (...) {
            auto hr = to_hresult();
            if (hr != RO_E_CLOSED) {
                std::lock_guard guard(self->lock);
                if (!self->closed) self->error = hr;
            }
        }
        try { deferral.Complete(); } catch (...) {}
    }

    hstring status() {
        std::lock_guard guard(lock);
        std::wstring text;
        auto field = [&](wchar_t const* key, uint64_t value) {
            text += key; text += L"="; text += std::to_wstring(value); text += L"\r\n";
        };
        field(L"running", !closed); field(L"width", width); field(L"height", height);
        field(L"submitted", gpuCount + cpuCount); field(L"presented", rendered);
        field(L"gpu_submitted", gpuCount); field(L"cpu_submitted", cpuCount);
        if (!cpuCount || !gpuCount) {
            field(L"gpu_presented", gpuCount ? rendered : 0);
            field(L"cpu_presented", cpuCount ? rendered : 0);
        }
        field(L"sample_us", sampleUs); field(L"sample_frames", measured);
        if (FAILED(error)) field(L"error", static_cast<uint32_t>(error));
        return hstring(text);
    }
};
std::mutex state;
std::shared_ptr<Stream> current;
}

extern "C" HRESULT FhdOpenMedia(HSTRING path, BYTE nativePad, void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        std::lock_guard guard(state);
        if (current) return E_PENDING;
        auto s = std::make_shared<Stream>();
        s->initialize(path, nativePad != 0);
        auto source = s->source;
        *out = winrt::detach_abi(source);
        current = std::move(s);
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT FhdMediaStatus(HSTRING* out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        std::lock_guard guard(state);
        auto text = current ? current->status() : winrt::hstring(L"running=0\r\n");
        *out = static_cast<HSTRING>(winrt::detach_abi(text));
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT FhdCloseMedia() noexcept {
    try {
        std::lock_guard guard(state);
        if (current) current->close();
        current.reset();
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
