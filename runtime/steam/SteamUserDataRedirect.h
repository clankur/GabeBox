#pragma once

#include <windows.h>
#include <fileapifromapp.h>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include "steam-userdata-path.h"

// The pinned steamclient64.dll computes its install root in a static
// initializer from its own module path, so a packaged client writes account
// state, Steam Cloud remote folders and remotecache.vdf below the read-only
// package directory and every ISteamRemoteStorage write ends in
// k_EResultIOFailure. After the host loads the client, this rebinds the
// path-taking kernel32 imports of steamclient64, tier0_s64 and vstdlib_s64 so
// that only <root>\userdata resolves to a writable app-data folder. Calling
// conventions, return values and last-error values are those of kernel32;
// other paths pass through untouched.
namespace steam_userdata {

struct RedirectState {
    std::mutex lock;
    std::wstring root;
    std::wstring target;
    std::wstring reportPath;
    std::atomic<long> mapped{0};
    std::atomic<long> failed{0};
    std::atomic<long> reported{0};
};

inline RedirectState& State() {
    static RedirectState state;
    return state;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PathFn = BOOL(WINAPI*)(LPCWSTR);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using SetFileAttributesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileExWFn = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using CopyFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using CopyFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, void*, LPVOID, LPBOOL, DWORD);
using ReplaceFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using GetTempFileNameWFn = UINT(WINAPI*)(LPCWSTR, LPCWSTR, UINT, LPWSTR);
using GetFullPathNameWFn = DWORD(WINAPI*)(LPCWSTR, DWORD, LPWSTR, LPWSTR*);

struct Originals {
    std::atomic<void*> createFile{};
    std::atomic<void*> createDirectory{};
    std::atomic<void*> removeDirectory{};
    std::atomic<void*> deleteFile{};
    std::atomic<void*> getAttributes{};
    std::atomic<void*> setAttributes{};
    std::atomic<void*> findFirst{};
    std::atomic<void*> findFirstEx{};
    std::atomic<void*> move{};
    std::atomic<void*> moveEx{};
    std::atomic<void*> copy{};
    std::atomic<void*> copyEx{};
    std::atomic<void*> replace{};
    std::atomic<void*> tempName{};
    std::atomic<void*> fullPath{};
};

inline Originals& Original() {
    static Originals originals;
    return originals;
}

template <class Fn>
inline Fn Load(std::atomic<void*>& original) {
    return reinterpret_cast<Fn>(original.load());
}

// Records the first failed redirected operation once, outside Steam's paths.
inline void ReportFailure(const wchar_t* operation, DWORD error) {
    auto& state = State();
    state.failed.fetch_add(1);
    if (state.reported.exchange(1) || state.reportPath.empty()) return;
    const auto create = Load<CreateFileWFn>(Original().createFile);
    if (!create) return;
    HANDLE file = create(state.reportPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[160];
    int length = 0;
    for (const wchar_t* c = L"steam_userdata=redirect-failed\r\noperation="; *c; ++c) text[length++] = static_cast<char>(*c);
    for (const wchar_t* c = operation; *c && length < 100; ++c) text[length++] = static_cast<char>(*c);
    const char* digits = "0123456789ABCDEF";
    const char* prefix = "\r\nerror=0x";
    for (const char* c = prefix; *c; ++c) text[length++] = *c;
    for (int shift = 28; shift >= 0; shift -= 4) text[length++] = digits[(error >> shift) & 0xF];
    text[length++] = '\r';
    text[length++] = '\n';
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

// Holds the path a kernel32 call should receive.
class MappedPath {
public:
    explicit MappedPath(LPCWSTR path) : path_(path) {
        auto& state = State();
        if (!path || state.root.empty()) return;
        std::wstring_view view(path);
        std::wstring full;
        if (!IsAbsolute(WithoutVerbatimPrefix(view))) {
            const auto resolve = Load<GetFullPathNameWFn>(Original().fullPath);
            if (!resolve) return;
            DWORD size = resolve(path, 0, nullptr, nullptr);
            if (!size) return;
            full.resize(size);
            size = resolve(path, size, full.data(), nullptr);
            if (!size || size >= full.size()) return;
            full.resize(size);
            view = full;
        }
        if (auto mapped = Redirect(view, state.root, state.target)) {
            storage_ = std::move(*mapped);
            path_ = storage_.c_str();
            redirected_ = true;
            state.mapped.fetch_add(1);
        }
    }
    LPCWSTR get() const { return path_; }
    bool redirected() const { return redirected_; }

private:
    LPCWSTR path_;
    std::wstring storage_;
    bool redirected_ = false;
};

template <class Result>
inline Result Finish(Result result, bool failed, bool redirected, const wchar_t* operation) {
    if (failed && redirected) {
        const DWORD error = GetLastError();
        ReportFailure(operation, error);
        SetLastError(error);
    }
    return result;
}

inline HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                 DWORD disposition, DWORD flags, HANDLE source) {
    MappedPath path(name);
    HANDLE result = Load<CreateFileWFn>(Original().createFile)(path.get(), access, share, security, disposition, flags, source);
    return Finish(result, result == INVALID_HANDLE_VALUE, path.redirected(), L"CreateFileW");
}

inline BOOL WINAPI CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security) {
    MappedPath path(name);
    BOOL result = Load<CreateDirectoryWFn>(Original().createDirectory)(path.get(), security);
    return Finish(result, !result && GetLastError() != ERROR_ALREADY_EXISTS, path.redirected(), L"CreateDirectoryW");
}

inline BOOL WINAPI RemoveDirectoryW(LPCWSTR name) {
    MappedPath path(name);
    return Load<PathFn>(Original().removeDirectory)(path.get());
}

inline BOOL WINAPI DeleteFileW(LPCWSTR name) {
    MappedPath path(name);
    return Load<PathFn>(Original().deleteFile)(path.get());
}

inline DWORD WINAPI GetFileAttributesW(LPCWSTR name) {
    MappedPath path(name);
    return Load<GetFileAttributesWFn>(Original().getAttributes)(path.get());
}

inline BOOL WINAPI SetFileAttributesW(LPCWSTR name, DWORD attributes) {
    MappedPath path(name);
    return Load<SetFileAttributesWFn>(Original().setAttributes)(path.get(), attributes);
}

inline HANDLE WINAPI FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    MappedPath path(name);
    return Load<FindFirstFileWFn>(Original().findFirst)(path.get(), data);
}

