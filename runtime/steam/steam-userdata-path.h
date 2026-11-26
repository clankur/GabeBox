#pragma once

#include <optional>
#include <string>
#include <string_view>

// Steam's client library takes its install root from the directory that holds
// steamclient64.dll and keeps per-account state below <root>\userdata,
// including each title's Steam Cloud folder <account>\<app>\remote and its
// remotecache.vdf. A packaged client's root is the read-only package directory,
// so this maps that one subtree onto a writable folder and leaves every other
// path unchanged.
namespace steam_userdata {

inline bool IsSeparator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

inline wchar_t Fold(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

inline std::wstring_view WithoutVerbatimPrefix(std::wstring_view path) {
    if (path.size() >= 4 && path.substr(0, 4) == L"\\\\?\\") path.remove_prefix(4);
    return path;
}

inline bool IsAbsolute(std::wstring_view path) {
    if (path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) return true;
    const wchar_t drive = Fold(path.empty() ? L'\0' : path[0]);
    return path.size() >= 3 && drive >= L'a' && drive <= L'z' && path[1] == L':' && IsSeparator(path[2]);
}

// Consumes prefix from path at offset, comparing ASCII case-insensitively and
// treating any run of either separator as one separator.
inline bool ConsumePrefix(std::wstring_view path, std::size_t& offset, std::wstring_view prefix) {
    std::size_t i = offset, j = 0;
    while (j < prefix.size()) {
        if (i >= path.size()) return false;
        if (IsSeparator(prefix[j])) {
            if (!IsSeparator(path[i])) return false;
            while (j < prefix.size() && IsSeparator(prefix[j])) ++j;
            while (i < path.size() && IsSeparator(path[i])) ++i;
            continue;
        }
        if (Fold(path[i]) != Fold(prefix[j])) return false;
        ++i;
        ++j;
    }
    offset = i;
    return true;
}

inline bool HasParentComponent(std::wstring_view path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = start;
        while (end < path.size() && !IsSeparator(path[end])) ++end;
        if (path.substr(start, end - start) == L"..") return true;
        start = end + 1;
    }
    return false;
}

// Returns target plus the part of path below <root>\userdata when path names
// that folder or anything inside it. Relative paths, paths outside the folder
// and paths that climb with ".." are left to the caller unchanged.
inline std::optional<std::wstring> Redirect(std::wstring_view path, std::wstring_view root,
                                            std::wstring_view target) {
    path = WithoutVerbatimPrefix(path);
    root = WithoutVerbatimPrefix(root);
    while (!root.empty() && IsSeparator(root.back())) root.remove_suffix(1);
    while (!target.empty() && IsSeparator(target.back())) target.remove_suffix(1);
    if (root.empty() || target.empty() || !IsAbsolute(path) || HasParentComponent(path)) return std::nullopt;

    std::size_t offset = 0;
    if (!ConsumePrefix(path, offset, root) || offset >= path.size() || !IsSeparator(path[offset]))
        return std::nullopt;
    while (offset < path.size() && IsSeparator(path[offset])) ++offset;
    if (!ConsumePrefix(path, offset, L"userdata")) return std::nullopt;
    if (offset < path.size() && !IsSeparator(path[offset])) return std::nullopt;

    std::wstring mapped(target);
    bool separator = false;
    for (; offset < path.size(); ++offset) {
        if (IsSeparator(path[offset])) {
            separator = true;
            continue;
        }
        if (separator) mapped += L'\\';
        separator = false;
        mapped += path[offset];
    }
    return mapped;
}

}  // namespace steam_userdata
