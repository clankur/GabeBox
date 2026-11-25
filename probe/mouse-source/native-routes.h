#pragma once
// Physical mouse delivery routes, sampled together in one foreground run.
// Each group is independent: a missing export, a refused device, or a denied
// HID collection records its own Win32 error or HRESULT and never stops the
// remaining groups. Raw input belongs to the window's own thread queue, so the
// XAML view registers and drains it; everything else runs on this worker.
#include <windows.h>
#include <corewindow.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <array>
#include <vector>

// Counters the XAML view owns. Only interlocked writes cross the thread.
struct RawInputRoute {
    volatile LONG registered = 0, sinkError = 0, foregroundError = 0, window = 0;
    volatile LONG bufferCalls = 0, bufferRecords = 0, mouseRecords = 0;
    volatile LONG motion = 0, buttons = 0, wheel = 0, peeked = 0, queryError = 0;
    volatile LONG exports = 0, listError = 0, mice = 0, keyboards = 0, other = 0;
    volatile LONG pointerMode = -1;
};

struct NativeMouseRoutes {
    using Window = winrt::Windows::UI::Core::CoreWindow;
    using DeviceInformation = winrt::Windows::Devices::Enumeration::DeviceInformation;
    using HidDevice = winrt::Windows::Devices::HumanInterfaceDevice::HidDevice;
    using FileAccessMode = winrt::Windows::Storage::FileAccessMode;

    struct Collection {
        char const* name;
        uint16_t page, usage;
        unsigned found = 0;
        HRESULT selector = E_PENDING, open = E_PENDING;
        volatile LONG reports = 0, payload = 0;
        winrt::event_token token{};
        HidDevice device{nullptr};
    };

    uint64_t runId;
    WCHAR reportPath[1024]{};
    ULONGLONG startTick = GetTickCount64();
    RawInputRoute& raw;
    // 0x01/0x02 and 0x01/0x06 are reserved for the system on Windows. The
    // consumer-control and vendor-defined collections are not, so a mouse that
    // publishes one of those can be read without the reserved collection.
    std::array<Collection, 4> collections{{
        {"mouse", 0x0001, 0x0002}, {"keyboard", 0x0001, 0x0006},
        {"consumer", 0x000c, 0x0001}, {"vendor", 0xff00, 0x0001}}};
    int mousePresent = -1, mouseButtons = -1, keyboardPresent = -1;
    unsigned pointerDevices = 0, pointerMice = 0;
    HRESULT capabilitiesHr = E_PENDING, pointerHr = E_PENDING;
    unsigned cursorReads = 0, cursorMoves = 0, asyncTransitions = 0, asyncButtons = 0;
    POINT cursor{};
    DWORD cursorError = 0, movePointsError = 0;
    int movePoints = -1;
    WCHAR hardware[256]{};

    NativeMouseRoutes(uint64_t id, RawInputRoute& shared) noexcept : runId{id}, raw{shared} {}

