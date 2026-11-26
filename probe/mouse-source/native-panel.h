#pragma once
#include <algorithm>
#include <charconv>
#include <iterator>
#include <memory>
#include <string_view>
#include <winrt/Windows.Devices.Input.h>
#include "../../runtime/presenter/frame-trace.h"

// All callbacks use the presenter's primary UI dispatcher. MouseMoved only
// modifies interlocked counters so it never needs XAML objects or file I/O.
struct NativePanelMouseProbe : std::enable_shared_from_this<NativePanelMouseProbe> {
    using Window = winrt::Windows::UI::Core::CoreWindow;
    using Mouse = winrt::Windows::Devices::Input::MouseDevice;
    using Kind = winrt::Windows::Devices::Input::PointerDeviceType;
    Window window{nullptr};
    Mouse mouse{nullptr};
    WCHAR reportPath[1024]{};
    winrt::event_token relative{}, core[5]{};
    unsigned registered = 0, events[5]{}, mouseEvents[5]{};
    HRESULT subscriptions[6]{}, captureHr = E_PENDING, releaseHr = E_PENDING;
    unsigned presses = 0, attempts = 0, losses = 0;
    uint32_t capturedPointer = 0;
    bool captureAccepted = false;
    volatile LONG foreground = 0, relativeEvents = 0, nonzeroDeltas = 0;
    ULONGLONG startTick = 0, nextWrite = 0;

