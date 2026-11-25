#pragma once
#include <atomic>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.System.Threading.h>

struct IndependentMouseProbe : std::enable_shared_from_this<IndependentMouseProbe> {
    using Panel = winrt::Windows::UI::Xaml::Controls::SwapChainPanel;
    using Source = winrt::Windows::UI::Core::CoreIndependentInputSource;
    using CoreArgs = winrt::Windows::UI::Core::PointerEventArgs;
    Panel panel{nullptr};
    winrt::com_ptr<IDXGISwapChain1> chain;
    HRESULT deviceHr = E_PENDING, attachHr = E_PENDING, presentHr = E_PENDING;
    std::atomic<HRESULT> sourceHr{E_PENDING}, registerHr{E_PENDING}, pumpHr{E_PENDING}, captureHr{E_PENDING};
    std::atomic<bool> stopped{false}, active{false}, hasCapture{false};
    std::atomic<unsigned> registered{0};
    std::atomic<unsigned> moves{0}, down{0}, up{0}, wheel{0}, lost{0}, captureSuccess{0}, pumpCount{0};
    bool tried = false, visibleOnce = false;

    void render() noexcept {
        HRESULT* step = &deviceHr;
        try {
            winrt::com_ptr<ID3D11Device> device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
            deviceHr = S_OK;
            step = &attachHr;
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 1920; desc.Height = 1080;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            winrt::check_hresult(factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, chain.put()));
            winrt::check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
            attachHr = S_OK;
            step = &presentHr;
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(chain->GetBuffer(0, IID_PPV_ARGS(texture.put())));
            winrt::com_ptr<ID3D11RenderTargetView> target;
            winrt::check_hresult(device->CreateRenderTargetView(texture.get(), nullptr, target.put()));
            const float violet[]{0.24f, 0.09f, 0.42f, 1.0f};
            context->ClearRenderTargetView(target.get(), violet);
            presentHr = chain->Present(1, 0);
        } catch (...) { *step = winrt::to_hresult(); }
    }
    void start() noexcept {
        if (tried || stopped || panel.ActualWidth() <= 0 || panel.ActualHeight() <= 0) return;
        tried = true;
        render();
        if (presentHr != S_OK) return;
        try {
            using namespace winrt::Windows::System::Threading;
            ThreadPool::RunAsync([self = shared_from_this(), surface = panel](auto const&) {
                self->listen(surface);
            }, WorkItemPriority::High, WorkItemOptions::TimeSliced);
        } catch (...) { sourceHr = winrt::to_hresult(); }
    }
    void listen(Panel const& surface) noexcept {
        using namespace winrt::Windows::UI::Core;
        Source input{nullptr};
        std::atomic<HRESULT>* step = &sourceHr;
        winrt::event_token tokens[5]{};
        unsigned attached = 0;
        try {
            if (stopped) return;
            input = surface.CreateCoreIndependentInputSource(CoreInputDeviceTypes::Mouse);
            if (!input) winrt::throw_hresult(E_NOINTERFACE);
            sourceHr = S_OK;
            step = &registerHr;
            auto mouse = [this](CoreArgs const& args) {
                return !stopped && args.CurrentPoint().PointerDevice().PointerDeviceType() == NativePanelMouseProbe::Kind::Mouse;
            };
            tokens[attached] = input.PointerMoved([this, mouse](auto const&, CoreArgs const& args) { if (mouse(args)) ++moves; });
            ++attached; registered = 1;
            // The input object owns these handlers and stays on its creating
            // thread. Use the callback sender to avoid a source/handler cycle.
            tokens[attached] = input.PointerPressed([this, mouse](auto const& sender, CoreArgs const& args) {
                if (!mouse(args)) return;
                ++down;
                if (!args.CurrentPoint().IsInContact() || hasCapture) return;
                try {
                    auto source = sender.template as<Source>();
                    source.SetPointerCapture();
                    hasCapture = source.HasCapture();
                    if (hasCapture) ++captureSuccess;
                    captureHr = S_OK;
                } catch (...) { captureHr = winrt::to_hresult(); }
            });
            ++attached; registered = 3;
            tokens[attached] = input.PointerReleased([this, mouse](auto const& sender, CoreArgs const& args) {
                if (!mouse(args)) return;
                ++up;
                if (hasCapture && !args.CurrentPoint().IsInContact()) {
                    auto source = sender.template as<Source>();
                    source.ReleasePointerCapture();
                    hasCapture = source.HasCapture();
                }
            });
            ++attached; registered = 7;
            tokens[attached] = input.PointerWheelChanged([this, mouse](auto const&, CoreArgs const& args) { if (mouse(args)) ++wheel; });
            ++attached; registered = 15;
            tokens[attached] = input.PointerCaptureLost([this](auto const&, auto const&) { ++lost; hasCapture = false; });
            ++attached;
            registerHr = S_OK;
            step = &pumpHr;
            auto dispatcher = input.Dispatcher();
            // Pump only available work so losing foreground can terminate the
            // loop even if no mouse events ever arrive.
            const auto end = GetTickCount64() + 180000;
            while (!stopped && GetTickCount64() < end) {
                dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                ++pumpCount;
                active = true;
                pumpHr = S_OK;
                Sleep(8);
            }
            stopped = true;
        } catch (...) { *step = winrt::to_hresult(); }
        if (input) {
            try { if (input.HasCapture()) input.ReleasePointerCapture(); } catch (...) {}
            if (attached >= 5) input.PointerCaptureLost(tokens[4]);
            if (attached >= 4) input.PointerWheelChanged(tokens[3]);
            if (attached >= 3) input.PointerReleased(tokens[2]);
            if (attached >= 2) input.PointerPressed(tokens[1]);
            if (attached >= 1) input.PointerMoved(tokens[0]);
        }
        hasCapture = false;
        active = false;
    }
    void close() noexcept {
        stopped = true;
        if (chain && panel) {
            if (auto native = panel.try_as<ISwapChainPanelNative>()) native->SetSwapChain(nullptr);
            chain = nullptr;
        }
    }
    void snapshot(bool visible, uint64_t id) {
        if (visible) { visibleOnce = true; start(); }
        else if (visibleOnce) close();
        struct Step { char const* name; HRESULT hr; };
        const Step steps[]{{"hardware-device",deviceHr}, {"swapchain-attach",attachHr},
            {"present",presentHr}, {"independent-source",sourceHr.load()},
            {"event-registration",registerHr.load()}, {"dispatcher-pump",pumpHr.load()}};
        char const* stage = "listening";
        const bool ended = stopped.load();
        char const* state = visible && active && !ended ? "independent-mouse-ready" : "independent-mouse-pending";
        HRESULT result = S_OK;
        for (auto const& step : steps) {
            if (step.hr == S_OK) continue;
            result = step.hr;
            stage = step.name;
            state = result == E_PENDING ? "independent-mouse-pending" : "independent-mouse-error";
            break;
        }
        if (ended && result == S_OK) { state = "independent-mouse-ended"; stage = "stopped"; }
        char text[1024];
        std::snprintf(text, sizeof(text),
            "run_id=%llu\r\nprobe=native-independent-mouse\r\nstatus=%s\r\nstage=%s\r\nhr=%08lx\r\n"
            "tick=%llu\r\nvisible=%u\r\nwidth=%.0f\r\nheight=%.0f\r\ndevice_hr=%08lx\r\nattach_hr=%08lx\r\n"
            "present_hr=%08lx\r\npresented=%u\r\nsource_hr=%08lx\r\nregistration_hr=%08lx\r\nregistered=0x%x\r\n"
            "pump_hr=%08lx\r\nactive=%u\r\nstopped=%u\r\npumps=%u\r\n"
            "move=%u\r\ndown=%u\r\nup=%u\r\nwheel=%u\r\ncapture_hr=%08lx\r\nhas_capture=%u\r\nconfirmed_captures=%u\r\nlost=%u\r\n",
            id, state, stage, (unsigned long)result, GetTickCount64(), unsigned(visible), panel.ActualWidth(),
            panel.ActualHeight(), (unsigned long)deviceHr, (unsigned long)attachHr, (unsigned long)presentHr,
            unsigned(presentHr == S_OK), (unsigned long)sourceHr.load(), (unsigned long)registerHr.load(), registered.load(),
            (unsigned long)pumpHr.load(), unsigned(active.load()), unsigned(ended), pumpCount.load(), moves.load(),
            down.load(), up.load(), wheel.load(), (unsigned long)captureHr.load(), unsigned(hasCapture.load()),
            captureSuccess.load(), lost.load());
        checkpoint("independent", text);
    }
};
