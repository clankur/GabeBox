#pragma once

/* The engine's static input imports keep the adapter loaded while it holds
   the function pointer. Ordinal lookups and unrelated modules stay native. */
static inline FARPROC HostedXInputExport(HMODULE module, LPCSTR name) {
    if (!module || (ULONG_PTR)name <= 0xffffu || name[0] != 'X') return NULL;
    static const char *const exports[] = {"XInputGetState", "XInputGetCapabilities", "XInputSetState"};
    for (unsigned i = 0; i < ARRAYSIZE(exports); ++i) {
        unsigned index = 0;
        while (exports[i][index] && exports[i][index] == name[index]) ++index;
        if (exports[i][index] != name[index]) continue;

        const DWORD previous = GetLastError();
        WCHAR enabled[2];
        FARPROC adapted = NULL;
        if (GetEnvironmentVariableW(L"XBOX_WIN32_HOST_XINPUT", enabled, ARRAYSIZE(enabled)) == 1 && enabled[0] == L'1' &&
                GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) > 1 &&
                (module == GetModuleHandleW(L"xinput1_4.dll") || module == GetModuleHandleW(L"xinput1_3.dll") ||
                 module == GetModuleHandleW(L"xinput9_1_0.dll"))) {
            HMODULE input = GetModuleHandleW(L"XboxWin32Input.dll");
            if (input) adapted = GetProcAddress(input, name);
        }
        SetLastError(previous);
        return adapted;
    }
    return NULL;
}
