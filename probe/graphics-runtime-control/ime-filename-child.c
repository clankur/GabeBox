#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include "../../runtime/shared/child-process.h"

static UINT VerifyImeFilename(void) {
    char filename[MAX_PATH];
    filename[0] = 'x';
    SetLastError(0x1234u);
    if (ImmGetIMEFileNameA(NULL, filename, sizeof(filename)) || GetLastError() != 0x1234u)
        return 0x6F010000u;
    if (filename[0] != 'x') return 0x6F020000u;

    typedef HKL (WINAPI *GetLayout)(DWORD);
    HMODULE user = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user) return 0x6F030000u | (GetLastError() & 0xffffu);
    const GetLayout getLayout = (GetLayout)(void*)GetProcAddress(user, "GetKeyboardLayout");
    const HKL layout = getLayout ? getLayout(0) : NULL;
    FreeLibrary(user);
    if (!layout) return 0x6F040000u;

    const UINT needed = ImmGetIMEFileNameA(layout, NULL, 0);
    if (needed >= sizeof(filename)) return 0x6F050000u;
    const UINT copied = ImmGetIMEFileNameA(layout, filename, sizeof(filename));
    if (copied != needed || (copied && filename[copied])) return 0x6F060000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(VerifyImeFilename());
}
