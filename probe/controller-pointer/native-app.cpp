// Standalone controller enumeration for a mouse-as-controller adapter.
//
// A physical mouse never reaches a Dev Mode package (analysis/xbox-mouse-input.md),
// so a cursor needs a device the console does deliver. This app records which game
// controllers Windows.Gaming.Input exposes on the console and how their readings
// behave: identity, axis resolution, update interval, and resting values. It answers
// two questions with one build:
//   1. Does a generic USB HID joystick (for example an RP2040 running hid-remapper)
//      appear as a RawGameController or HID collection without Xbox authentication?
//   2. For an adapter that does appear, such as an Xbox Adaptive Controller fed by a
//      mouse-to-joystick board, what resolution, rate and deadzone reach the package?
#include <windows.h>
#include <appmodel.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt;
using namespace Windows::Gaming::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using Windows::Devices::Enumeration::DeviceInformation;
using Windows::Devices::Enumeration::DeviceInformationUpdate;
using Windows::Devices::Enumeration::DeviceWatcher;
using Windows::Devices::Enumeration::DeviceWatcherStatus;
using Windows::Devices::HumanInterfaceDevice::HidDevice;
using Windows::UI::Core::CoreWindow;
using Windows::UI::Core::CoreWindowActivationMode;
using Windows::UI::Xaml::Media::SolidColorBrush;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
// Reading arrays are sized from each controller's counts. Only the report caps
// what it prints.
constexpr unsigned reportedAxes = 8, maskedButtons = 32, maxControllers = 8;
// Foreground snapshots required before the sampling status. Controllers arrive
// through Added events after the view first becomes foreground.
constexpr unsigned settleSnapshots = 3;

void write(wchar_t const* leaf, std::string const& text) noexcept {
    try {
        auto path = std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path()) + leaf;
        file_handle file{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, nullptr)};
        DWORD written;
        if (file) WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    } catch (...) {}
}

std::string utf8(hstring const& text) {
    std::string out;
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    out.resize(size > 0 ? size : 0);
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    for (auto& c : out) if (c == '\r' || c == '\n' || c == '=') c = '_';
    return out;
}

template <typename... Args> std::string format(char const* pattern, Args... args) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), pattern, args...);
    return buffer;
}

uint64_t qpcMicroseconds() {
    static const LONGLONG frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart / frequency * 1000000 + now.QuadPart % frequency * 1000000 / frequency);
}

struct Axis {
    double first = NAN, min = NAN, max = NAN, last = NAN, step = 0;
    unsigned changes = 0;
    void sample(double value) {
        if (std::isnan(first)) { first = min = max = last = value; return; }
        if (value == last) return;
        const double delta = std::fabs(value - last);
        if (step == 0 || delta < step) step = delta;
        ++changes;
        min = (std::min)(min, value);
        max = (std::max)(max, value);
        last = value;
    }
};

struct Identity {
    std::string name, id;
    unsigned vid = 0, pid = 0, axisCount = 0, buttonCount = 0, switchCount = 0;
    bool wireless = false;
};

// One record per controller the console has exposed during this run. Records
// survive removal so a replug shows up as a new generation, not a reset.
struct Controller {
    RawGameController raw{nullptr};
    Gamepad pad{nullptr};
    Identity identity;
    bool connected = true;
    unsigned generation = 1;
    uint64_t polls = 0, distinct = 0, lastTimestamp = 0, intervalMin = 0, intervalSum = 0, intervals = 0;
    unsigned readFailures = 0;
    HRESULT lastReadHr = S_OK;
    std::vector<Axis> axes;
    uint32_t buttonsSeen = 0, buttonsLast = 0, switchesSeen = 0;
    unsigned buttonsAbove32 = 0, buttonTransitions = 0;
    // The typed gamepad reading carries the same device through the path the
    // presenter uses today, so its values are recorded beside the raw axes.
    Axis padAxes[6];
    uint32_t padButtonsSeen = 0;
    uint64_t padDistinct = 0, padTimestamp = 0;
};

// A reading taken outside the lock, folded into its record afterwards.
struct Reading {
    size_t index = 0;
    unsigned generation = 0;
    RawGameController raw{nullptr};
    Gamepad pad{nullptr};
    unsigned buttonCount = 0, switchCount = 0, axisCount = 0;
    std::unique_ptr<bool[]> buttons;
    std::vector<GameControllerSwitchPosition> switches;
    std::vector<double> axes;
    uint64_t timestamp = 0;
    HRESULT hr = S_OK;
    bool padValid = false;
    GamepadReading padReading{};
};