    void report(char const* stage) noexcept {
        if (!reportPath[0] &&
            (FAILED(FhdLocalPath(reportPath, ARRAYSIZE(reportPath))) ||
             !FhdFrameTrace::suffix(reportPath, ARRAYSIZE(reportPath), L"\\mouse-routes.txt"))) return;
        char text[2048];
        int n = std::snprintf(text, sizeof(text),
            "run_id=%llu\r\nprobe=native-mouse-routes\r\nstage=%s\r\nstart_tick=%llu\r\ntick=%llu\r\n"
            "user32=0x%lx pointer_mode=%ld window=%ld rawinput_registered=%ld rawinput_sink_error=%ld rawinput_foreground_error=%ld\r\n"
            "raw_list_error=%ld raw_mice=%ld raw_keyboards=%ld raw_other=%ld\r\n"
            "raw_buffer_calls=%ld raw_buffer_records=%ld raw_mouse_records=%ld raw_motion=%ld "
            "raw_buttons=%ld raw_wheel=%ld raw_peeked=%ld raw_query_error=%ld\r\n"
            "cursor_reads=%u cursor_moves=%u cursor_x=%ld cursor_y=%ld cursor_error=%lu\r\n"
            "async_buttons=%u async_transitions=%u move_points=%d move_points_error=%lu\r\n"
            "mouse_present=%d mouse_buttons=%d keyboard_present=%d capabilities_hr=%08lx "
            "pointer_devices=%u pointer_mice=%u pointer_hr=%08lx\r\n",
            static_cast<unsigned long long>(runId), stage,
            static_cast<unsigned long long>(startTick), static_cast<unsigned long long>(GetTickCount64()),
            static_cast<unsigned long>(InterlockedCompareExchange(&raw.exports, 0, 0)),
            InterlockedCompareExchange(&raw.pointerMode, 0, 0),
            InterlockedCompareExchange(&raw.window, 0, 0),
            InterlockedCompareExchange(&raw.registered, 0, 0),
            InterlockedCompareExchange(&raw.sinkError, 0, 0),
            InterlockedCompareExchange(&raw.foregroundError, 0, 0),
            InterlockedCompareExchange(&raw.listError, 0, 0),
            InterlockedCompareExchange(&raw.mice, 0, 0),
            InterlockedCompareExchange(&raw.keyboards, 0, 0),
            InterlockedCompareExchange(&raw.other, 0, 0),
            InterlockedCompareExchange(&raw.bufferCalls, 0, 0),
            InterlockedCompareExchange(&raw.bufferRecords, 0, 0),
            InterlockedCompareExchange(&raw.mouseRecords, 0, 0),
            InterlockedCompareExchange(&raw.motion, 0, 0),
            InterlockedCompareExchange(&raw.buttons, 0, 0),
            InterlockedCompareExchange(&raw.wheel, 0, 0),
            InterlockedCompareExchange(&raw.peeked, 0, 0),
            InterlockedCompareExchange(&raw.queryError, 0, 0),
            cursorReads, cursorMoves, cursor.x, cursor.y, cursorError,
            asyncButtons, asyncTransitions, movePoints, movePointsError,
            mousePresent, mouseButtons, keyboardPresent, static_cast<unsigned long>(capabilitiesHr),
            pointerDevices, pointerMice, static_cast<unsigned long>(pointerHr));
        for (auto const& item : collections) {
            if (n < 0 || n >= static_cast<int>(sizeof(text))) break;
            n += std::snprintf(text + n, sizeof(text) - n,
                "hid_%s_found=%u hid_%s_selector_hr=%08lx hid_%s_open_hr=%08lx hid_%s_reports=%ld hid_%s_payload=%ld\r\n",
                item.name, item.found, item.name, static_cast<unsigned long>(item.selector),
                item.name, static_cast<unsigned long>(item.open),
                item.name, InterlockedCompareExchange(const_cast<volatile LONG*>(&item.reports), 0, 0),
                item.name, InterlockedCompareExchange(const_cast<volatile LONG*>(&item.payload), 0, 0));
        }
        if (n > 0 && n < static_cast<int>(sizeof(text)) && hardware[0]) {
            char ids[256];
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, hardware, -1, ids, sizeof(ids), nullptr, nullptr);
            if (bytes > 0) n += std::snprintf(text + n, sizeof(text) - n, "hid_ids=%s\r\n", ids);
        }
        if (n <= 0) return;
        file_handle file{CreateFile2(reportPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     CREATE_ALWAYS, nullptr)};
        DWORD written;
        if (file) WriteFile(file.get(), text, static_cast<DWORD>(n), &written, nullptr);
    }

    // Keep only the vendor and product of each collection so the report never
    // carries a device serial number.
    void noteHardware(winrt::hstring const& id) noexcept {
        auto const text = std::wstring_view(id);
        const auto vendor = text.find(L"VID_");
        if (vendor == std::wstring_view::npos || text.size() < vendor + 17) return;
        const auto part = text.substr(vendor, 17);
        auto const existing = std::wstring_view(hardware);
        if (existing.find(part) != std::wstring_view::npos) return;
        const size_t used = existing.size();
        if (used + part.size() + 2 >= ARRAYSIZE(hardware)) return;
        WCHAR* end = hardware + used;
        if (used) *end++ = L';';
        std::copy_n(part.data(), part.size(), end);
        end[part.size()] = 0;
    }

    void openCollections() noexcept {
        for (auto& item : collections) {
            try {
                const auto selector = HidDevice::GetDeviceSelector(item.page, item.usage);
                item.selector = S_OK;
                const auto found = DeviceInformation::FindAllAsync(selector).get();
                item.found = found.Size();
                for (auto const& info : found) {
                    noteHardware(info.Id());
                    if (item.device) continue;
                    auto device = HidDevice::FromIdAsync(info.Id(), FileAccessMode::Read).get();
                    if (!device) { item.open = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); continue; }
                    item.open = S_OK;
                    item.device = device;
                    item.token = device.InputReportReceived(
                        [&item](auto const&, auto const& args) noexcept {
                            InterlockedIncrement(&item.reports);
                            try {
                                if (args.Report().Data().Length()) InterlockedIncrement(&item.payload);
                            } catch (...) {}
                        });
                }
            } catch (...) {
                if (item.selector == E_PENDING) item.selector = winrt::to_hresult();
                else item.open = winrt::to_hresult();
            }
        }
    }

    void closeCollections() noexcept {
        for (auto& item : collections) {
            if (!item.device) continue;
            try { item.device.RemoveInputReportReceived(item.token); } catch (...) {}
            try { item.device.Close(); } catch (...) {}
            item.device = nullptr;
        }
    }

    void readCapabilities() noexcept {
        try {
            winrt::Windows::Devices::Input::MouseCapabilities mouse;
            mousePresent = mouse.MousePresent();
            mouseButtons = mouse.NumberOfButtons();
            keyboardPresent = winrt::Windows::Devices::Input::KeyboardCapabilities{}.KeyboardPresent();
            capabilitiesHr = S_OK;
        } catch (...) { capabilitiesHr = winrt::to_hresult(); }
        try {
            using Kind = winrt::Windows::Devices::Input::PointerDeviceType;
            for (auto const& device : winrt::Windows::Devices::Input::PointerDevice::GetPointerDevices()) {
                ++pointerDevices;
                if (device.PointerDeviceType() == Kind::Mouse) ++pointerMice;
            }
            pointerHr = S_OK;
        } catch (...) { pointerHr = winrt::to_hresult(); }
    }

    void run(std::shared_ptr<NativePanelMouseProbe> view) noexcept {
        report("routes-starting");
        readCapabilities();
        openCollections();
        report("routes-ready");

        typedef BOOL (WINAPI *ReadCursor)(LPPOINT);
        typedef SHORT (WINAPI *ReadAsyncKey)(int);
        typedef int (WINAPI *ReadMovePoints)(UINT, LPMOUSEMOVEPOINT, LPMOUSEMOVEPOINT, int, DWORD);
        HMODULE user = GetModuleHandleW(L"user32.dll");
        if (!user) user = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto readCursor = reinterpret_cast<ReadCursor>(
            user ? GetProcAddress(user, "GetCursorPos") : nullptr);
        const auto readAsyncKey = reinterpret_cast<ReadAsyncKey>(
            user ? GetProcAddress(user, "GetAsyncKeyState") : nullptr);
        const auto readMovePoints = reinterpret_cast<ReadMovePoints>(
            user ? GetProcAddress(user, "GetMouseMovePointsEx") : nullptr);

        static const int keys[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        bool held[5]{}, baseline = false;
        POINT previous{};
        const auto end = GetTickCount64() + 180000;
        ULONGLONG nextWrite = 0;
        while (GetTickCount64() < end && InterlockedCompareExchange(&view->foreground, 0, 0)) {
            if (readCursor) {
                POINT position{};
                if (readCursor(&position)) {
                    ++cursorReads;
                    if (baseline && (position.x != previous.x || position.y != previous.y)) ++cursorMoves;
                    baseline = true;
                    previous = position;
                    cursor = position;
                } else cursorError = GetLastError();
            }
            if (readAsyncKey) {
                for (unsigned i = 0; i < 5; ++i) {
                    const bool down = (readAsyncKey(keys[i]) & 0x8000) != 0;
                    if (down != held[i]) { ++asyncTransitions; held[i] = down; }
                    if (down) ++asyncButtons;
                }
            }
            if (readMovePoints && movePoints < 0) {
                MOUSEMOVEPOINT request{previous.x & 0xffff, previous.y & 0xffff, 0, 0};
                MOUSEMOVEPOINT history[16]{};
                movePoints = readMovePoints(sizeof(request), &request, history, 16, GMMP_USE_DISPLAY_POINTS);
                if (movePoints < 0) movePointsError = GetLastError();
            }
            if (GetTickCount64() >= nextWrite) {
                report("routes-sampling");
                nextWrite = GetTickCount64() + 1000;
            }
            Sleep(25);
        }
        closeCollections();
        report("routes-finished");
    }

    static void start(uint64_t id, RawInputRoute& shared, std::shared_ptr<NativePanelMouseProbe> view) {
        winrt::Windows::System::Threading::ThreadPool::RunAsync(
            [id, &shared, view = std::move(view)](auto const&) {
                auto routes = std::make_shared<NativeMouseRoutes>(id, shared);
                routes->run(view);
            });
    }
};

