/* Compare Unity's native LocalLow call with the shared known-folder adapter. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>
#include <combaseapi.h>
#include "../win32-runtime/shell-lookup.h"

DECLSPEC_IMPORT DECLSPEC_NORETURN void WINAPI ExitProcess(UINT code);

typedef HRESULT (WINAPI *KnownFolder)(const GUID *, DWORD, HANDLE, PWSTR *);
static const GUID localLow = {0xa520a1a4u, 0x1780, 0x4ff6, {0xbd, 0x18, 0x16, 0x73, 0x43, 0xc5, 0xaf, 0x16}};
static WCHAR packagePath[1024];

static unsigned NativeStatus(HMODULE shell) {
    if (!shell) return 1;
    const KnownFolder knownFolder = (KnownFolder)(void *)GetProcAddress(shell, "SHGetKnownFolderPath");
    if (!knownFolder) return 2;
    PWSTR path = NULL;
    const HRESULT result = knownFolder(&localLow, 0, NULL, &path);
    unsigned status = 0;
    if (FAILED(result)) status = 0x80u | (HRESULT_CODE(result) & 0x7fu);
    else if (!path || !*path) status = 3;
    else {
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) status = 4;
    }
    CoTaskMemFree(path);
    return status;
}

static unsigned AdapterStatus(HMODULE shell) {
    if (!shell) return 1;
    if (!LoadPackagedLibrary(L"XboxWin32Desktop.dll", 0)) return 2;
    UINT32 characters = ARRAYSIZE(packagePath);
    if (GetCurrentPackagePath(&characters, packagePath) != ERROR_SUCCESS ||
            !SetEnvironmentVariableW(L"XBOX_WIN32_LOCAL_LOW", packagePath)) return 3;
    SetLastError(0x711u);
    const KnownFolder knownFolder = (KnownFolder)(void *)ScopedShellExport(shell, "SHGetKnownFolderPath");
    if (!knownFolder || GetLastError() != 0x711u) return 4;
    PWSTR path = NULL;
    const HRESULT result = knownFolder(&localLow, 0, NULL, &path);
    unsigned status = 0;
    if (FAILED(result)) status = 0x80u | (HRESULT_CODE(result) & 0x7fu);
    else if (!path) status = 5;
    else {
        unsigned index = 0;
        while (packagePath[index] && path[index] == packagePath[index]) ++index;
        if (path[index] != packagePath[index]) status = 6;
    }
    CoTaskMemFree(path);
    return status;
}

void WINAPI ChildEntry(void) {
    SetEnvironmentVariableW(L"XBOX_WIN32_LOCAL_LOW", NULL);
    const HMODULE shell = LoadLibraryW(L"shell32.dll");
    const unsigned native = NativeStatus(shell);
    ExitProcess(0x71000000u | (native << 8) | AdapterStatus(shell));
}