// A HID joystick or gamepad collection the console enumerates would be a second,
// authentication-free route for the same adapter. Enumeration shows presence;
// opened counts show whether the package can actually read the device.
struct HidRoute {
    DeviceWatcher watcher{nullptr};
    std::atomic<int> added{0}, removed{0}, opened{0}, openFailed{0};
    std::atomic<HRESULT> openHr{E_PENDING}, watcherHr{E_PENDING};
    bool settled() const {
        if (FAILED(watcherHr.load()) && watcherHr.load() != E_PENDING) return true;
        if (!watcher) return false;
        const auto status = watcher.Status();
        // An open still in flight would report found without opened or open_failed.
        return (status == DeviceWatcherStatus::EnumerationCompleted || status == DeviceWatcherStatus::Aborted ||
                status == DeviceWatcherStatus::Stopped) && opened.load() + openFailed.load() >= added.load();
    }
    int status() const { return watcher ? static_cast<int>(watcher.Status()) : -1; }
};

// The first HidDevice open from an app should run on the UI thread so the system
// can show a consent prompt, so the open is marshalled to the view's dispatcher.
fire_and_forget openHid(std::shared_ptr<HidRoute> route, hstring id, Windows::UI::Core::CoreDispatcher dispatcher) {
    try {
        co_await resume_foreground(dispatcher);
        auto device = co_await HidDevice::FromIdAsync(id, Windows::Storage::FileAccessMode::Read);
        if (device) { ++route->opened; route->openHr = S_OK; device.Close(); }
        else { ++route->openFailed; route->openHr = E_ACCESSDENIED; }
    } catch (...) { ++route->openFailed; route->openHr = to_hresult(); }
}

void watchHid(std::shared_ptr<HidRoute> const& route, uint16_t usage, Windows::UI::Core::CoreDispatcher const& dispatcher) {
    try {
        route->watcher = DeviceInformation::CreateWatcher(HidDevice::GetDeviceSelector(0x01, usage));
        // A watcher delivers Added only when Added, Removed and Updated all have handlers.
        route->watcher.Added([route, dispatcher](auto const&, DeviceInformation const& info) {
            ++route->added;
            openHid(route, info.Id(), dispatcher);
        });
        route->watcher.Removed([route](auto const&, DeviceInformationUpdate const&) { ++route->removed; });
        route->watcher.Updated([](auto const&, DeviceInformationUpdate const&) {});
        route->watcher.Start();
        route->watcherHr = S_OK;
    } catch (...) { route->watcherHr = to_hresult(); }
}

struct Probe {
    std::mutex lock;
    std::vector<Controller> controllers;
    std::atomic<unsigned> rawAdded{0}, rawRemoved{0}, padAdded{0}, padRemoved{0};
    std::atomic<bool> running{true};
    std::atomic<uint64_t> samplerLoops{0}, samplerStart{0}, samplerLast{0};
    std::atomic<HRESULT> timerHr{E_PENDING};
    std::shared_ptr<HidRoute> hidJoystick = std::make_shared<HidRoute>(), hidGamepad = std::make_shared<HidRoute>();
    std::thread sampler;

    void attach(RawGameController const& raw) {
        // Read the device's properties before taking the lock the sampler holds.
        Identity identity;
        Gamepad pad{nullptr};
        try {
            identity.id = utf8(raw.NonRoamableId());
            identity.name = utf8(raw.DisplayName());
            identity.vid = raw.HardwareVendorId();
            identity.pid = raw.HardwareProductId();
            identity.axisCount = raw.AxisCount();
            identity.buttonCount = raw.ButtonCount();
            identity.switchCount = raw.SwitchCount();
            identity.wireless = raw.IsWireless();
            pad = Gamepad::FromGameController(raw);
        } catch (...) { return; }
        std::lock_guard guard(lock);
        for (auto& item : controllers) {
            if (item.identity.id != identity.id) continue;
            if (!item.connected) {
                item.raw = raw;
                item.pad = pad;
                item.connected = true;
                ++item.generation;
                // The first interval after a replug would otherwise span the time unplugged.
                item.lastTimestamp = item.padTimestamp = 0;
                item.buttonsLast = 0;
            }
            return;
        }
        if (controllers.size() == maxControllers) return;
        Controller item;
        item.raw = raw;
        item.pad = pad;
        item.identity = std::move(identity);
        item.axes.resize(item.identity.axisCount);
        controllers.push_back(std::move(item));
    }

    void detach(RawGameController const& raw) {
        std::string id;
        try { id = utf8(raw.NonRoamableId()); } catch (...) { return; }
        std::lock_guard guard(lock);
        for (auto& item : controllers)
            if (item.identity.id == id) { item.connected = false; item.raw = nullptr; item.pad = nullptr; }
    }