// The XAML view owns the window's raw input registration and queue drain.
struct RawInputWindow {
    RawInputRoute state;
    typedef BOOL (WINAPI *Register)(PCRAWINPUTDEVICE, UINT, UINT);
    typedef UINT (WINAPI *ReadBuffer)(PRAWINPUT, PUINT, UINT);
    typedef UINT (WINAPI *ReadRecord)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    typedef UINT (WINAPI *ReadList)(PRAWINPUTDEVICELIST, PUINT, UINT);
    typedef BOOL (WINAPI *Peek)(LPMSG, HWND, UINT, UINT, UINT);
    Register registerDevices = nullptr;
    ReadBuffer readBuffer = nullptr;
    ReadRecord readRecord = nullptr;
    ReadList readList = nullptr;
    Peek peek = nullptr;
    HWND window = nullptr;

    void open(winrt::Windows::UI::Core::CoreWindow const& core) noexcept {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        if (!user) user = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!user) return;
        LONG resolved = 0;
        auto lookup = [&](unsigned bit, char const* name) {
            auto const address = GetProcAddress(user, name);
            if (address) resolved |= 1 << bit;
            return address;
        };
        registerDevices = reinterpret_cast<Register>(lookup(0, "RegisterRawInputDevices"));
        readBuffer = reinterpret_cast<ReadBuffer>(lookup(1, "GetRawInputBuffer"));
        readRecord = reinterpret_cast<ReadRecord>(lookup(2, "GetRawInputData"));
        readList = reinterpret_cast<ReadList>(lookup(3, "GetRawInputDeviceList"));
        peek = reinterpret_cast<Peek>(lookup(4, "PeekMessageW"));
        lookup(5, "GetCursorPos");
        lookup(6, "GetAsyncKeyState");
        lookup(7, "GetMouseMovePointsEx");
        InterlockedExchange(&state.exports, resolved);

