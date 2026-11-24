#pragma once
// Compile only for the standalone raw-device control, with the v0 SDK header.
#include <winrt/Windows.System.Threading.h>
#include <array>
#include <mutex>
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
#include "native-gameinput-versions.h"
#endif
#include <GameInput.h>
#include "native-input-control.h"

struct NativeMouseDevices {
    struct Sample {
        winrt::com_ptr<IGameInputDevice> device;
        winrt::com_ptr<IGameInputDevice> lastSource;
        int family = -2;
        unsigned vendor = 0, product = 0, sourceSwitches = 0;
        bool baseline = false;
        MouseRawReportControl raw;
        GameInputMouseState previous{};
        unsigned reads = 0, moves = 0, buttons = 0, wheels = 0, distinctReadings = 0;
        uint64_t timestamp = 0;
        unsigned status = 0;
        HRESULT hr = E_PENDING;
        void read(IGameInput* input) noexcept {
            if (device) status = static_cast<unsigned>(device->GetDeviceStatus());
            winrt::com_ptr<IGameInputReading> value;
            hr = input->GetCurrentReading(GameInputKindMouse, device.get(), value.put());
            GameInputMouseState state{};
            if (FAILED(hr)) return;
            if (!value || !value->GetMouseState(&state)) { hr = S_FALSE; return; }
            if (!device) {
                winrt::com_ptr<IGameInputDevice> source;
                value->GetDevice(source.put());
                if (lastSource.get() != source.get()) {
                    if (lastSource) ++sourceSwitches;
                    baseline = false;
                    lastSource = source;
                }
                const auto info = source ? source->GetDeviceInfo() : nullptr;
                family = info ? static_cast<int>(info->deviceFamily) : -2;
            }
            const uint64_t nextTimestamp = value->GetTimestamp();
            if (!baseline || nextTimestamp != timestamp) ++distinctReadings;
            timestamp = nextTimestamp;
            ++reads;
            if (baseline) {
                moves += state.positionX != previous.positionX || state.positionY != previous.positionY;
                buttons += state.buttons != previous.buttons;
                wheels += state.wheelX != previous.wheelX || state.wheelY != previous.wheelY;
            }
            baseline = true;
            previous = state;
        }
    };
    // The callback owns the registry; the worker polls a retained snapshot.
    // A slot generation denotes one connection, even if the same mouse returns.
    struct Slot {
        winrt::com_ptr<IGameInputDevice> device;
        unsigned generation = 0, connects = 0, disconnects = 0;
        unsigned status = 0;
        uint64_t transitionTime = 0;
    };
    struct Registry {
        std::array<Slot, 8> slots{};
        unsigned count = 0, omitted = 0, transitions = 0;
    };
    std::mutex lock;
    Registry registry, observed;
    std::array<Sample, 8> devices{};
    Sample unfiltered;
    KeyboardReadingControl keyboard;
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
    MouseInterfaceVersions versions;
#ifdef MOUSE_PROBE_GAMEINPUT_REDIST
    HRESULT packagedLoad = E_PENDING, packagedFactory = E_PENDING;
    decltype(versions.v4) packaged;
#endif
#endif
    unsigned initialDevices = 0;
    bool initializeExport = false;
    uint64_t runId;
    WCHAR path[1024]{};
    explicit NativeMouseDevices(uint64_t id) : runId(id) {}
    static void CALLBACK connected(GameInputCallbackToken, void* context, IGameInputDevice* device,
                                   uint64_t when, GameInputDeviceStatus status, GameInputDeviceStatus) noexcept {
        auto& self = *static_cast<NativeMouseDevices*>(context);
        std::lock_guard guard(self.lock);
        auto& registry = self.registry;
        Slot* available = nullptr;
        for (auto& slot : registry.slots) {
            if (slot.device.get() == device) {
                slot.status = static_cast<unsigned>(status);
                if (!(status & GameInputDeviceConnected)) {
                    slot.device = nullptr;
                    slot.transitionTime = when;
                    ++slot.disconnects;
                    ++registry.transitions;
                }
                return;
            }
            if (!slot.device && !available) available = &slot;
        }
        if (!(status & GameInputDeviceConnected)) return;
        if (!available) { ++registry.omitted; return; }
        if (!available->generation) ++registry.count;
        available->device.copy_from(device);
        ++available->generation;
        ++available->connects;
        ++registry.transitions;
        available->status = static_cast<unsigned>(status);
        available->transitionTime = when;
    }
    void refreshRegistry() {
        Registry next;
        {
            std::lock_guard guard(lock);
            next = registry;
        }
        // Even if a device reuses its interface address, the aggregate sample
        // must not count a position reset on reconnect as mouse movement.
        if (next.transitions != observed.transitions) unfiltered.baseline = false;
        for (unsigned i = 0; i < next.count; ++i) {
            auto const& slot = next.slots[i];
            auto& sample = devices[i];
            if (slot.generation != observed.slots[i].generation) {
                sample = {};
                sample.device = slot.device;
                const auto* info = slot.device ? slot.device->GetDeviceInfo() : nullptr;
                if (info) {
                    sample.family = static_cast<int>(info->deviceFamily);
                    sample.raw.describe(*info);
                    if (info->vendorId && info->vendorId != 0xffff && info->productId && info->productId != 0xffff) {
                        sample.vendor = info->vendorId;
                        sample.product = info->productId;
                    }
                }
            }
            if (!slot.device) {
                sample.device = nullptr;
                sample.status = slot.status;
                sample.hr = GAMEINPUT_E_DEVICE_DISCONNECTED;
            }
        }
        observed = std::move(next);
    }
    void report(char const* stage, HRESULT hr) noexcept {
        char text[8192];
        const unsigned count = observed.count;
        int used = std::snprintf(text, sizeof(text),
            "run_id=%llu stage=%s hr=%08lx tick=%llu devices=%u omitted=%u initialize_export=%u initial_devices=%u transitions=%u\r\n",
            runId, stage, static_cast<unsigned long>(hr), GetTickCount64(), count, observed.omitted, unsigned(initializeExport), initialDevices, observed.transitions);
        for (unsigned i = 0; i <= count; ++i) {
            auto const& s = i == count ? unfiltered : devices[i];
            used += std::snprintf(text + used, sizeof(text) - used,
                "%s=%u family=%d reads=%u moves=%u buttons=%u wheels=%u distinct_readings=%u timestamp=%llu device_status=%x hr=%08lx\r\n",
                i == count ? "default" : "device", i == count ? 0 : i, s.family,
                s.reads, s.moves, s.buttons, s.wheels, s.distinctReadings, s.timestamp, s.status, static_cast<unsigned long>(s.hr));
            if (i == count) {
                used += std::snprintf(text + used, sizeof(text) - used, "default_source_switches=%u\r\n", s.sourceSwitches);
            } else {
                auto const& slot = observed.slots[i];
                used += std::snprintf(text + used, sizeof(text) - used,
                    "slot=%u generation=%u connected=%u connects=%u disconnects=%u transition_time=%llu",
                    i, slot.generation, unsigned(bool(slot.device)), slot.connects, slot.disconnects, slot.transitionTime);
                if (s.vendor && s.product)
                    used += std::snprintf(text + used, sizeof(text) - used, " vid=%04x pid=%04x", s.vendor, s.product);
                used += std::snprintf(text + used, sizeof(text) - used,
                    " supported_input=%x input_report_count=%u mouse_info=%u raw_attempts=%u raw_reports=%u "
                    "raw_distinct=%u raw_timestamp=%llu raw_hr=%08lx\r\n",
                    s.raw.supportedKinds, s.raw.inputReports, unsigned(s.raw.hasMouseInfo), s.raw.attempts,
                    s.raw.reports, s.raw.distinct, s.raw.timestamp, static_cast<unsigned long>(s.raw.hr));
            }
        }
        used += std::snprintf(text + used, sizeof(text) - used,
            "keyboard_reads=%u keyboard_distinct=%u keyboard_timestamp=%llu key_count_changes=%u peak_key_count=%u keyboard_hr=%08lx\r\n",
            keyboard.reads, keyboard.distinct, keyboard.timestamp, keyboard.countChanges, keyboard.peakCount,
            static_cast<unsigned long>(keyboard.hr));
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
        auto version = [&](int api, auto const& value) {
            used += std::snprintf(text + used, sizeof(text) - used,
                "version=%d query_hr=%08lx reading_hr=%08lx reads=%u changes=%u\r\n",
                api, static_cast<unsigned long>(value.query), static_cast<unsigned long>(value.reading),
                value.reads, value.changes);
        };
#ifdef MOUSE_PROBE_GAMEINPUT_REDIST
        used += std::snprintf(text + used, sizeof(text) - used,
            "packaged_v4 load_hr=%08lx factory_hr=%08lx reading_hr=%08lx reads=%u distinct=%u "
            "timestamp=%llu family=%d changes=%u\r\n", static_cast<unsigned long>(packagedLoad),
            static_cast<unsigned long>(packagedFactory), static_cast<unsigned long>(packaged.reading),
            packaged.reads, packaged.distinct, packaged.timestamp, packaged.family, packaged.changes);
#endif
        version(1, versions.v1); version(2, versions.v2); version(3, versions.v3); version(4, versions.v4);
#endif
        winrt::file_handle file{CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, nullptr)};
        DWORD written;
        if (file) WriteFile(file.get(), text, used, &written, nullptr);
    }
    void run(std::shared_ptr<NativePanelMouseProbe> const& view,
             std::shared_ptr<NativeMouseDevices> const& owner) noexcept {
        if (FAILED(FhdLocalPath(path, ARRAYSIZE(path))) ||
            !FhdFrameTrace::suffix(path, ARRAYSIZE(path), L"\\mouse-gameinput.txt")) return;
        HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) { report("apartment-error", apartment); return; }
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartmentLifetime;
#ifdef MOUSE_PROBE_GAMEINPUT_REDIST
        report("opening-packaged-library", E_PENDING);
        struct Redist {
            HMODULE handle;
            NativeMouseDevices& sample;
            ~Redist() {
                sample.packaged.input = nullptr;
                if (handle) FreeLibrary(handle);
            }
        } redist{LoadPackagedLibrary(L"GameInputRedist.dll", 0), *this};
        packagedLoad = redist.handle ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        if (FAILED(packagedLoad)) { report("packaged-library-error", packagedLoad); return; }
        auto initialize = reinterpret_cast<decltype(&GameInput::v4::GameInputInitialize)>(
            GetProcAddress(redist.handle, "GameInputInitialize"));
        if (!initialize) { packagedFactory = HRESULT_FROM_WIN32(GetLastError()); report("packaged-export-error", packagedFactory); return; }
        report("calling-packaged-factory", E_PENDING);
        packagedFactory = initialize(GameInput::v4::IID_IGameInput, packaged.input.put_void());
        if (FAILED(packagedFactory) || !packaged.input) {
            report("packaged-factory-error", FAILED(packagedFactory) ? packagedFactory : E_POINTER);
            return;
        }
        report("packaged-factory-ready", S_OK);
        ULONGLONG until = GetTickCount64() + 180000, nextReport = 0;
        bool first = true;
        while (GetTickCount64() < until && InterlockedCompareExchange(&view->foreground, 0, 0)) {
            packaged.poll([&](char const* stage) { if (first) report(stage, E_PENDING); });
            first = false;
            if (GetTickCount64() >= nextReport) {
                report("packaged-sampling", packaged.reading);
                nextReport = GetTickCount64() + 1000;
            }
            Sleep(25);
        }
        report("packaged-finished", packaged.reading);
        return;