    void fold(Reading& r) {
        std::lock_guard guard(lock);
        if (r.index >= controllers.size()) return;
        auto& item = controllers[r.index];
        if (!item.connected || item.generation != r.generation) return;
        ++item.polls;
        if (FAILED(r.hr)) { ++item.readFailures; item.lastReadHr = r.hr; return; }
        if (r.timestamp != item.lastTimestamp) {
            if (item.lastTimestamp && r.timestamp > item.lastTimestamp) {
                const uint64_t interval = r.timestamp - item.lastTimestamp;
                if (!item.intervalMin || interval < item.intervalMin) item.intervalMin = interval;
                item.intervalSum += interval;
                ++item.intervals;
            }
            item.lastTimestamp = r.timestamp;
            ++item.distinct;
        }
        for (unsigned i = 0; i < r.axisCount && i < item.axes.size(); ++i) item.axes[i].sample(r.axes[i]);
        uint32_t mask = 0;
        for (unsigned i = 0; i < r.buttonCount; ++i) {
            if (!r.buttons[i]) continue;
            if (i < maskedButtons) mask |= 1u << i;
            else ++item.buttonsAbove32;
        }
        if (mask != item.buttonsLast) ++item.buttonTransitions;
        item.buttonsLast = mask;
        item.buttonsSeen |= mask;
        for (auto position : r.switches) item.switchesSeen |= 1u << static_cast<unsigned>(position);
        if (r.padValid) {
            auto const& p = r.padReading;
            if (p.Timestamp != item.padTimestamp) { item.padTimestamp = p.Timestamp; ++item.padDistinct; }
            const double values[] = {p.LeftThumbstickX, p.LeftThumbstickY, p.RightThumbstickX,
                                     p.RightThumbstickY, p.LeftTrigger, p.RightTrigger};
            for (unsigned i = 0; i < 6; ++i) item.padAxes[i].sample(values[i]);
            item.padButtonsSeen |= static_cast<uint32_t>(p.Buttons);
        }
    }

