#pragma once

#include <string_view>
#include "SteamWin32Stage.h"

namespace steam_child_exit_trace {

inline bool WriteReport(std::wstring const& path, std::string_view text, bool append = false) noexcept {
    auto opened = CreateFile2(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, append ? OPEN_EXISTING : CREATE_ALWAYS, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return false;
    winrt::handle file{opened};
    DWORD written{};
    return WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size();
}

// Once this observer takes the stage, it keeps it available through the last
// trace write even if the presenter releases its copy of the process handle.
inline winrt::fire_and_forget WaitAndRecord(winrt::handle process,
        winrt::Windows::Storage::StorageFolder stage, std::wstring path, winrt::apartment_context caller) {
    try {
        if (!co_await winrt::resume_on_signal(process.get())) {
            WriteReport(path, "steam_shadow_child_status=wait-error\r\n");
            co_return;
        }
    } catch (...) {
        WriteReport(path, "steam_shadow_child_status=wait-error\r\n");
        co_return; // The process may still need its staged files.
    }

    try {
        DWORD code{};
        std::string report = "steam_shadow_child_status=exited\r\n";
        if (GetExitCodeProcess(process.get(), &code)) {
            report += "steam_shadow_child_exit_u32=" + std::to_string(code) + "\r\n";
        } else {
            report += "steam_shadow_child_exit_error_u32=" + std::to_string(GetLastError()) + "\r\n";
        }
        WriteReport(path, report);
    } catch (...) {
        // An optional diagnostic cannot change the launch or shutdown verdict.
    }
    try {
        co_await caller;
    } catch (...) {
        co_return;
    }
    try {
        auto trace = co_await steam_win32_stage::ReadLoaderTrace(stage);
        WriteReport(path, winrt::to_string(trace), true);
    } catch (...) {}
    steam_win32_stage::RemoveStage(stage);
}

// A failed watcher setup leaves stage lifetime with the existing presenter.
inline bool Start(HANDLE child, winrt::Windows::Storage::StorageFolder stage,
        winrt::Windows::Storage::StorageFolder output, std::wstring const& reportName) {
    try {
        winrt::handle process;
        if (!stage || !output ||
                !DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), process.put(),
                    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) return false;
        const winrt::apartment_context caller;
        auto path = std::wstring(output.Path()) + L"\\" + reportName;
        if (!WriteReport(path, "steam_shadow_child_status=watching\r\n")) return false;
        WaitAndRecord(std::move(process), std::move(stage), std::move(path), caller);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace steam_child_exit_trace