#endif
        report("opening-system-library", E_PENDING);
        struct Module {
            HMODULE value;
            NativeMouseDevices& sample;
            ~Module() {
                if (!value) return;
                sample.devices = {};
                sample.unfiltered = {};
                sample.keyboard = {};
                sample.registry = {};
                sample.observed = {};
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
                sample.versions = {};
#endif
                FreeLibrary(value);
            }
        } module{LoadLibraryExW(L"gameinput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), *this};
        if (!module.value) { report("library-error", HRESULT_FROM_WIN32(GetLastError())); return; }
        initializeExport = GetProcAddress(module.value, "GameInputInitialize") != nullptr;
        auto create = reinterpret_cast<HRESULT (WINAPI*)(IGameInput**)>(GetProcAddress(module.value, "GameInputCreate"));
        if (!create) { report("export-error", HRESULT_FROM_WIN32(GetLastError())); return; }
        winrt::com_ptr<IGameInput> input;
        HRESULT hr = create(input.put());
        if (FAILED(hr) || !input) { report("factory-error", FAILED(hr) ? hr : E_POINTER); return; }
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
        versions.open(input.get());
#endif
        report("enumerating", E_PENDING);
        GameInputCallbackToken token{};
        hr = input->RegisterDeviceCallback(nullptr, GameInputKindMouse, GameInputDeviceConnected,
            GameInputBlockingEnumeration, this, connected, &token);
        if (FAILED(hr)) { report("enumeration-error", hr); return; }
        refreshRegistry();
        initialDevices = observed.count;
        auto end = GetTickCount64() + 180000;
        ULONGLONG nextWrite = 0;
        while (GetTickCount64() < end && InterlockedCompareExchange(&view->foreground, 0, 0)) {
            refreshRegistry();
            for (unsigned i = 0; i < observed.count; ++i) {
                auto& sample = devices[i];
                if (!sample.device) continue;
                sample.read(input.get());
                sample.raw.poll(input.get(), sample.device.get());
            }
            keyboard.poll(input.get());
            unfiltered.read(input.get());
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
            versions.poll();
#endif
            if (GetTickCount64() >= nextWrite) {
                report("sampling", S_OK);
                nextWrite = GetTickCount64() + 1000;
            }
            Sleep(25);
        }
        input->StopCallback(token);
        // Stop and drain outside the registry lock before freeing callback state.
        if (!input->UnregisterCallback(token, 1000000)) {
            // This one-shot executable may still have a callback in flight.
            // Keep its context and module alive until process exit on timeout.
            static auto pendingContext = owner;
            static auto pendingInput = input;
            module.value = nullptr;
            report("unregister-timeout", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            return;
        }
        refreshRegistry();
        report("finished", S_OK);
    }
    static void start(uint64_t id, std::shared_ptr<NativePanelMouseProbe> view) {
        winrt::Windows::System::Threading::ThreadPool::RunAsync([id, view = std::move(view)](auto const&) {
            auto sample = std::make_shared<NativeMouseDevices>(id);
            sample->run(view, sample);
        });
    }
};