        if (readList) {
            UINT count = 0;
            if (readList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == 0 && count &&
                count < 256) {
                std::vector<RAWINPUTDEVICELIST> devices(count);
                const UINT read = readList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
                if (read == static_cast<UINT>(-1)) InterlockedExchange(&state.listError, static_cast<LONG>(GetLastError()));
                else for (UINT i = 0; i < read; ++i) {
                    if (devices[i].dwType == RIM_TYPEMOUSE) InterlockedIncrement(&state.mice);
                    else if (devices[i].dwType == RIM_TYPEKEYBOARD) InterlockedIncrement(&state.keyboards);
                    else InterlockedIncrement(&state.other);
                }
            } else InterlockedExchange(&state.listError, static_cast<LONG>(GetLastError()));
        }

        try {
            auto const interop = core.as<ICoreWindowInterop>();
            if (SUCCEEDED(interop->get_WindowHandle(&window)) && window)
                InterlockedExchange(&state.window, 1);
        } catch (...) {}
        if (!registerDevices || !window) return;
        RAWINPUTDEVICE request{0x0001, 0x0002, RIDEV_INPUTSINK, window};
        if (registerDevices(&request, 1, sizeof(request))) { InterlockedExchange(&state.registered, 1); return; }
        InterlockedExchange(&state.sinkError, static_cast<LONG>(GetLastError()));
        request.dwFlags = 0;
        if (registerDevices(&request, 1, sizeof(request))) InterlockedExchange(&state.registered, 2);
        else InterlockedExchange(&state.foregroundError, static_cast<LONG>(GetLastError()));
    }

    void account(RAWINPUT const& record) noexcept {
        InterlockedIncrement(&state.bufferRecords);
        if (record.header.dwType != RIM_TYPEMOUSE) return;
        InterlockedIncrement(&state.mouseRecords);
        auto const& mouse = record.data.mouse;
        if (mouse.lLastX || mouse.lLastY) InterlockedIncrement(&state.motion);
        if (mouse.usButtonFlags & 0x03ff) InterlockedIncrement(&state.buttons);
        if (mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) InterlockedIncrement(&state.wheel);
    }

    void drain() noexcept {
        if (!InterlockedCompareExchange(&state.registered, 0, 0)) return;
        InterlockedIncrement(&state.bufferCalls);
        if (readBuffer) {
            alignas(8) BYTE storage[8192];
            UINT size = sizeof(storage);
            const UINT records = readBuffer(reinterpret_cast<PRAWINPUT>(storage), &size, sizeof(RAWINPUTHEADER));
            if (records == static_cast<UINT>(-1)) InterlockedExchange(&state.queryError, static_cast<LONG>(GetLastError()));
            else {
                PRAWINPUT next = reinterpret_cast<PRAWINPUT>(storage);
                for (UINT i = 0; i < records; ++i) {
                    account(*next);
                    next = NEXTRAWINPUTBLOCK(next);
                }
            }
        }
        if (!peek || !readRecord) return;
        MSG message;
        while (peek(&message, window, WM_INPUT, WM_INPUT, PM_REMOVE)) {
            InterlockedIncrement(&state.peeked);
            RAWINPUT record{};
            UINT size = sizeof(record);
            if (readRecord(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT, &record, &size,
                           sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) account(record);
        }
    }
};