    template<typename Add> void subscribe(unsigned index, Add add) noexcept {
        try {
            add();
            registered |= 1u << index;
        } catch (...) { subscriptions[index] = winrt::to_hresult(); }
    }
    void open(Window const& owner) noexcept {
        WCHAR path[1024];
        UINT n = ARRAYSIZE(path);
        if (GetCurrentPackagePath(&n, path) != ERROR_SUCCESS ||
            !FhdFrameTrace::suffix(path, ARRAYSIZE(path), L"\\fhd-mouse-source.txt")) return;
        HANDLE marker = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (marker == INVALID_HANDLE_VALUE) return;
        CloseHandle(marker);
        if (FAILED(FhdLocalPath(reportPath, ARRAYSIZE(reportPath))) ||
            !FhdFrameTrace::suffix(reportPath, ARRAYSIZE(reportPath), L"\\fhd-mouse-source.txt")) return;
        window = owner;
        startTick = GetTickCount64();
        subscribe(0, [&] {
            mouse = Mouse::GetForCurrentView();
            if (!mouse) winrt::throw_hresult(E_NOINTERFACE);
            relative = mouse.MouseMoved([weak = weak_from_this()](auto const&, auto const& args) noexcept {
                const auto self = weak.lock();
                if (!self || !InterlockedCompareExchange(&self->foreground, 0, 0)) return;
                InterlockedIncrement(&self->relativeEvents);
                try {
                    const auto delta = args.MouseDelta();
                    if (delta.X || delta.Y) InterlockedIncrement(&self->nonzeroDeltas);
                } catch (...) {}
            });
        });
        auto count = [weak = weak_from_this()](unsigned i) {
            return [weak,i](auto const&, auto const& args) noexcept {
                const auto self = weak.lock();
                if (!self) return;
                const bool active = InterlockedCompareExchange(&self->foreground, 0, 0) != 0;
                if (active) ++self->events[i];
                try {
                    const auto point = args.CurrentPoint();
                    if (point.PointerDevice().PointerDeviceType() == Kind::Mouse) {
                        if (active) ++self->mouseEvents[i];
                        if (point.PointerId() == self->capturedPointer && self->captureAccepted) {
                            if (i == 2 && !point.IsInContact()) self->release();
                            if (i == 4) { self->captureAccepted = false; ++self->losses; }
                        }
                    }
                } catch (...) {}
            };
        };
        subscribe(1, [&] { core[0] = window.PointerMoved(count(0)); });
        subscribe(2, [&] { core[1] = window.PointerPressed(count(1)); });
        subscribe(3, [&] { core[2] = window.PointerReleased(count(2)); });
        subscribe(4, [&] { core[3] = window.PointerWheelChanged(count(3)); });
        subscribe(5, [&] { core[4] = window.PointerCaptureLost(count(4)); });
    }
    void press(winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs const& args) noexcept {
        if (!window || !InterlockedCompareExchange(&foreground, 0, 0)) return;
        try {
            const auto pointer = args.Pointer();
            if (pointer.PointerDeviceType() != Kind::Mouse) return;
            ++presses;
            if (window.ActivationMode() != winrt::Windows::UI::Core::CoreWindowActivationMode::ActivatedInForeground ||
                !pointer.IsInContact() || captureAccepted) return;
            capturedPointer = pointer.PointerId();
            ++attempts;
            window.SetPointerCapture();
            captureHr = S_OK;
            captureAccepted = true;
        } catch (...) { captureHr = winrt::to_hresult(); }
    }
    void release() noexcept {
        if (!captureAccepted) return;
        captureAccepted = false;
        try {
            window.ReleasePointerCapture();
            releaseHr = S_OK;
        } catch (...) { releaseHr = winrt::to_hresult(); captureAccepted = true; }
    }
    void release(winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs const& args) noexcept {
        if (!captureAccepted) return;
        try {
            const auto pointer = args.Pointer();
            if (pointer.PointerId() == capturedPointer && !pointer.IsInContact()) release();
        } catch (...) {}
    }
    void report(bool visible, bool closed = false) noexcept {
        if (!window) return;
        InterlockedExchange(&foreground, visible);
        if (!visible) release();
        const auto now = GetTickCount64();
        if (!closed && now < nextWrite) return;
        nextWrite = now + 1000;
        char record[768];
        auto at = record;
        auto put = [&](std::string_view text) { at = std::copy(text.begin(), text.end(), at); };
        auto number = [&](uint64_t value, int base = 10) { at = std::to_chars(at, std::end(record), value, base).ptr; };
        auto field = [&](std::string_view name, uint64_t value, int base = 10) { put(name); number(value, base); };
        auto row = [&](std::string_view name, auto const& values, int base) {
            put(name);
            const char* separator = "";
            for (auto value : values) { put(separator); number(static_cast<uint32_t>(value), base); separator = ","; }
            put("\r\n");
        };
        field("schema=1 closed=", closed); field(" visible=", visible); field(" start_tick=", startTick);
        field(" tick=", now); field(" registered=0x", registered, 16); put("\r\n");
        row("registration_hr_relative_move_down_up_wheel_lost=", subscriptions, 16);
        field("relative=", InterlockedCompareExchange(&relativeEvents, 0, 0));
        field(" nonzero_delta=", InterlockedCompareExchange(&nonzeroDeltas, 0, 0)); put("\r\n");
        row("core_move_down_up_wheel_lost=", events, 10);
        row("mouse_move_down_up_wheel_lost=", mouseEvents, 10);
        field("panel_mouse_down=", presses); field(" capture_attempts=", attempts);
        field(" capture_hr=", static_cast<uint32_t>(captureHr), 16); field(" capture_accepted=", captureAccepted);
        field(" capture_losses=", losses); field(" release_hr=", static_cast<uint32_t>(releaseHr), 16); put("\r\n");
        HANDLE file = CreateFile2(reportPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  CREATE_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written;
        WriteFile(file, record, static_cast<DWORD>(at - record), &written, nullptr);
        CloseHandle(file);
    }
    void close() noexcept {
        if (!window) return;
        report(false, true);
        if (registered & 1) mouse.MouseMoved(relative);
        if (registered & 2) window.PointerMoved(core[0]);
        if (registered & 4) window.PointerPressed(core[1]);
        if (registered & 8) window.PointerReleased(core[2]);
        if (registered & 16) window.PointerWheelChanged(core[3]);
        if (registered & 32) window.PointerCaptureLost(core[4]);
        window = nullptr;
    }
};
