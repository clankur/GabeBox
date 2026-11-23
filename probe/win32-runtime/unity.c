#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

#define TRACE(literal) AppendTrace(literal, sizeof(literal) - 1)

typedef int (__cdecl *PlayerMain)(HINSTANCE, HINSTANCE, LPWSTR, int);

int __cdecl XboxUnityMain(HINSTANCE instance, HINSTANCE previous, LPWSTR arguments, int show) {
    static const WCHAR engine[] = L"UnityPlayer.dll";
    WCHAR path[1024];
    DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) {
        TRACE("load UnityPlayer.dll error=launcher-path\r\n");
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    while (length && path[length - 1] != L'\\') --length;
    if (!length || length + ARRAYSIZE(engine) > ARRAYSIZE(path)) {
        TRACE("load UnityPlayer.dll error=launcher-directory\r\n");
        return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }
    for (DWORD offset = 0; offset < ARRAYSIZE(engine); ++offset) path[length + offset] = engine[offset];

    HMODULE player = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!player) {
        const DWORD error = GetLastError();
        TRACE("load UnityPlayer.dll error=load-failed\r\n");
        return HRESULT_FROM_WIN32(error);
    }
    TRACE("load UnityPlayer.dll ok source=adjacent\r\n");
    const PlayerMain entry = (PlayerMain)(void *)GetProcAddress(player, "UnityMain");
    if (!entry) {
        const DWORD error = GetLastError();
        TRACE("export UnityMain error=unavailable\r\n");
        FreeLibrary(player);
        return HRESULT_FROM_WIN32(error);
    }
    TRACE("export UnityMain ok\r\n");
    // Unity may retain process-scoped services after its entry point returns.
    return entry(instance, previous, arguments, show);
}
