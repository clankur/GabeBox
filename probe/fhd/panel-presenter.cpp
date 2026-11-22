#include <windows.h>
#include <winstring.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

extern "C" HRESULT FhdClaimMedia(HSTRING, LONG*);
extern "C" HRESULT FhdDrawMedia(LONG, ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*, bool, UINT64&, bool&);
extern "C" void FhdMediaSequences(LONG, UINT64&, UINT64&);
extern "C" void FhdReleaseMedia(LONG);

namespace {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using Windows::UI::Xaml::Media::CompositionTarget;
constexpr UINT width = 1920, height = 1080;

struct Status {
    LONG token = 0;
    bool visible = false;
    UINT64 gpu = 0, cpu = 0, sequence = 0;
    HRESULT error = S_OK;
};
Status published;
std::mutex statusLock;
void publish(Status value) {
    std::lock_guard guard(statusLock);
    published = value;
}

struct Presenter {
    Status status;
    CoreWindow window{nullptr};
    Grid root{nullptr};
    Canvas steam{nullptr};
    SwapChainPanel panel{nullptr};
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain2> chain;
    com_ptr<ID3D11RenderTargetView> target;
    bool nativePad = false, attached = false, inserted = false;
    bool watchingFrames = false, watchingVisibility = false, watchingActivation = false;
    event_token frames{}, visibility{}, activation{};
    bool queuedGpu = false;
    UINT64 queuedSequence = 0;
    float fittedWidth = 0, fittedHeight = 0;

    bool visible() const {
        return SUCCEEDED(status.error) && window.Visible() &&
            window.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground &&
            steam.Visibility() == Visibility::Collapsed && panel.Visibility() == Visibility::Visible &&
            panel.ActualWidth() > 0 && panel.ActualHeight() > 0;
    }
    void updateVisibility() {
        status.visible = visible();
        if (!status.visible) queuedSequence = 0;
        publish(status);
    }
    void fit() {
        const float scale = static_cast<float>((std::min)(root.ActualWidth() / width, root.ActualHeight() / height));
        const float w = width * scale, h = height * scale;
        if (w <= 0 || h <= 0 || (fittedWidth == w && fittedHeight == h)) return;
        panel.Width(w);
        panel.Height(h);
        // The back buffer is in FHD pixels. Map it into the actual panel DIPs.
        DXGI_MATRIX_3X2_F matrix{scale, 0, 0, scale, 0, 0};
        check_hresult(chain->SetMatrixTransform(&matrix));
        fittedWidth = w;
        fittedHeight = h;
    }
    void render() noexcept {
        if (FAILED(status.error)) return;
        try {
            fit();
            status.visible = visible();
            if (!status.visible) {
                queuedSequence = 0;
            } else {
                if (!queuedSequence) {
                    UINT64 seq = 0;
                    bool gpu = false;
                    HRESULT hr = FhdDrawMedia(status.token, device.get(), context.get(), target.get(), nativePad, seq, gpu);
                    check_hresult(hr);
                    if (hr == S_OK) { queuedSequence = seq; queuedGpu = gpu; }
                }
                if (queuedSequence) {
                    // XAML supplies the render cadence. Never hold its UI thread
                    // waiting for DXGI to free a buffer; retry this exact frame.
                    HRESULT hr = chain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
                    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                        check_hresult(hr);
                        if (hr == S_OK) {
                            ++(queuedGpu ? status.gpu : status.cpu);
                            status.sequence = queuedSequence;
                        }
                        queuedSequence = 0;
                    }
                }
            }
        } catch (...) {
            status.error = to_hresult();
            status.visible = false;
        }
        publish(status);
    }
    void create(HSTRING path, bool pad) {
        nativePad = pad;
        auto xaml = Window::Current();
        if (!xaml || !(root = xaml.Content().try_as<Grid>()) || root.Children().Size() != 2)
            throw hresult_error(E_UNEXPECTED);
        auto account = root.Children().GetAt(0).try_as<WebView>();
        steam = root.Children().GetAt(1).try_as<Canvas>();
        if (!account || account.Name() != L"SteamAccountHost" || !steam || steam.Name() != L"SteamCompositionHost")
            throw hresult_error(E_ACCESSDENIED);
        auto signedPage = account.Tag().try_as<Uri>();
        if (!signedPage || signedPage.RawUri() != Uri{L"ms-appx-web:///steam-signin.html"}.RawUri())
            throw hresult_error(E_ACCESSDENIED);
        window = xaml.CoreWindow();
        check_hresult(FhdClaimMedia(path, &status.token));
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        com_ptr<IDXGISwapChain1> created;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, created.put()));
        chain = created.as<IDXGISwapChain2>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(chain->GetBuffer(0, IID_PPV_ARGS(texture.put())));
        check_hresult(device->CreateRenderTargetView(texture.get(), nullptr, target.put()));
        panel = SwapChainPanel{};
        panel.Name(L"SteamGameSurface");
        panel.IsHitTestVisible(false);
        panel.HorizontalAlignment(HorizontalAlignment::Center);
        panel.VerticalAlignment(VerticalAlignment::Center);
        fit();
        root.Children().InsertAt(1, panel);
        inserted = true;
        check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
        attached = true;
        visibility = window.VisibilityChanged([this](auto const&, auto const&) { updateVisibility(); });
        watchingVisibility = true;
        activation = window.Activated([this](auto const&, auto const&) { updateVisibility(); });
        watchingActivation = true;
        frames = CompositionTarget::Rendering([this](auto const&, auto const&) { render(); });
        watchingFrames = true;
        updateVisibility();
    }
    void close() noexcept {
        if (watchingFrames) CompositionTarget::Rendering(frames);
        if (watchingVisibility) window.VisibilityChanged(visibility);
        if (watchingActivation) window.Activated(activation);
        if (attached) {
            if (auto native = panel.try_as<ISwapChainPanelNative>()) native->SetSwapChain(nullptr);
        }
        if (inserted) {
            try {
                uint32_t i;
                if (root.Children().IndexOf(panel, i)) root.Children().RemoveAt(i);
            } catch (...) {}
        }
        if (status.token) FhdReleaseMedia(status.token);
        status.token = 0;
        status.visible = false;
        publish(status);
    }
};

