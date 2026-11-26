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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "frame-trace.h"
#include "gpu-receiver.h"
#include "hosted-input.h"
#include "../../probe/mouse-source/native-panel.h"

extern "C" HRESULT FhdClaimMedia(HSTRING, LONG*);
extern "C" HRESULT FhdDrawMedia(LONG, ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*, bool, UINT64&, bool&);
extern "C" void FhdMediaGpuCounters(LONG, HostedGpuCounters&);
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
    UINT64 chordsIgnored = 0, chordsExpired = 0;
    bool nativeInput = false;
    // Whether hosted input reaches the game now: unpaused, and the window focused, visible
    // and accepting input. Key events count CoreWindow key delivery for remote and keyboard input.
    bool inputActive = false, inputPaused = false;
    UINT keyEvents = 0;
    UINT64 focusRestores = 0;
    // Win32 keyboard focus (WindowFocus::state): 1 the view's window, 0 another window,
    // 2 no window, -1 unknown.
    int windowFocus = -1;
    UINT64 windowFocusTaken = 0;
    // With the native controller profile: the first sample published after the latest resume
    // (0 until published) and the game's acknowledged serial.
    LONG padResumeSerial = 0, padAck = 0;
    // What the game panel's composition went through (clankur/GabeBox#89, a white flicker
    // over a game's menus). hidden counts shown-to-hidden changes, split by what hid it:
    // the Steam shell shown over the game, the panel collapsed or without size, or the view
    // hidden or not active. uncovered counts hidden-to-shown changes, including the first
    // show. reordered counts changes in the panel's place among the page's children.
    // repeatPresents counts presents whose producer frame had already been presented.
    UINT64 hidden = 0, hiddenShell = 0, hiddenPanel = 0, hiddenWindow = 0;
    UINT64 uncovered = 0, reordered = 0, repeatPresents = 0;
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

