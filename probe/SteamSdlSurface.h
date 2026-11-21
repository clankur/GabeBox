#pragma once

#include "sdl-host-protocol.h"

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
        std::memcpy(bytes + sizeof(header), view_->pixels, count);
        for (std::uint32_t offset = sizeof(header) + 3; offset < length; offset += 4) {
            bytes[offset] = 255;
        }
        return transfer_;
    }
};

static std::mutex steamSdlMutex;
static std::shared_ptr<SteamSdlSurface> steamSdlSurface;
