#pragma once
#include "sdl-rumble-protocol.h"

// The Windows SDK declares this with its own exception specification, so a
// local declaration beside it is a redefinition for MSVC. Prefer the SDK
// header wherever it exists and keep the declaration for toolchains without it.
#if defined(__has_include)
#if __has_include(<fileapifromapp.h>)
#define SDL_RUMBLE_SDK_FILEAPIFROMAPP 1
#endif
#endif
#ifdef SDL_RUMBLE_SDK_FILEAPIFROMAPP
#include <fileapifromapp.h>
#else
#ifdef __cplusplus
extern "C"
#endif
DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD,
    LPCREATEFILE2_EXTENDED_PARAMETERS);
#endif

static inline SdlRumbleMailbox *MapSdlRumbleMailbox(LPCWSTR path) {
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    const BOOL valid = GetFileSizeEx(file, &size) && size.QuadPart == (LONGLONG)sizeof(SdlRumbleMailbox);
    HANDLE mapping = valid ? CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return NULL;
    SdlRumbleMailbox *box = (SdlRumbleMailbox *)MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(*box));
    CloseHandle(mapping);
    if (box && InterlockedCompareExchange(&box->magic, 0, 0) != SDL_RUMBLE_MAGIC) {
        UnmapViewOfFile(box);
        return NULL;
    }
    return box;
}

static inline BOOL AppendSdlRumbleSuffix(WCHAR *path, SIZE_T length, SIZE_T capacity) {
    const WCHAR suffix[] = SDL_RUMBLE_SUFFIX;
    if (!length || length + ARRAYSIZE(suffix) > capacity) return FALSE;
    for (SIZE_T i = 0; i < ARRAYSIZE(suffix); ++i) path[length + i] = suffix[i];
    return TRUE;
}

static inline SdlRumbleMailbox *OpenSdlRumbleMailbox(LPCWSTR mailboxPath) {
    WCHAR path[1032];
    SIZE_T length = 0;
    while (length < ARRAYSIZE(path) && mailboxPath[length]) {
        path[length] = mailboxPath[length];
        ++length;
    }
    return AppendSdlRumbleSuffix(path, length, ARRAYSIZE(path)) ? MapSdlRumbleMailbox(path) : NULL;
}