// All XAML and D3D state is owned by the primary UI dispatcher. Only a numeric
// snapshot crosses back to calls originating on the trusted script thread.
std::unique_ptr<Presenter> active;
auto dispatcher() {
    return Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
}
IAsyncAction openPanel(hstring path, bool gamepad) {
    co_await resume_foreground(dispatcher());
    if (active) throw hresult_error(E_PENDING);
    auto next = std::make_unique<Presenter>();
    try {
        next->create(static_cast<HSTRING>(get_abi(path)), gamepad);
        active = std::move(next);
    } catch (...) {
        next->status.error = to_hresult();
        next->close();
        throw;
    }
}
IAsyncAction setVisible(bool show) {
    co_await resume_foreground(dispatcher());
    if (!active) throw hresult_error(RO_E_CLOSED);
    check_hresult(active->status.error);
    active->panel.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    active->updateVisibility();
}
IAsyncAction closePanel() {
    co_await resume_foreground(dispatcher());
    if (active) { active->close(); active.reset(); }
}
}

extern "C" HRESULT FhdOpenPanel(HSTRING path, BYTE pad, void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        UINT32 length;
        auto raw = WindowsGetStringRawBuffer(path, &length);
        *out = winrt::detach_abi(openPanel(winrt::hstring{raw, length}, pad != 0));
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT FhdSetPanelVisible(BYTE show, void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try { *out = winrt::detach_abi(setVisible(show != 0)); return S_OK; }
    catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT FhdClosePanel(void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try { *out = winrt::detach_abi(closePanel()); return S_OK; }
    catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT FhdPanelStatus(HSTRING* out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
        Status s;
        { std::lock_guard guard(statusLock); s = published; }
        UINT64 cpuSeq = 0, gpuSeq = 0;
        if (s.token) FhdMediaSequences(s.token, cpuSeq, gpuSeq);
        std::wstring text = L"primary_panel=1\r\n";
        auto field = [&](const wchar_t* name, UINT64 value) {
            text += name; text += L"="; text += std::to_wstring(value); text += L"\r\n";
        };
        field(L"running", s.token != 0); field(L"visible", s.visible);
        field(L"width", width); field(L"height", height);
        field(L"presented", s.gpu + s.cpu); field(L"gpu_presented", s.gpu); field(L"cpu_presented", s.cpu);
        field(L"source_sequence", s.sequence); field(L"gpu_sequence", gpuSeq); field(L"cpu_sequence", cpuSeq);
        if (FAILED(s.error)) field(L"error", static_cast<uint32_t>(s.error));
        return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), out);
    } catch (...) { return winrt::to_hresult(); }
}
