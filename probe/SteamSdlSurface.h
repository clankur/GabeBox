#pragma once

#include "sdl-host-protocol.h"
#include "sdl-frame-copy.h"
#include <utility>

class SteamSdlSurface {
    winrt::handle file_;
    SdlHostMailbox *view_ = nullptr;
    mutable winrt::Windows::Storage::Streams::Buffer transfer_{nullptr};
    winrt::handle process_;

public:
    SteamSdlSurface() = default;
    SteamSdlSurface(SteamSdlSurface const&) = delete;
    SteamSdlSurface& operator=(SteamSdlSurface const&) = delete;

    ~SteamSdlSurface() {
        Quit();
        if (view_) UnmapViewOfFile(view_);
    }

    void Open(std::wstring const& path) {
        CREATEFILE2_EXTENDED_PARAMETERS options{};
        options.dwSize = sizeof(options);
        options.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
        options.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
        auto file = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                CREATE_NEW, &options);
        if (file == INVALID_HANDLE_VALUE) winrt::throw_last_error();
        file_.attach(file);
        winrt::handle mapping{CreateFileMappingFromApp(file_.get(), nullptr, PAGE_READWRITE,
                                                       sizeof(SdlHostMailbox), nullptr)};
        if (!mapping) winrt::throw_last_error();
        view_ = static_cast<SdlHostMailbox*>(MapViewOfFileFromApp(mapping.get(), FILE_MAP_WRITE, 0, sizeof(SdlHostMailbox)));
        if (!view_) winrt::throw_last_error();
        InterlockedExchange(&view_->magic, SDL_HOST_MAGIC);
    }

    void Attach(winrt::handle process) { process_ = std::move(process); }
    bool Alive() const { return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    bool HasFrame() const { return view_ && SdlHostLoad(&view_->frame_state) == SDL_HOST_READY; }

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
        auto &pad = view_->gamepad;
        InterlockedIncrement(&pad.serial);
        InterlockedExchange(&pad.buttons, buttons & (SDL_HOST_PAD_CONNECTED | ((1u << SDL_HOST_PAD_BUTTONS) - 1)));
        for (size_t i = 0; i < axes.size(); ++i) {
            InterlockedExchange(&pad.axes[i], std::clamp(axes[i], -32768, 32767));
        }
        return static_cast<std::uint32_t>(InterlockedIncrement(&pad.serial));
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

    winrt::hstring RgbFrame() const {
        if (!Frame()) return {};
        SdlHostFrame header{};
        const auto bytes = transfer_.data();
        std::memcpy(&header, bytes, sizeof(header));
        wchar_t prefix[96];
        const auto prefixLength = swprintf_s(prefix, L"%u,%u,%u,%u,%u\n", header.width, header.height,
                                             header.sequence, header.input_ack, header.input_events);
        const auto count = header.width * header.height;
        wchar_t *encoded;
        HSTRING_BUFFER storage;
        winrt::check_hresult(WindowsPreallocateStringBuffer(prefixLength + 2 * count, &encoded, &storage));
        std::memcpy(encoded, prefix, prefixLength * sizeof(wchar_t));
        EncodeSdlRgb(reinterpret_cast<std::uint16_t *>(encoded + prefixLength), bytes + sizeof(header), count);
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

    // Async writers retain their pixels until WriteAsync completes. A second
    // reader can claim a new frame while the first writer owns its buffer.
    winrt::Windows::Storage::Streams::Buffer TakeFrame() {
        if (!Frame()) return nullptr;
        return std::exchange(transfer_, nullptr);
    }

    void ReuseFrame(winrt::Windows::Storage::Streams::Buffer buffer) {
        if (!transfer_) transfer_ = std::move(buffer);
    }
};

static std::mutex steamSdlMutex;
static std::shared_ptr<SteamSdlSurface> steamSdlSurface;
