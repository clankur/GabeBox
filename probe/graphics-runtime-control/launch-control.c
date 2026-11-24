/* Exercise the Slay the Spire 2 import mappings using an original test child. */
#include "../../runtime/shared/child-process.h"

#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
typedef long HRESULT;

IMPORT HRESULT WINAPI CoInitializeEx(void *reserved, unsigned long mode);
IMPORT void WINAPI CoUninitialize(void);
IMPORT HRESULT WINAPI GetErrorInfo(unsigned long reserved, void **error);
IMPORT HRESULT WINAPI SetErrorInfo(unsigned long reserved, void *error);
IMPORT unsigned int WINAPI MapVirtualKeyExA(unsigned int key, unsigned int type, void *layout);
IMPORT unsigned int WINAPI MapVirtualKeyExW(unsigned int key, unsigned int type, void *layout);
IMPORT int WINAPI GetPointerPenInfo(unsigned int pointerId, void *penInfo);
IMPORT unsigned long WINAPI GetLastError(void);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

static unsigned int CheckImports(void) {
    if (CoInitializeEx(0, 0) < 0) return 0x6601;
    void *error = (void *)1;
    const int com = SetErrorInfo(0, 0) == 0 && GetErrorInfo(0, &error) == 1 && !error;
    CoUninitialize();
    if (!com) return 0x6602;

    const unsigned int scan = MapVirtualKeyExW(0x41, 0, 0);
    if (!scan || MapVirtualKeyExA(0x41, 0, 0) != scan) return 0x6603;
    if (MapVirtualKeyExA(0x41, 2, 0) != 'A') return 0x6604;

    unsigned long long penInfo[24] = {0};
    if (GetPointerPenInfo(0, penInfo) || !GetLastError()) return 0x6605;
    return PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned int (*volatile testImports)(void) = CheckImports;

void WINAPI ChildEntry(void) {
    ExitProcess(testImports());
}
