#pragma once

#include "sdl-host-protocol.h"
#include "sdl-gpu-protocol.h"
#include "sdl-frame-copy.h"
#include "SteamWin32Stage.h"

class SteamSdlSurface {
    winrt::handle file_;
    winrt::handle gpuFile_;
    bool gpuFrameSequence_ = false;
    SdlHostMailbox *view_ = nullptr;
    mutable winrt::Windows::Storage::Streams::Buffer transfer_{nullptr};
    winrt::handle process_;
    winrt::Windows::Storage::StorageFolder stagedGame_{nullptr};
    std::vector<std::uint8_t> previousPixels_;
    std::vector<std::uint16_t> encodedPixels_;
    SdlHostFrame previousHeader_{};

    std::uint32_t PublishGamepad(std::uint32_t buttons,
            std::array<std::int32_t, SDL_HOST_PAD_AXES> const& axes) const {
        auto &pad = view_->gamepad;
        InterlockedIncrement(&pad.serial);
        InterlockedExchange(&pad.buttons, buttons & (SDL_HOST_PAD_CONNECTED | ((1u << SDL_HOST_PAD_BUTTONS) - 1)));
        for (size_t i = 0; i < axes.size(); ++i) {
            InterlockedExchange(&pad.axes[i], std::clamp(axes[i], -32768, 32767));
        }
        return static_cast<std::uint32_t>(InterlockedIncrement(&pad.serial));
    }

    template<class Mailbox> static Mailbox* OpenMailbox(std::wstring const& path, winrt::handle& file, LONG magic) {
        CREATEFILE2_EXTENDED_PARAMETERS options{};
        options.dwSize = sizeof(options);
        options.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
        options.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
        auto handle = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  CREATE_NEW, &options);
        if (handle == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        winrt::handle created{handle};
        winrt::handle mapping{CreateFileMappingFromApp(created.get(), nullptr, PAGE_READWRITE, sizeof(Mailbox), nullptr)};
        if (!mapping) winrt::throw_last_error();
        auto* view = static_cast<Mailbox*>(MapViewOfFileFromApp(mapping.get(), FILE_MAP_WRITE, 0, sizeof(Mailbox)));
        if (!view) winrt::throw_last_error();
        InterlockedExchange(&view->magic, magic);
        file = std::move(created);
        return view;
    }

public:
    enum class GpuMailbox { None, Required, Optional };

    SteamSdlSurface() = default;
    SteamSdlSurface(SteamSdlSurface const&) = delete;
    SteamSdlSurface& operator=(SteamSdlSurface const&) = delete;

    ~SteamSdlSurface() {
        Quit();
        if (view_) UnmapViewOfFile(view_);
        if (stagedGame_) steam_win32_stage::RemoveStageAfterExit(std::move(stagedGame_), std::move(process_));
    }

    bool Open(std::wstring const& path, GpuMailbox prepareGpu) {
        view_ = OpenMailbox<SdlHostMailbox>(path, file_, SDL_HOST_MAGIC);
        if (prepareGpu == GpuMailbox::None) return false;
        try {
            auto* gpu = OpenMailbox<SdlGpuMailbox>(path + L".gpu", gpuFile_, SDL_GPU_MAGIC);
            UnmapViewOfFile(gpu);
        } catch (winrt::hresult_error const&) {
            if (prepareGpu == GpuMailbox::Required) throw;
            return false;
        }
        // DXGI signals first publication through the sequence. OpenGL keeps
        // publishing a complete CPU frame until the presenter accepts GPU sharing.
        gpuFrameSequence_ = prepareGpu == GpuMailbox::Required;
        return true;
    }

