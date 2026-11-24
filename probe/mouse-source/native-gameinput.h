#pragma once
// Compile only for the standalone raw-device control, with the v0 SDK header.
#include <winrt/Windows.System.Threading.h>
#include <array>
#include <mutex>
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
#include "native-gameinput-versions.h"
#endif
#include <GameInput.h>

struct NativeMouseDevices {
    struct Sample {
        winrt::com_ptr<IGameInputDevice> device;
        int family = -2;
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
                const auto info = source ? source->GetDeviceInfo() : nullptr;
                family = info ? static_cast<int>(info->deviceFamily) : -2;
            }
            const uint64_t nextTimestamp = value->GetTimestamp();
            if (reads == 0 || nextTimestamp != timestamp) ++distinctReadings;
            timestamp = nextTimestamp;
            if (reads++) {
                moves += state.positionX != previous.positionX || state.positionY != previous.positionY;
                buttons += state.buttons != previous.buttons;
                wheels += state.wheelX != previous.wheelX || state.wheelY != previous.wheelY;
            }
            previous = state;
        }
    };
    // One launch takes one connection snapshot. Only reference-counted device
    // interfaces survive enumeration. No device identifiers go to disk.
    std::mutex lock;
    std::array<Sample, 8> devices{};
    Sample unfiltered;
#ifdef MOUSE_PROBE_GAMEINPUT_VERSIONS
    MouseInterfaceVersions versions;
#ifdef MOUSE_PROBE_GAMEINPUT_REDIST
    HRESULT packagedLoad = E_PENDING, packagedFactory = E_PENDING;
    decltype(versions.v4) packaged;
#endif
#endif
    unsigned count = 0, omitted = 0;
    bool initializeExport = false;
    uint64_t runId;
    WCHAR path[1024]{};
    explicit NativeMouseDevices(uint64_t id) : runId(id) {}
    static void CALLBACK connected(GameInputCallbackToken, void* context, IGameInputDevice* device,
                                   uint64_t, GameInputDeviceStatus status, GameInputDeviceStatus) noexcept {
        if (!(status & GameInputDeviceConnected)) return;
        auto& self = *static_cast<NativeMouseDevices*>(context);
        std::lock_guard guard(self.lock);
        for (unsigned i = 0; i < self.count; ++i)
            if (self.devices[i].device.get() == device) return;
        if (self.count == self.devices.size()) { ++self.omitted; return; }
        auto& sample = self.devices[self.count++];
        sample.device.copy_from(device);
        if (auto info = device->GetDeviceInfo()) sample.family = static_cast<int>(info->deviceFamily);
    }
    void report(char const* stage, HRESULT hr) noexcept {
        char text[2200];
        int used = std::snprintf(text, sizeof(text),
            "run_id=%llu stage=%s hr=%08lx tick=%llu devices=%u omitted=%u initialize_export=%u\r\n",
            runId, stage, static_cast<unsigned long>(hr), GetTickCount64(), count, omitted, unsigned(initializeExport));
        for (unsigned i = 0; i <= count; ++i) {
            auto const& s = i == count ? unfiltered : devices[i];
            used += std::snprintf(text + used, sizeof(text) - used,
                "%s=%u family=%d reads=%u moves=%u buttons=%u wheels=%u distinct_readings=%u timestamp=%llu device_status=%x hr=%08lx\r\n",
                i == count ? "default" : "device", i == count ? 0 : i, s.family,
                s.reads, s.moves, s.buttons, s.wheels, s.distinctReadings, s.timestamp, s.status, static_cast<unsigned long>(s.hr));
        }
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
                for (auto& device : sample.devices) device.device = nullptr;
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
        input->StopCallback(token);
        // Drain on the worker before reading the fixed list or freeing context.
        if (!input->UnregisterCallback(token, 1000000)) {
            // This one-shot executable may still have a callback in flight.
            // Keep its context and module alive until process exit on timeout.
            std::lock_guard guard(lock);
            static auto pendingContext = owner;
            static auto pendingInput = input;
            module.value = nullptr;
            report("unregister-timeout", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            return;
        }
        auto end = GetTickCount64() + 180000;
        ULONGLONG nextWrite = 0;
        while (GetTickCount64() < end && InterlockedCompareExchange(&view->foreground, 0, 0)) {
            for (unsigned i = 0; i < count; ++i) devices[i].read(input.get());
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
        report("finished", S_OK);
    }
    static void start(uint64_t id, std::shared_ptr<NativePanelMouseProbe> view) {
        winrt::Windows::System::Threading::ThreadPool::RunAsync([id, view = std::move(view)](auto const&) {
            auto sample = std::make_shared<NativeMouseDevices>(id);
            sample->run(view, sample);
        });
    }
};