    // Poll faster than any adapter reports so the timestamp interval measures the
    // device's own update rate. The default timer resolution would make Sleep(1)
    // wait about 15.6 ms, so a high-resolution waitable timer paces the loop and the
    // report carries the loop's measured period.
    void sample() {
        handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        timerHr = timer ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        std::vector<Reading> readings;
        samplerStart = qpcMicroseconds();
        while (running) {
            readings.clear();
            {
                std::lock_guard guard(lock);
                for (size_t i = 0; i < controllers.size(); ++i) {
                    auto const& item = controllers[i];
                    if (!item.connected) continue;
                    Reading r;
                    r.index = i;
                    r.generation = item.generation;
                    r.raw = item.raw;
                    r.pad = item.pad;
                    r.buttonCount = item.identity.buttonCount;
                    r.switchCount = item.identity.switchCount;
                    r.axisCount = item.identity.axisCount;
                    readings.push_back(std::move(r));
                }
            }
            for (auto& r : readings) {
                r.buttons = std::make_unique<bool[]>(r.buttonCount ? r.buttonCount : 1);
                r.switches.resize(r.switchCount);
                r.axes.resize(r.axisCount);
                try {
                    r.timestamp = r.raw.GetCurrentReading(array_view<bool>(r.buttons.get(), r.buttons.get() + r.buttonCount),
                                                          array_view<GameControllerSwitchPosition>(r.switches),
                                                          array_view<double>(r.axes));
                } catch (...) { r.hr = to_hresult(); }
                if (r.pad) {
                    try { r.padReading = r.pad.GetCurrentReading(); r.padValid = true; } catch (...) {}
                }
                fold(r);
            }
            ++samplerLoops;
            samplerLast = qpcMicroseconds();
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -5000; // 0.5 ms, relative, in 100 ns units
                if (SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer.get(), INFINITE);
                    continue;
                }
            }
            Sleep(1);
        }
    }

    bool hidSettled() const { return hidJoystick->settled() && hidGamepad->settled(); }

    std::string report(uint64_t runId, uint64_t startTick, char const* status, bool visible, bool hidDeclared) {
        std::string text = format("run_id=%llu\r\nprobe=controller-pointer\r\nstatus=%s\r\nvisible=%u\r\n"
                                  "start_tick=%llu\r\ntick=%llu\r\n",
                                  static_cast<unsigned long long>(runId), status, unsigned(visible),
                                  static_cast<unsigned long long>(startTick),
                                  static_cast<unsigned long long>(GetTickCount64()));
        unsigned raw = 0, pads = 0, nav = 0, arcade = 0, flight = 0, wheels = 0;
        try {
            raw = RawGameController::RawGameControllers().Size();
            pads = Gamepad::Gamepads().Size();
            nav = UINavigationController::UINavigationControllers().Size();
            arcade = ArcadeStick::ArcadeSticks().Size();
            flight = FlightStick::FlightSticks().Size();
            wheels = RacingWheel::RacingWheels().Size();
        } catch (...) {}
        text += format("raw_controllers=%u\r\ngamepads=%u\r\nui_navigation=%u\r\narcade_sticks=%u\r\n"
                       "flight_sticks=%u\r\nracing_wheels=%u\r\n", raw, pads, nav, arcade, flight, wheels);
        text += format("raw_added=%u\r\nraw_removed=%u\r\ngamepad_added=%u\r\ngamepad_removed=%u\r\n",
                       rawAdded.load(), rawRemoved.load(), padAdded.load(), padRemoved.load());
        const uint64_t loops = samplerLoops.load(), elapsed = samplerLast.load() - samplerStart.load();
        text += format("sampler_loops=%llu\r\nsampler_period_us=%llu\r\nsampler_timer_hr=%08lx\r\n",
                       static_cast<unsigned long long>(loops),
                       static_cast<unsigned long long>(loops > 1 && samplerLast.load() ? elapsed / (loops - 1) : 0),
                       static_cast<unsigned long>(timerHr.load()));
        text += format("hid_declared=%u\r\n", unsigned(hidDeclared));
        auto hid = [&](char const* name, HidRoute const& route) {
            // The joystick and gamepad counts are current totals: added minus removed.
            text += format("hid_%s_found=%d\r\nhid_%s=added=%d removed=%d opened=%d open_failed=%d open_hr=%08lx "
                           "watcher_status=%d watcher_hr=%08lx\r\n",
                           name, route.added.load() - route.removed.load(), name, route.added.load(),
                           route.removed.load(), route.opened.load(), route.openFailed.load(),
                           static_cast<unsigned long>(route.openHr.load()), route.status(),
                           static_cast<unsigned long>(route.watcherHr.load()));
        };
        hid("joystick", *hidJoystick);
        hid("gamepad", *hidGamepad);
        std::lock_guard guard(lock);
        text += format("controllers_seen=%u\r\n", static_cast<unsigned>(controllers.size()));
        for (unsigned n = 0; n < controllers.size(); ++n) {
            auto const& c = controllers[n];
            auto const& d = c.identity;
            text += format("controller%u=vid=0x%04x pid=0x%04x gamepad=%u wireless=%u connected=%u generation=%u "
                           "axes=%u buttons=%u switches=%u name=", n, d.vid, d.pid, unsigned(bool(c.pad) || c.padDistinct),
                           unsigned(d.wireless), unsigned(c.connected), c.generation, d.axisCount, d.buttonCount,
                           d.switchCount) + d.name + "\r\n";
            text += format("controller%u_readings=polls=%llu distinct=%llu interval_min=%llu interval_avg=%llu "
                           "read_failures=%u last_read_hr=%08lx\r\n",
                           n, static_cast<unsigned long long>(c.polls), static_cast<unsigned long long>(c.distinct),
                           static_cast<unsigned long long>(c.intervalMin),
                           static_cast<unsigned long long>(c.intervals ? c.intervalSum / c.intervals : 0),
                           c.readFailures, static_cast<unsigned long>(c.lastReadHr));
            for (unsigned i = 0; i < (std::min)(static_cast<unsigned>(c.axes.size()), reportedAxes); ++i) {
                auto const& a = c.axes[i];
                text += format("controller%u_axis%u=first=%.6f min=%.6f max=%.6f last=%.6f changes=%u step=%.6f\r\n",
                               n, i, a.first, a.min, a.max, a.last, a.changes, a.step);
            }
            text += format("controller%u_buttons=seen=0x%08x above_32=%u transitions=%u switches_seen=0x%x\r\n",
                           n, c.buttonsSeen, c.buttonsAbove32, c.buttonTransitions, c.switchesSeen);
            if (c.pad || c.padDistinct) {
                static char const* names[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
                text += format("controller%u_gamepad=distinct=%llu buttons_seen=0x%08x\r\n", n,
                               static_cast<unsigned long long>(c.padDistinct), c.padButtonsSeen);
                for (unsigned i = 0; i < 6; ++i) {
                    auto const& a = c.padAxes[i];
                    text += format("controller%u_gamepad_%s=first=%.6f min=%.6f max=%.6f changes=%u step=%.6f\r\n",
                                   n, names[i], a.first, a.min, a.max, a.changes, a.step);
                }
            }
        }
        return text;
    }
};