    void Attach(winrt::handle process, winrt::Windows::Storage::StorageFolder stagedGame = nullptr) {
        process_ = std::move(process);
        stagedGame_ = std::move(stagedGame);
    }
    void PrimeGamepadConnection() const {
        if (view_ && !process_) PublishGamepad(SDL_HOST_PAD_CONNECTED, {0, 0, 0, 0, -32768, -32768});
    }
    bool Alive() const { return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    bool HasFrame() const {
        return view_ && (gpuFrameSequence_ ? SdlHostLoad(&view_->frame_sequence) != 0 :
                                            SdlHostLoad(&view_->frame_state) == SDL_HOST_READY);
    }

    void Quit() const {
        if (view_) InterlockedExchange(&view_->quit, 1);
    }

    std::uint32_t Input(std::uint32_t pointer, std::uint32_t keys) const {
        if (!Alive() || SdlHostLoad(&view_->quit)) return 0;
        InterlockedIncrement(&view_->input_serial);
        InterlockedExchange(&view_->input_pointer, pointer & 0x01ffffffu);
        InterlockedExchange(&view_->input_keys, keys & 0x7fu);
        return static_cast<std::uint32_t>(InterlockedIncrement(&view_->input_serial));
    }

    std::uint32_t Gamepad(std::uint32_t buttons, std::array<std::int32_t, SDL_HOST_PAD_AXES> const& axes) const {
        if (!Alive() || SdlHostLoad(&view_->quit)) return 0;
        return PublishGamepad(buttons, axes);
    }

    std::uint32_t InputAck() const {
        return Alive() ? static_cast<std::uint32_t>(SdlHostLoad(&view_->input_ack)) : 0;
    }

    std::uint32_t GamepadAck() const {
        return Alive() ? static_cast<std::uint32_t>(SdlHostLoad(&view_->gamepad.ack)) : 0;
    }

    bool KeyboardReady() const {
        return Alive() && !SdlHostLoad(&view_->quit) && SdlHostLoad(&view_->keyboard.available);
    }

    bool Key(std::uint32_t scan, bool down) const {
        if (!KeyboardReady() || !scan || scan >= SDL_HOST_KEY_LIMIT) return false;
        auto &keyboard = view_->keyboard;
        const auto write = static_cast<std::uint32_t>(SdlHostLoad(&keyboard.write));
        const auto read = static_cast<std::uint32_t>(SdlHostLoad(&keyboard.read));
        if (write - read >= SDL_HOST_KEY_QUEUE_SIZE) {
            ResetKeyboard();
            return false;
        }
        keyboard.entries[write % SDL_HOST_KEY_QUEUE_SIZE] = {static_cast<std::uint16_t>(scan), down ? uint8_t{1} : uint8_t{0}, {}};
        InterlockedExchange(&keyboard.write, static_cast<LONG>(write + 1));
        return true;
    }

    bool Text(winrt::hstring const& text) const {
        constexpr auto payload = SDL_HOST_TEXT_BYTES - 1;
        if (!KeyboardReady() || text.empty() || text.size() > 96) return false;
        const auto bytes = winrt::to_string(text);
        if (bytes.find('\0') != std::string::npos) return false;
        auto &keyboard = view_->keyboard;
        auto write = static_cast<std::uint32_t>(SdlHostLoad(&keyboard.write));
        const auto read = static_cast<std::uint32_t>(SdlHostLoad(&keyboard.read));
        for (size_t start = 0; start < bytes.size();) {
            if (write - read >= SDL_HOST_KEY_QUEUE_SIZE) return false;
            auto end = (std::min)(start + payload, bytes.size());
            while (end < bytes.size() && (static_cast<unsigned char>(bytes[end]) & 0xc0) == 0x80) --end;
            auto &entry = keyboard.entries[write++ % SDL_HOST_KEY_QUEUE_SIZE];
            entry = {};
            std::memcpy(entry.text, bytes.data() + start, end - start);
            start = end;
        }
        InterlockedExchange(&keyboard.write, static_cast<LONG>(write));
        return true;
    }

    void ResetKeyboard() const {
        if (KeyboardReady()) {
            auto &keyboard = view_->keyboard;
            InterlockedExchange(&keyboard.reset, SdlHostLoad(&keyboard.write));
        }
    }

    winrt::Windows::Storage::Streams::IBuffer Frame() const {
        if (!Alive() || InterlockedCompareExchange(&view_->frame_state, SDL_HOST_READING, SDL_HOST_READY) != SDL_HOST_READY) {
            return nullptr;
        }
        struct Release {
            SdlHostMailbox *view;
            ~Release() { InterlockedExchange(&view->frame_state, SDL_HOST_EMPTY); }
        } release{view_};
        const auto header = view_->frame;
        if (!header.width || header.width > SDL_HOST_MAX_WIDTH || !header.height || header.height > SDL_HOST_MAX_HEIGHT) {
            return nullptr;
        }
        const auto count = header.width * header.height * 4;
        const auto length = count + static_cast<std::uint32_t>(sizeof(header));
        if (!transfer_ || transfer_.Capacity() < length) {
            transfer_ = winrt::Windows::Storage::Streams::Buffer(length);
        }
        transfer_.Length(length);
        auto bytes = transfer_.data();
        std::memcpy(bytes, &header, sizeof(header));
        CopyOpaqueSdlFrame(bytes + sizeof(header), view_->pixels, count / 4);
        return transfer_;
    }

    winrt::hstring RgbFrame(std::uint32_t previousSequence) {
        if (!Frame()) return {};
        SdlHostFrame header{};
        const auto bytes = transfer_.data();
        std::memcpy(&header, bytes, sizeof(header));
        const auto count = header.width * header.height;
        const auto pixels = bytes + sizeof(header);
        const bool delta = previousHeader_.sequence == previousSequence && previousHeader_.width == header.width &&
            previousHeader_.height == header.height;
        wchar_t prefix[96];
        const auto prefixLength = swprintf_s(prefix, L"%u,%u,%u,%u,%u,%c\n", header.width, header.height,
                                             header.sequence, header.input_ack, header.input_events, delta ? L'D' : L'F');
        encodedPixels_.resize(2 * count);
        std::size_t units = 2 * count;
        if (delta) units = EncodeSdlRgbDelta(encodedPixels_.data(), pixels, previousPixels_.data(), count);
        else EncodeSdlRgb(encodedPixels_.data(), pixels, count);
        previousPixels_.assign(pixels, pixels + 4 * count);
        previousHeader_ = header;
        wchar_t *encoded;
        HSTRING_BUFFER storage;
        winrt::check_hresult(WindowsPreallocateStringBuffer(prefixLength + static_cast<UINT32>(units), &encoded, &storage));
        std::memcpy(encoded, prefix, prefixLength * sizeof(wchar_t));
        std::memcpy(encoded + prefixLength, encodedPixels_.data(), units * sizeof(wchar_t));
        HSTRING text;
        const auto hr = WindowsPromoteStringBuffer(storage, &text);
        if (FAILED(hr)) {
            WindowsDeleteStringBuffer(storage);
            winrt::throw_hresult(hr);
        }
        winrt::hstring result;
        winrt::attach_abi(result, text);
        return result;
    }
};

static std::mutex steamSdlMutex;
static std::shared_ptr<SteamSdlSurface> steamSdlSurface;
