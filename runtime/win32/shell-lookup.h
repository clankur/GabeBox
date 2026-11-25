#pragma once

/* The profile's static shell binding keeps the desktop adapter loaded while
   the engine holds a dynamically resolved function pointer. */
static inline FARPROC ScopedShellExport(HMODULE module, LPCSTR name) {
    static const char knownFolder[] = "SHGetKnownFolderPath";
    if (!module || (ULONG_PTR)name <= 0xffffu) return NULL;
    unsigned index = 0;
    while (knownFolder[index] && name[index] == knownFolder[index]) ++index;
    if (name[index] != knownFolder[index]) return NULL;

    const DWORD originalError = GetLastError();
    FARPROC adapted = NULL;
    if (module == GetModuleHandleW(L"shell32.dll") &&
            GetEnvironmentVariableW(L"XBOX_WIN32_LOCAL_LOW", NULL, 0) > 1) {
        HMODULE desktop = GetModuleHandleW(L"XboxWin32Desktop.dll");
        if (desktop) adapted = GetProcAddress(desktop, knownFolder);
    }
    SetLastError(originalError);
    return adapted;
}
