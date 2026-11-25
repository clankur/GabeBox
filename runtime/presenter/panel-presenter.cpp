#include <windows.h>
#include <winstring.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "frame-trace.h"
#include "hosted-input.h"

extern "C" HRESULT FhdClaimMedia(HSTRING, LONG*);
extern "C" HRESULT FhdDrawMedia(LONG, ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*, bool, UINT64&, bool&);
extern "C" void FhdMediaSequences(LONG, UINT64&, UINT64&);
extern "C" void FhdReleaseMedia(LONG);
extern "C" SdlHostMailbox *FhdMediaInputMailbox(LONG, SdlRumbleMailbox**);

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
    UINT64 renderCallbacks = 0, sourceEmpty = 0, presentBusy = 0;
    UINT64 menuRequests = 0;
    UINT64 menuChords = 0;
    bool nativeInput = false;
    HRESULT inputError = S_OK;
    HRESULT pointerTargetError = S_OK;
    HRESULT error = S_OK;
};
Status published;
std::mutex statusLock;
void publish(Status value) {
    std::lock_guard guard(statusLock);
    published = value;
}

void relayPointer(Windows::Foundation::IInspectable const& sender,
                  Windows::UI::Xaml::Input::PointerRoutedEventArgs const& event,
                  bool wheel) noexcept {
    try {
        const auto target = sender.as<Grid>();
        const auto width = target.ActualWidth(), height = target.ActualHeight();
        if (width <= 0 || height <= 0) return;
        const auto point = event.GetCurrentPoint(target);
        const auto position = point.Position();
        const auto props = point.Properties();
        const LONG buttons = (props.IsLeftButtonPressed() ? SDL_HOST_POINTER_LEFT : 0) |
            (props.IsRightButtonPressed() ? SDL_HOST_POINTER_RIGHT : 0) |
            (props.IsMiddleButtonPressed() ? SDL_HOST_POINTER_MIDDLE : 0) |
            (props.IsXButton1Pressed() ? SDL_HOST_POINTER_X1 : 0) |
            (props.IsXButton2Pressed() ? SDL_HOST_POINTER_X2 : 0);
        FhdHostedPointerUpdate(position.X / width, position.Y / height, buttons,
            wheel ? props.MouseWheelDelta() : 0, wheel && props.IsHorizontalMouseWheel());
    } catch (...) { /* The view may disappear while a pointer event is being dispatched. */ }
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
    FhdFrameTrace trace;
    HANDLE inputTrace = INVALID_HANDLE_VALUE;
    bool menuHeld = false;
    bool nativePad = false, attached = false, inserted = false;
    bool inputPaused = false;
    SdlHostMailbox *inputMailbox = nullptr;
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
        updateInputVisibility();
        publish(status);
    }
    void updateInputVisibility() {
        const bool pause = !status.visible;
        if (!status.nativeInput || inputPaused == pause) return;
        inputPaused = pause;
        if (inputPaused) FhdPauseHostedInput();
        else FhdResumeHostedInput();
    }
    void inputCheckpoint(const char *record, DWORD length) noexcept {
        if (inputTrace == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        if (!WriteFile(inputTrace, record, length, &written, nullptr) || written != length) {
            CloseHandle(inputTrace);
            inputTrace = INVALID_HANDLE_VALUE;
        }
    }
    void pollInput() {
        if (!status.nativeInput) return;
        const bool requested = FhdPollHostedInput(inputMailbox, get_abi(window));
        const bool held = FhdHostedMenuHeld();
        if (held && !menuHeld && status.menuChords++ < 32) {
            constexpr char record[] = "menu_chord=held\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        menuHeld = held;
        if (requested && status.menuRequests++ < 32) {
            constexpr char record[] = "menu_request=ready\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
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
        if (FAILED(status.error)) {
            if (status.nativeInput) FhdStopHostedRumble();
            return;
        }
        try {
            fit();
            status.visible = visible();
            updateInputVisibility();
            if (!status.visible) {
                queuedSequence = 0;
                if (status.nativeInput) FhdStopHostedRumble();
            } else {
                pollInput();
                ++status.renderCallbacks;
                if (!queuedSequence) {
                    UINT64 seq = 0;
                    bool gpu = false;
                    HRESULT hr = FhdDrawMedia(status.token, device.get(), context.get(), target.get(), nativePad, seq, gpu);
                    check_hresult(hr);
                    if (hr == S_OK) { queuedSequence = seq; queuedGpu = gpu; }
                    else if (hr == S_FALSE) ++status.sourceEmpty;
                }
                if (queuedSequence) {
                    // XAML supplies the render cadence. Never hold its UI thread
                    // waiting for DXGI to free a buffer; retry this exact frame.
                    HRESULT hr = chain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
                    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                        ++status.presentBusy;
                    } else {
                        check_hresult(hr);
                        if (hr == S_OK) {
                            ++(queuedGpu ? status.gpu : status.cpu);
                            status.sequence = queuedSequence;
                            if (trace.enabled()) {
                                LARGE_INTEGER timestamp{};
                                QueryPerformanceCounter(&timestamp);
                                trace.record(timestamp.QuadPart, queuedSequence,
                                    static_cast<UINT>(status.gpu + status.cpu), queuedGpu);
                            }
                        }
                        queuedSequence = 0;
                    }
                }
            }
        } catch (...) {
            status.error = to_hresult();
            status.visible = false;
            if (status.nativeInput) FhdPauseHostedInput();
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
        bool routedPointer = false;
        try {
            // SwapChainPanel rejects Background. Its child can receive the pointer.
            Grid pointerTarget;
            pointerTarget.Background(Windows::UI::Xaml::Media::SolidColorBrush(Windows::UI::Color{}));
            const auto update = [](auto const& sender, auto const& event) { relayPointer(sender, event, false); };
            pointerTarget.PointerMoved(update);
            pointerTarget.PointerPressed(update);
            pointerTarget.PointerReleased(update);
            pointerTarget.PointerWheelChanged([](auto const& sender, auto const& event) { relayPointer(sender, event, true); });
            pointerTarget.PointerExited([](auto const&, auto const&) { FhdHostedPointerRelease(TRUE); });
            pointerTarget.PointerCaptureLost([](auto const&, auto const&) { FhdHostedPointerRelease(FALSE); });
            panel.Children().Append(pointerTarget);
            routedPointer = true;
        } catch (...) {
            status.pointerTargetError = to_hresult();
        }
        panel.HorizontalAlignment(HorizontalAlignment::Center);
        panel.VerticalAlignment(VerticalAlignment::Center);
        fit();
        root.Children().InsertAt(1, panel);
        inserted = true;
        check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
        attached = true;
        trace.open();
        WCHAR inputPath[1024];
        if (trace.enabled() && SUCCEEDED(FhdLocalPath(inputPath, ARRAYSIZE(inputPath))) &&
                FhdFrameTrace::suffix(inputPath, ARRAYSIZE(inputPath), L"\\fhd-panel-input.txt")) {
            inputTrace = CreateFile2(inputPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                CREATE_ALWAYS, nullptr);
        }
        SdlRumbleMailbox *rumble = nullptr;
        inputMailbox = FhdMediaInputMailbox(status.token, &rumble);
        status.inputError = FhdBeginHostedInput(inputMailbox, rumble, get_abi(window));
        if (SUCCEEDED(status.inputError)) {
            if (routedPointer) FhdUseRoutedPointer();
            FhdSetHostedInputProfile(nativePad, width, height);
            FhdHostedMenuOnRelease(TRUE);
            status.nativeInput = true;
            constexpr char record[] = "panel_input=ready\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        visibility = window.VisibilityChanged([this](auto const&, auto const&) { updateVisibility(); });
        watchingVisibility = true;
        activation = window.Activated([this](auto const&, auto const&) { updateVisibility(); });
        watchingActivation = true;
        frames = CompositionTarget::Rendering([this](auto const&, auto const&) { render(); });
        watchingFrames = true;
        updateVisibility();
    }
    void close() noexcept {
        if (inputTrace != INVALID_HANDLE_VALUE) {
            CloseHandle(inputTrace);
            inputTrace = INVALID_HANDLE_VALUE;
        }
        trace.close();
        if (status.nativeInput) FhdEndHostedInput();
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
    if (show && active->status.nativeInput && active->status.visible) FhdResumeHostedInput();
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
        field(L"render_callbacks", s.renderCallbacks); field(L"source_empty_callbacks", s.sourceEmpty);
        field(L"present_busy_callbacks", s.presentBusy);
        field(L"native_input", s.nativeInput); field(L"native_menu_requests", s.menuRequests);
        field(L"native_menu_chords", s.menuChords);
        if (FAILED(s.inputError)) field(L"native_input_hresult", static_cast<uint32_t>(s.inputError));
        if (FAILED(s.pointerTargetError)) field(L"pointer_target_hresult", static_cast<uint32_t>(s.pointerTargetError));
        if (FAILED(s.error)) field(L"error", static_cast<uint32_t>(s.error));
        return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), out);
    } catch (...) { return winrt::to_hresult(); }
}
