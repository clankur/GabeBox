#pragma once
// Only the foreground diagnostic worker touches these samples. No key values
// or raw report payloads are read. Keep the two source lifetimes independent.
struct MouseRawReportControl {
    unsigned supportedKinds = 0, inputReports = 0, attempts = 0, reports = 0, distinct = 0;
    bool hasMouseInfo = false;
    uint64_t timestamp = 0;
    HRESULT hr = E_PENDING;

    void describe(GameInputDeviceInfo const& info) noexcept {
        supportedKinds = static_cast<unsigned>(info.supportedInput);
        inputReports = info.inputReportCount;
        hasMouseInfo = info.mouseInfo != nullptr;
    }
    void poll(IGameInput* input, IGameInputDevice* mouse) noexcept {
        if (!(supportedKinds & GameInputKindRawDeviceReport)) return;
        ++attempts;
        winrt::com_ptr<IGameInputReading> value;
        hr = input->GetCurrentReading(GameInputKindRawDeviceReport, mouse, value.put());
        if (FAILED(hr)) return;
        winrt::com_ptr<IGameInputRawDeviceReport> report;
        if (!value || !value->GetRawReport(report.put()) || !report) { hr = S_FALSE; return; }
        const auto next = value->GetTimestamp();
        if (!reports || next != timestamp) ++distinct;
        timestamp = next;
        ++reports;
    }
};

struct KeyboardReadingControl {
    winrt::com_ptr<IGameInputDevice> previousDevice;
    unsigned reads = 0, distinct = 0, countChanges = 0, peakCount = 0, lastCount = 0;
    uint64_t timestamp = 0;
    HRESULT hr = E_PENDING;

    void poll(IGameInput* input) noexcept {
        winrt::com_ptr<IGameInputReading> value;
        hr = input->GetCurrentReading(GameInputKindKeyboard, nullptr, value.put());
        if (FAILED(hr)) return;
        if (!value) { hr = S_FALSE; return; }
        winrt::com_ptr<IGameInputDevice> source;
        value->GetDevice(source.put());
        const auto next = value->GetTimestamp();
        const unsigned count = value->GetKeyCount();
        const bool sameSource = reads && previousDevice.get() == source.get();
        if (!sameSource || next != timestamp) ++distinct;
        if (sameSource && lastCount != count) ++countChanges;
        if (count > peakCount) peakCount = count;
        ++reads;
        timestamp = next;
        lastCount = count;
        previousDevice = std::move(source);
    }
};