inline HANDLE WINAPI FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                      LPVOID filter, DWORD flags) {
    MappedPath path(name);
    return Load<FindFirstFileExWFn>(Original().findFirstEx)(path.get(), level, data, search, filter, flags);
}

inline BOOL WINAPI MoveFileW(LPCWSTR from, LPCWSTR to) {
    MappedPath source(from), destination(to);
    BOOL result = Load<MoveFileWFn>(Original().move)(source.get(), destination.get());
    return Finish(result, !result, source.redirected() || destination.redirected(), L"MoveFileW");
}

inline BOOL WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    MappedPath source(from), destination(to);
    BOOL result = Load<MoveFileExWFn>(Original().moveEx)(source.get(), destination.get(), flags);
    return Finish(result, !result, source.redirected() || destination.redirected(), L"MoveFileExW");
}

inline BOOL WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    MappedPath source(from), destination(to);
    return Load<CopyFileWFn>(Original().copy)(source.get(), destination.get(), failIfExists);
}

inline BOOL WINAPI CopyFileExW(LPCWSTR from, LPCWSTR to, void* progress, LPVOID data, LPBOOL cancel,
                               DWORD flags) {
    MappedPath source(from), destination(to);
    return Load<CopyFileExWFn>(Original().copyEx)(source.get(), destination.get(), progress, data, cancel, flags);
}

inline BOOL WINAPI ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID exclude,
                                LPVOID reserved) {
    MappedPath target(replaced), source(replacement), saved(backup);
    BOOL result = Load<ReplaceFileWFn>(Original().replace)(target.get(), source.get(), saved.get(), flags, exclude, reserved);
    return Finish(result, !result, target.redirected() || source.redirected(), L"ReplaceFileW");
}

inline UINT WINAPI GetTempFileNameW(LPCWSTR directory, LPCWSTR prefix, UINT unique, LPWSTR name) {
    MappedPath path(directory);
    return Load<GetTempFileNameWFn>(Original().tempName)(path.get(), prefix, unique, name);
}

struct ImportHook {
    const char* name;
    std::atomic<void*>* original;
    void* replacement;
};

template <class Fn>
inline ImportHook Hook(const char* name, std::atomic<void*>& original, Fn replacement) {
    return {name, &original, reinterpret_cast<void*>(replacement)};
}

