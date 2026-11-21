// A live video source for the primary WWA view. The existing receiver converts
// both the shared GPU texture and CPU SDL mailbox into the same BGRA target.
#include "gpu-receiver.h"
#include "media-frame-clock.h"
#include "media-frame-count.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

extern "C" HRESULT FhdClaimMedia(HSTRING, LONG*);
extern "C" void FhdReleaseMedia(LONG);
extern "C" void FhdMediaSequences(LONG, UINT64&, UINT64&);
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

// The media sink may retain more surfaces than the reusable pool contains.
// D3D private data lives exactly as long as its texture (including sink-owned
// references). This bounds extra allocations even if Processed is delayed.
constexpr GUID textureLeaseId{0x66993deb,0xd087,0x44a9,{0xb5,0x5d,0x1d,0xe4,0x9f,0x36,0x34,0x16}};
class TextureLease final : public ::IUnknown {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<std::atomic<unsigned>> count;
public:
    explicit TextureLease(std::shared_ptr<std::atomic<unsigned>> value) : count(std::move(value)) { ++*count; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<::IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto left = --refs;
        if (!left) { --*count; delete this; }
        return left;
    }
};

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
    Slot spare;
    std::shared_ptr<std::atomic<unsigned>> outstanding = std::make_shared<std::atomic<unsigned>>(0);
    uint64_t requests = 0, pending = 0, processed = 0, poolWaits = 0, sourceWaits = 0;
    uint64_t extraAllocations = 0, peak = 0, lastSequence = 0;
    MediaStreamSource source{nullptr};
    MediaFrameCount frames;
    MediaFrameClock clock;
    winrt::handle stopEvent;

    ~Stream() { FhdReleaseMedia(token); }

    void initialize(HSTRING path, bool pad) {
        nativePad = pad;
        check_hresult(FhdClaimMedia(path, &token));
        stopEvent = winrt::handle{CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET,
            EVENT_MODIFY_STATE | SYNCHRONIZE)};
        if (!stopEvent) throw_last_error();
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        auto multithread = context.try_as<ID3D11Multithread>();
        if (multithread) multithread->SetMultithreadProtected(TRUE);
        for (auto& slot : slots) allocate(slot, false);
        auto props = VideoEncodingProperties::CreateUncompressed(MediaEncodingSubtypes::Bgra8(), width, height);
        props.FrameRate().Numerator(MediaFrameClock::rate); props.FrameRate().Denominator(1);
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

    void allocate(Slot& slot, bool ownedBySample) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
        if (ownedBySample) {
            com_ptr<::IUnknown> lease;
            lease.attach(new TextureLease(outstanding));
            check_hresult(texture->SetPrivateDataInterface(textureLeaseId, lease.get()));
            ++extraAllocations;
            peak = (std::max)(peak, static_cast<uint64_t>(outstanding->load()));
        }
        check_hresult(device->CreateRenderTargetView(texture.get(), nullptr, slot.target.put()));
        com_ptr<::IInspectable> wrapped;
        check_hresult(CreateDirect3D11SurfaceFromDXGISurface(texture.as<IDXGISurface>().get(), wrapped.put()));
        slot.surface = wrapped.as<IDirect3DSurface>();
    }

    void onRendered() {
        std::lock_guard guard(lock);
        if (!closed) frames.display(Clock::now());
    }

    void close() {
        LONG release = 0;
        {
            std::lock_guard guard(lock);
            closed = true;
            SetEvent(stopEvent.get());
            std::swap(release, token);
        }
        FhdReleaseMedia(release);
    }

    // Each SDL mailbox acquisition represents a distinct producer frame. MF
    // may drop one; only SampleRendered contributes to the presentation count.
    MediaStreamSample next(TimeSpan& delay) {
        std::lock_guard guard(lock);
        if (closed) throw hresult_error(RO_E_CLOSED);
        delay = clock.wait(std::chrono::duration_cast<TimeSpan>(Clock::now().time_since_epoch()));
        if (delay > TimeSpan::zero()) return nullptr;
        delay = 4ms;
        size_t index = 0;
        while (index < slots.size() && slots[index].busy) ++index;
        const bool extra = index == slots.size();
        auto& slot = extra ? spare : slots[index];
        if (extra && !spare.target) {
            if (outstanding->load() >= 12) { ++poolWaits; return nullptr; }
            allocate(spare, true);
        }
        bool gpu = false;
        UINT64 sequence = 0;
        auto hr = FhdDrawMedia(token, device.get(), context.get(), slot.target.get(), nativePad, sequence, gpu);
        if (hr == S_FALSE) { ++sourceWaits; return nullptr; }
        lastSequence = sequence;
        check_hresult(hr);
        context->Flush();
        const auto timing = clock.advance(std::chrono::duration_cast<TimeSpan>(Clock::now().time_since_epoch()));
        auto sample = MediaStreamSample::CreateFromDirect3D11Surface(slot.surface, timing.timestamp);
        sample.Duration(timing.duration);
        sample.KeyFrame(true);
        sample.Processed([weak = weak_from_this(),index](auto const&, auto const&) {
            if (auto self = weak.lock()) {
                std::lock_guard guard(self->lock);
                ++self->processed;
                if (index < self->slots.size()) self->slots[index].busy = false;
            }
        });
        if (extra) spare = {}; // The sample now owns its entire texture lifetime.
        else slot.busy = true;
        frames.submit(gpu);
        return sample;
    }

    static fire_and_forget provide(std::shared_ptr<Stream> self, MediaStreamSourceSampleRequest request,
                                   MediaStreamSourceSampleRequestDeferral deferral) {
        try {
            {
                std::lock_guard guard(self->lock);
                ++self->requests; ++self->pending;
            }
            co_await resume_background();
            for (;;) {
                TimeSpan delay;
                if (auto frame = self->next(delay)) { request.Sample(frame); break; }
                co_await resume_on_signal(self->stopEvent.get(), delay);
            }
        } catch (...) {
            auto hr = to_hresult();
            if (hr != RO_E_CLOSED) {
                std::lock_guard guard(self->lock);
                if (!self->closed) self->error = hr;
            }
        }
        { std::lock_guard guard(self->lock); --self->pending; }
        try { deferral.Complete(); } catch (...) {}
    }

    hstring status() {
        std::lock_guard guard(lock);
        std::wstring text;
        auto field = [&](wchar_t const* key, uint64_t value) {
            text += key; text += L"="; text += std::to_wstring(value); text += L"\r\n";
        };
        field(L"running", !closed); field(L"width", width); field(L"height", height);
        UINT64 cpu = 0, gpu = 0;
        FhdMediaSequences(token, cpu, gpu);
        size_t busy = 0;
        for (auto const& slot : slots) if (slot.busy) ++busy;
        field(L"requests", requests); field(L"pending", pending); field(L"processed", processed);
        field(L"slots_busy", busy); field(L"outstanding", outstanding->load());
        field(L"pool_waits", poolWaits); field(L"source_waits", sourceWaits);
        field(L"spill_allocations", extraAllocations); field(L"texture_peak", peak);
        field(L"source_sequence", lastSequence); field(L"cpu_sequence", cpu); field(L"gpu_sequence", gpu);
        field(L"submitted", frames.gpu + frames.cpu); field(L"presented", frames.rendered);
        field(L"gpu_submitted", frames.gpu); field(L"cpu_submitted", frames.cpu);
        if (frames.singleMode()) {
            field(L"gpu_presented", frames.gpu ? frames.rendered : 0);
            field(L"cpu_presented", frames.cpu ? frames.rendered : 0);
        }
        field(L"sample_us", frames.sampleUs); field(L"sample_frames", frames.sampleFrames);
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
