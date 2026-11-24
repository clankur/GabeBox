#pragma once
// Public versioned interfaces from the same checksum-pinned NuGet package.
// v1 and v2 also declare incompatible C-linkage factory prototypes. This
// probe only uses QueryInterface, so keep their unused declarations distinct.
#define GameInputCreate UnusedVersionOneFactory
#include <v1/GameInput.h>
#undef GameInputCreate
#undef GAMEINPUT_API_VERSION
#define GameInputCreate UnusedVersionTwoFactory
#include <v2/GameInput.h>
#undef GameInputCreate
#undef GAMEINPUT_API_VERSION
#include <v3/GameInput.h>
#undef GAMEINPUT_API_VERSION
#include <v4/GameInput.h>

template<typename Input, typename Reading, typename State, auto mouseKind>
struct MouseInterfaceSample {
    winrt::com_ptr<Input> input;
    HRESULT query = E_PENDING, reading = E_PENDING;
    State last{};
    unsigned reads = 0, changes = 0, distinct = 0;
    uint64_t timestamp = 0;
    int family = -2;
    void open(IUnknown* system, IID const& iid) {
        query = system->QueryInterface(iid, input.put_void());
    }
    template<typename Progress> void poll(Progress progress) {
        if (!input) return;
        winrt::com_ptr<Reading> value;
        progress("packaged-get-current-reading");
        reading = input->GetCurrentReading(mouseKind, nullptr, value.put());
        State state{};
        if (FAILED(reading)) return;
        progress("packaged-get-mouse-state");
        if (!value || !value->GetMouseState(&state)) { reading = S_FALSE; return; }
        progress("packaged-get-timestamp");
        auto next = value->GetTimestamp();
        if (!reads || timestamp != next) ++distinct;
        timestamp = next;
        if constexpr (std::is_same_v<Reading, GameInput::v4::IGameInputReading>) {
            winrt::com_ptr<GameInput::v4::IGameInputDevice> source;
            progress("packaged-get-source");
            value->GetDevice(source.put());
            family = source ? static_cast<int>(source->GetDeviceFamily()) : -2;
        }
        if (reads++) {
            changes += state.positionX != last.positionX || state.positionY != last.positionY ||
                state.absolutePositionX != last.absolutePositionX || state.absolutePositionY != last.absolutePositionY ||
                state.buttons != last.buttons || state.wheelX != last.wheelX || state.wheelY != last.wheelY;
        }
        last = state;
    }
    void poll() { poll([](char const*) {}); }
};
struct MouseInterfaceVersions {
#define MOUSE_INTERFACE_SAMPLE(N) MouseInterfaceSample<GameInput::v##N::IGameInput, \
    GameInput::v##N::IGameInputReading, GameInput::v##N::GameInputMouseState, GameInput::v##N::GameInputKindMouse> v##N
    MOUSE_INTERFACE_SAMPLE(1);
    MOUSE_INTERFACE_SAMPLE(2);
    MOUSE_INTERFACE_SAMPLE(3);
    MOUSE_INTERFACE_SAMPLE(4);
#undef MOUSE_INTERFACE_SAMPLE
    void open(IUnknown* system) {
        v1.open(system, GameInput::v1::IID_IGameInput);
        v2.open(system, GameInput::v2::IID_IGameInput);
        v3.open(system, GameInput::v3::IID_IGameInput);
        v4.open(system, GameInput::v4::IID_IGameInput);
    }
    void poll() { v1.poll(); v2.poll(); v3.poll(); v4.poll(); }
};