inline bool SameAscii(const char* left, const char* right) {
    for (;; ++left, ++right) {
        char a = *left, b = *right;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
        if (!a) return true;
    }
}

// Rebinds the named kernel32 imports of one loaded image. Returns the number of
// import slots that now point at a replacement.
template <std::size_t Count>
inline int RebindImports(HMODULE module, ImportHook (&hooks)[Count]) {
    if (!module) return 0;
    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return 0;
    int rebound = 0;
    for (auto* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry) {
        if (!SameAscii(reinterpret_cast<const char*>(base + entry->Name), "kernel32.dll") || !entry->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (auto& hook : hooks) {
                if (!SameAscii(reinterpret_cast<const char*>(import->Name), hook.name)) continue;
                auto* slot = reinterpret_cast<void**>(&slots->u1.Function);
                void* current = *slot;
                if (current != hook.replacement) {
                    void* expected = nullptr;
                    hook.original->compare_exchange_strong(expected, current);
                }
                if (!hook.replacement) break;
                if (current == hook.replacement) {
                    ++rebound;
                    break;
                }
                ULONG previous = 0;
                if (VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
                    InterlockedExchangePointer(slot, hook.replacement);
                    VirtualProtectFromApp(slot, sizeof(void*), previous, &previous);
                    ++rebound;
                }
                break;
            }
        }
    }
    return rebound;
}

// Installs the redirect for a loaded steamclient64.dll. The companion
// libraries are already loaded as its imports; LoadPackagedLibrary only adds a
// reference to them for the duration of the rebind.
inline std::wstring InstallRedirect(HMODULE client, std::wstring const& localState) {
    auto& state = State();
    std::lock_guard guard(state.lock);
    if (state.root.empty()) {
        std::wstring module(MAX_PATH, L'\0');
        DWORD size = 0;
        for (;;) {
            size = GetModuleFileNameW(client, module.data(), static_cast<DWORD>(module.size()));
            if (!size || size < module.size()) break;
            module.resize(module.size() * 2);
        }
        if (!size) return L"steam_userdata=module-path-error\r\n";
        module.resize(size);
        const auto slash = module.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return L"steam_userdata=module-path-error\r\n";
        state.target = localState + L"\\SteamUserData";
        state.reportPath = localState + L"\\steam-userdata-report.txt";
        if (!CreateDirectoryFromAppW(state.target.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return L"steam_userdata=target-error\r\n";
        state.root = module.substr(0, slash);
    }

    auto& original = Original();
    ImportHook hooks[] = {
        Hook("GetFullPathNameW", original.fullPath, static_cast<GetFullPathNameWFn>(nullptr)),
        Hook("CreateFileW", original.createFile, &steam_userdata::CreateFileW),
        Hook("CreateDirectoryW", original.createDirectory, &steam_userdata::CreateDirectoryW),
        Hook("RemoveDirectoryW", original.removeDirectory, &steam_userdata::RemoveDirectoryW),
        Hook("DeleteFileW", original.deleteFile, &steam_userdata::DeleteFileW),
        Hook("GetFileAttributesW", original.getAttributes, &steam_userdata::GetFileAttributesW),
        Hook("SetFileAttributesW", original.setAttributes, &steam_userdata::SetFileAttributesW),
        Hook("FindFirstFileW", original.findFirst, &steam_userdata::FindFirstFileW),
        Hook("FindFirstFileExW", original.findFirstEx, &steam_userdata::FindFirstFileExW),
        Hook("MoveFileW", original.move, &steam_userdata::MoveFileW),
        Hook("MoveFileExW", original.moveEx, &steam_userdata::MoveFileExW),
        Hook("CopyFileW", original.copy, &steam_userdata::CopyFileW),
        Hook("CopyFileExW", original.copyEx, &steam_userdata::CopyFileExW),
        Hook("ReplaceFileW", original.replace, &steam_userdata::ReplaceFileW),
        Hook("GetTempFileNameW", original.tempName, &steam_userdata::GetTempFileNameW),
    };
    int rebound = RebindImports(client, hooks);
    for (const auto* name : {L"tier0_s64.dll", L"vstdlib_s64.dll"}) {
        HMODULE companion = LoadPackagedLibrary(name, 0);
        rebound += RebindImports(companion, hooks);
        if (companion) FreeLibrary(companion);
    }
    return L"steam_userdata=redirected\r\nsteam_userdata_imports=" + std::to_wstring(rebound) + L"\r\n";
}

}  // namespace steam_userdata