struct ControllerApp : ApplicationT<ControllerApp> {
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    DispatcherTimer timer{nullptr};
    TextBlock status{nullptr};
    CoreWindow core{nullptr};
    uint64_t runId = 0, startTick = 0;
    unsigned foregroundSnapshots = 0;
    bool hidDeclared = false, sampling = false;

    ControllerApp() {
        // Keep the gamepad delivered to the app. Mouse mode would turn the
        // controller into the system pointer and hide its readings.
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        WCHAR path[1024];
        UINT32 size = ARRAYSIZE(path);
        if (GetCurrentPackagePath(&size, path) == ERROR_SUCCESS) {
            const std::wstring marker = std::wstring(path) + L"\\controller-hid-declared.txt";
            hidDeclared = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
    }

    void snapshot() {
        const bool visible = core.Visible() && core.ActivationMode() == CoreWindowActivationMode::ActivatedInForeground;
        foregroundSnapshots = visible ? foregroundSnapshots + 1 : 0;
        // Sampling starts once the view has stayed foreground long enough for the
        // Added events to arrive and both HID watchers have finished their first pass.
        sampling = sampling || (foregroundSnapshots >= settleSnapshots && probe->hidSettled());
        write(L"\\controller-pointer.txt", probe->report(runId, startTick,
              sampling ? "controller-sampling" : "controller-starting", visible, hidDeclared));
        unsigned seen, connected = 0;
        {
            std::lock_guard guard(probe->lock);
            seen = static_cast<unsigned>(probe->controllers.size());
            for (auto const& c : probe->controllers) connected += c.connected;
        }
        status.Text(L"Controllers connected  " + to_hstring(connected) + L"     seen this run  " + to_hstring(seen));
    }

    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        auto view = Window::Current();
        if (view.Content()) { view.Activate(); return; }
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        runId = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        startTick = GetTickCount64();
        try {
            core = view.CoreWindow();
            auto state = probe;
            RawGameController::RawGameControllerAdded([state](auto const&, RawGameController const& raw) {
                ++state->rawAdded;
                state->attach(raw);
            });
            RawGameController::RawGameControllerRemoved([state](auto const&, RawGameController const& raw) {
                ++state->rawRemoved;
                state->detach(raw);
            });
            Gamepad::GamepadAdded([state](auto const&, auto const&) { ++state->padAdded; });
            Gamepad::GamepadRemoved([state](auto const&, auto const&) { ++state->padRemoved; });
            for (auto const& raw : RawGameController::RawGameControllers()) probe->attach(raw);
            watchHid(probe->hidJoystick, 0x04, core.Dispatcher());
            watchHid(probe->hidGamepad, 0x05, core.Dispatcher());
            probe->sampler = std::thread([state] { state->sample(); });
            core.Closed([state](auto const&, auto const&) {
                state->running = false;
                if (state->sampler.joinable()) state->sampler.join();
            });

            Grid root;
            root.Background(SolidColorBrush(Windows::UI::Color{255, 22, 30, 45}));
            StackPanel lines;
            lines.VerticalAlignment(VerticalAlignment::Center);
            lines.HorizontalAlignment(HorizontalAlignment::Center);
            auto label = [&](wchar_t const* text, double size) {
                TextBlock line;
                line.Text(text);
                line.FontSize(size);
                line.Foreground(SolidColorBrush(Windows::UI::Color{255, 255, 255, 255}));
                lines.Children().Append(line);
                return line;
            };
            label(L"Xbox controller pointer", 32);
            label(L"Leave the adapter still, then move the mouse in slow and fast strokes and press each button.", 18);
            status = label(L"Listening", 16);
            root.Children().Append(lines);
            view.Content(root);
            view.Activate();
            snapshot();
            timer = DispatcherTimer{};
            timer.Interval(std::chrono::seconds(1));
            timer.Tick([this](auto const&, auto const&) { snapshot(); });
            timer.Start();
        } catch (...) {
            write(L"\\controller-pointer.txt",
                  format("run_id=%llu\r\nprobe=controller-pointer\r\nstatus=controller-error\r\nhr=%08lx\r\n",
                         static_cast<unsigned long long>(runId), static_cast<unsigned long>(to_hresult())));
            throw;
        }
    }
};
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        Application::Start([](auto const&) { make<ControllerApp>(); });
        return 0;
    } catch (...) { return to_hresult(); }
}