// Win32 keyboard focus of the view's CoreWindow. While Big Picture covers the game, its
// WebView2 takes keyboard focus (MoveFocus gives the browser's input window HWND focus).
// XAML does not track that window, so focusing a XAML element afterwards can report success
// without returning Win32 focus. The CoreWindow then raises no key events, which carry remote
// controller input, the keyboard and the remote Menu + View chord: the game is shown but
// cannot be controlled. GetFocus and SetFocus are resolved at run time because the
// app-container link does not import them. Like WinUI's UWP WebView2 control, look in the
// rtcore API set first, then user32. Without them the state is unknown and nothing changes.
struct WindowFocus {
    using GetFocusFn = HWND(WINAPI*)();
    using SetFocusFn = HWND(WINAPI*)(HWND);
    // ICoreWindowInterop (corewindow.h), called through its ABI.
    struct Interop : ::IUnknown {
        virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* hwnd) = 0;
        virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(unsigned char value) = 0;
    };
    GetFocusFn get = nullptr;
    SetFocusFn set = nullptr;
    HWND host = nullptr;
    static void* resolve(const char* name) noexcept {
        for (auto module : {L"ext-ms-win-rtcore-webview-l1-1-0.dll", L"user32.dll"}) {
            if (HMODULE loaded = GetModuleHandleW(module))
                if (auto proc = GetProcAddress(loaded, name)) return reinterpret_cast<void*>(proc);
        }
        return nullptr;
    }
    void bind(CoreWindow const& window) noexcept {
        get = reinterpret_cast<GetFocusFn>(resolve("GetFocus"));
        set = reinterpret_cast<SetFocusFn>(resolve("SetFocus"));
        constexpr GUID iid{0x45d64a29, 0xa63e, 0x4cb6, {0xb4, 0x98, 0x57, 0x81, 0xd2, 0x98, 0xcb, 0x4f}};
        Interop* interop = nullptr;
        auto unknown = static_cast<::IUnknown*>(get_abi(window));
        if (unknown && SUCCEEDED(unknown->QueryInterface(iid, reinterpret_cast<void**>(&interop))) && interop) {
            HWND hwnd = nullptr;
            if (SUCCEEDED(interop->get_WindowHandle(&hwnd))) host = hwnd;
            interop->Release();
        }
    }
    enum { Unknown = -1, Other = 0, Held = 1, None = 2 };
    int state() const noexcept {
        if (!get || !host) return Unknown;
        const HWND current = get();
        return current == host ? Held : current ? Other : None;
    }
    // The state after giving focus back to the view's window.
    int take() noexcept {
        int now = state();
        if ((now == Other || now == None) && set) {
            set(host);
            now = state();
        }
        return now;
    }
};

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
    WindowFocus focus;
    DWORD focusTakenAt = 0;
    UINT focusRecords = 0;
    CoreWindow window{nullptr};
    Grid root{nullptr};
    Canvas steam{nullptr};
    WebView account{nullptr};
    SwapChainPanel panel{nullptr};
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain2> chain;
    com_ptr<ID3D11RenderTargetView> target;
    FhdFrameTrace trace;
    std::shared_ptr<NativePanelMouseProbe> mouseProbe = std::make_shared<NativePanelMouseProbe>();
    HANDLE inputTrace = INVALID_HANDLE_VALUE;
    // fhd-panel-stats.txt: the composition counters, rewritten in place at most once a second
    // when they change, so a long session leaves a bounded file with the latest totals.
    HANDLE statsTrace = INVALID_HANDLE_VALUE;
    DWORD statsWrittenAt = 0;
    UINT64 statsWritten[8]{};
    uint32_t panelIndex = UINT32_MAX, childCount = 0;
    bool menuHeld = false;
    FhdHostedChordCounts chords{};
    bool nativePad = false, attached = false, inserted = false;
    bool inputPaused = false;
    bool hostedPaused = false, watchingKeys = false, shown = false;
    UINT keysAtShow = 0;
    SdlHostMailbox *inputMailbox = nullptr;
    bool watchingFrames = false, watchingVisibility = false, watchingActivation = false;
    event_token frames{}, visibility{}, activation{};
    bool queuedGpu = false;
    UINT64 queuedSequence = 0;
    float fittedWidth = 0, fittedHeight = 0;

    enum class Cover { None, Error, Window, Shell, Panel };
    // What keeps the game from being shown, or Cover::None while it is shown.
    Cover cover() const {
        if (FAILED(status.error)) return Cover::Error;
        if (!window.Visible() || window.ActivationMode() != CoreWindowActivationMode::ActivatedInForeground)
            return Cover::Window;
        if (steam.Visibility() != Visibility::Collapsed) return Cover::Shell;
        if (panel.Visibility() != Visibility::Visible || !(panel.ActualWidth() > 0) || !(panel.ActualHeight() > 0))
            return Cover::Panel;
        return Cover::None;
    }
    bool visible() const { return cover() == Cover::None; }
    // Counts a change in the panel's place among the page's children (the account host, this
    // panel and the Steam composition host), which would move the game under another layer.
    void noteOrder() noexcept {
        try {
            uint32_t index = UINT32_MAX;
            const auto children = root.Children();
            if (!children.IndexOf(panel, index)) index = UINT32_MAX;
            const uint32_t count = children.Size();
            if (index != panelIndex || count != childCount) {
                if (childCount) ++status.reordered;
                panelIndex = index;
                childCount = count;
            }
        } catch (...) {}
    }
    void updateVisibility() {
        status.visible = visible();
        if (!status.visible) queuedSequence = 0;
        noteShown();
        updateInputVisibility();
        snapshotInput();
        publish(status);
    }
    void snapshotInput() noexcept {
        if (!status.nativeInput) return;
        status.inputPaused = FhdHostedInputPaused() != FALSE;
        status.inputActive = FhdHostedInputForeground() != FALSE;
        status.keyEvents = FhdHostedKeyEvents();
        FhdReadHostedResumeSample(&status.padResumeSerial, &status.padAck);
        status.windowFocus = focus.state();
        // A focused window destroyed while the game is shown (a WebView closed after it took
        // focus) leaves no window with keyboard focus and the game without key input. Take it
        // back, at most once a second. Focus held by another window, such as a text field the
        // account host opened over the game, is left alone until the game is next shown.
        if (status.visible && status.windowFocus == WindowFocus::None && GetTickCount() - focusTakenAt >= 1000)
            takeWindowFocus("retaken");
        if (hostedPaused && !status.inputPaused) {
            constexpr char record[] = "panel_input=resumed\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        hostedPaused = status.inputPaused;
        if (watchingKeys && status.keyEvents != keysAtShow) {
            watchingKeys = false;
            constexpr char record[] = "panel_keys=after-show\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
    }
    // Gameplay keeps keyboard focus on the account host, where the CoreWindow still raises
    // the key events that carry remote controller input, the keyboard and the remote chord.
    // Big Picture moves focus into its WebView while it covers the game. If that WebView
    // closes or hangs while it holds focus, nothing returns it, so take it back each time
    // the game becomes visible (the Steam shell is then collapsed).
    void noteShown() noexcept {
        if (status.visible && !shown) {
            ++status.uncovered;
            restoreFocus();
        } else if (!status.visible && shown) {
            ++status.hidden;
            Cover why = Cover::Error;
            try { why = cover(); } catch (...) {}
            if (why == Cover::Shell) ++status.hiddenShell;
            else if (why == Cover::Panel) ++status.hiddenPanel;
            else if (why == Cover::Window) ++status.hiddenWindow;
        }
        shown = status.visible;
    }
    void writeStats() noexcept {
        if (statsTrace == INVALID_HANDLE_VALUE) return;
        const UINT64 values[8] = {status.hidden, status.hiddenShell, status.hiddenPanel, status.hiddenWindow,
            status.uncovered, status.reordered, status.repeatPresents, status.gpu + status.cpu};
        const DWORD now = GetTickCount();
        if (now - statsWrittenAt < 1000 || std::equal(values, values + 8, statsWritten)) return;
        static constexpr const char* names[8] = {"panel_hidden", "panel_hidden_shell", "panel_hidden_panel",
            "panel_hidden_window", "panel_uncovered", "panel_reordered", "panel_repeat_presents", "panel_presented"};
        std::string text;
        for (size_t i = 0; i < 8; ++i) text += std::string(names[i]) + "=" + std::to_string(values[i]) + "\r\n";
        LARGE_INTEGER start{};
        DWORD written = 0;
        if (!SetFilePointerEx(statsTrace, start, nullptr, FILE_BEGIN) ||
                !WriteFile(statsTrace, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
                written != text.size() || !SetEndOfFile(statsTrace)) {
            CloseHandle(statsTrace);
            statsTrace = INVALID_HANDLE_VALUE;
            return;
        }
        statsWrittenAt = now;
        std::copy(values, values + 8, statsWritten);
    }
    // Records panel_window_focus=<event> from=other|none when Win32 focus moved back to the
    // view's window, or =declined when it could not. A window that already holds focus, or an
    // unknown state, records nothing.
    void takeWindowFocus(const char* event) noexcept {
        focusTakenAt = GetTickCount();
        const int before = focus.state();
        if (before != WindowFocus::Other && before != WindowFocus::None) return;
        status.windowFocus = focus.take();
        if (status.windowFocus == WindowFocus::Held) ++status.windowFocusTaken;
        if (focusRecords >= 32) return;
        ++focusRecords;
        std::string record = std::string("panel_window_focus=") +
            (status.windowFocus == WindowFocus::Held ? event : "declined") +
            (before == WindowFocus::None ? " from=none\r\n" : " from=other\r\n");
        inputCheckpoint(record.data(), static_cast<DWORD>(record.size()));
    }
    void restoreFocus() noexcept {
        if (!status.nativeInput) return;
        // Win32 focus first: the XAML focus below routes keys only once the window has it.
        takeWindowFocus("taken");
        bool focused = false;
        try { focused = account && account.Focus(FocusState::Programmatic); } catch (...) {}
        if (focused) {
            ++status.focusRestores;
            constexpr char record[] = "panel_focus=restored\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        } else {
            constexpr char record[] = "panel_focus=declined\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        keysAtShow = FhdHostedKeyEvents();
        watchingKeys = true;
    }
    void updateInputVisibility() {
        mouseProbe->report(status.visible && (status.gpu || status.cpu));
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
        FhdHostedChordCounts counts{};
        FhdReadHostedChordCounts(&counts);
        // Record where each accepted chord came from, and each rejected or expired one,
        // so a menu request can be traced to a live controller or to key events.
        if (held && !menuHeld && status.menuChords++ < 32) {
            if (counts.keys > chords.keys) {
                constexpr char record[] = "menu_chord=held source=keys\r\n";
                inputCheckpoint(record, sizeof(record) - 1);
            } else {
                constexpr char record[] = "menu_chord=held source=pad\r\n";
                inputCheckpoint(record, sizeof(record) - 1);
            }
        }
        if (counts.ignored > chords.ignored && status.chordsIgnored++ < 32) {
            constexpr char record[] = "menu_chord=ignored reason=unpaired\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        if (counts.expired > chords.expired && status.chordsExpired++ < 32) {
            constexpr char record[] = "menu_chord=expired\r\n";
            inputCheckpoint(record, sizeof(record) - 1);
        }
        chords = counts;
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
            noteShown();
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
                            if (status.gpu + status.cpu && queuedSequence == status.sequence) ++status.repeatPresents;
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
            mouseProbe->report(false);
            if (status.nativeInput) FhdPauseHostedInput();
        }
        noteOrder();
        snapshotInput();
        writeStats();
        publish(status);
    }
    void create(HSTRING path, bool pad) {
        nativePad = pad;
        auto xaml = Window::Current();
        if (!xaml || !(root = xaml.Content().try_as<Grid>()) || root.Children().Size() != 2)
            throw hresult_error(E_UNEXPECTED);
        account = root.Children().GetAt(0).try_as<WebView>();
        steam = root.Children().GetAt(1).try_as<Canvas>();
        if (!account || account.Name() != L"SteamAccountHost" || !steam || steam.Name() != L"SteamCompositionHost")
            throw hresult_error(E_ACCESSDENIED);
        auto signedPage = account.Tag().try_as<Uri>();
        if (!signedPage || signedPage.RawUri() != Uri{L"ms-appx-web:///steam-signin.html"}.RawUri())
            throw hresult_error(E_ACCESSDENIED);
        window = xaml.CoreWindow();
        focus.bind(window);
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
            pointerTarget.PointerPressed([probe = mouseProbe](auto const& sender, auto const& event) {
                probe->press(event);
                relayPointer(sender, event, false);
            });
            pointerTarget.PointerReleased([probe = mouseProbe](auto const& sender, auto const& event) {
                probe->release(event);
                relayPointer(sender, event, false);
            });
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
        mouseProbe->open(window);
        WCHAR inputPath[1024];
        if (trace.enabled() && SUCCEEDED(FhdLocalPath(inputPath, ARRAYSIZE(inputPath))) &&
                FhdFrameTrace::suffix(inputPath, ARRAYSIZE(inputPath), L"\\fhd-panel-input.txt")) {
            inputTrace = CreateFile2(inputPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                CREATE_ALWAYS, nullptr);
        }
        WCHAR statsPath[1024];
        if (trace.enabled() && SUCCEEDED(FhdLocalPath(statsPath, ARRAYSIZE(statsPath))) &&
                FhdFrameTrace::suffix(statsPath, ARRAYSIZE(statsPath), L"\\fhd-panel-stats.txt")) {
            statsTrace = CreateFile2(statsPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
        mouseProbe->close();
        if (inputTrace != INVALID_HANDLE_VALUE) {
            CloseHandle(inputTrace);
            inputTrace = INVALID_HANDLE_VALUE;
        }
        if (statsTrace != INVALID_HANDLE_VALUE) {
            statsWrittenAt = GetTickCount() - 1000;
            writeStats();
            if (statsTrace != INVALID_HANDLE_VALUE) CloseHandle(statsTrace);
            statsTrace = INVALID_HANDLE_VALUE;
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
    active->snapshotInput();
    publish(active->status);
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
        HostedGpuCounters exchange{};
        if (s.token) {
            FhdMediaSequences(s.token, cpuSeq, gpuSeq);
            FhdMediaGpuCounters(s.token, exchange);
        }
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
        // Shared-texture exchange (gpu-receiver.h): frames left undrawn because the producer's
        // copy had not completed, frames held or superseded, and the last drawn fence values.
        field(L"gpu_wait_producer", exchange.notReady); field(L"gpu_held", exchange.held);
        field(L"gpu_released_unshown", exchange.released); field(L"gpu_drawn", exchange.drawn);
        field(L"gpu_ready_fence", exchange.readyValue); field(L"gpu_done_fence", exchange.doneValue);
        field(L"panel_hidden", s.hidden); field(L"panel_hidden_shell", s.hiddenShell);
        field(L"panel_hidden_panel", s.hiddenPanel); field(L"panel_hidden_window", s.hiddenWindow);
        field(L"panel_uncovered", s.uncovered); field(L"panel_reordered", s.reordered);
        field(L"repeat_presents", s.repeatPresents);
        field(L"native_input", s.nativeInput); field(L"native_menu_requests", s.menuRequests);
        field(L"native_menu_chords", s.menuChords);
        field(L"native_menu_chords_ignored", s.chordsIgnored);
        field(L"native_menu_chords_expired", s.chordsExpired);
        if (s.nativeInput) {
            field(L"native_input_active", s.inputActive); field(L"native_input_paused", s.inputPaused);
            field(L"native_key_events", s.keyEvents); field(L"native_focus_restores", s.focusRestores);
            if (s.padResumeSerial > 0) {
                field(L"native_pad_resume_serial", static_cast<UINT64>(s.padResumeSerial));
                field(L"native_pad_ack", static_cast<uint32_t>(s.padAck));
            }
            if (s.windowFocus >= 0) {
                field(L"native_window_focus", static_cast<UINT64>(s.windowFocus));
                field(L"native_window_focus_taken", s.windowFocusTaken);
            }
        }
        if (FAILED(s.inputError)) field(L"native_input_hresult", static_cast<uint32_t>(s.inputError));
        if (FAILED(s.pointerTargetError)) field(L"pointer_target_hresult", static_cast<uint32_t>(s.pointerTargetError));
        if (FAILED(s.error)) field(L"error", static_cast<uint32_t>(s.error));
        return WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), out);
    } catch (...) { return winrt::to_hresult(); }
}
