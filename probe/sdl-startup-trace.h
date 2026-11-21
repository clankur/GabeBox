#pragma once
#include <windows.h>
#include "SDL_stdinc.h"

static void XboxSdlTrace(const char *line) {
    DWORD last_error = GetLastError();
    WCHAR path[1024];
    DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE", path, ARRAYSIZE(path));
    if (length > 0 && length < ARRAYSIZE(path)) {
        HANDLE file = CreateFileW(path, FILE_APPEND_DATA | GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            DWORD written;
            if (GetFileSizeEx(file, &size) && size.QuadPart < 4000) {
                WriteFile(file, line, (DWORD)SDL_strlen(line), &written, NULL);
            }
            CloseHandle(file);
        }
    }
    SetLastError(last_error);
}

#ifdef XBOX_SDL_MAIN_TRACE
static void XboxSdlSetWorkingDirectory(void) {
    DWORD last_error = GetLastError();
    WCHAR directory[1024];
    DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_WORKING_DIRECTORY", directory, ARRAYSIZE(directory));
    if (length > 0 && length < ARRAYSIZE(directory)) {
        XboxSdlTrace(SetCurrentDirectoryW(directory) ? "sdl_working_directory=ready\n" : "sdl_working_directory=error\n");
    }
    SetLastError(last_error);
}
#endif
