#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"

static UINT VerifyRawInput(void) {
    const UINT headerSize = sizeof(RAWINPUTHEADER);
    UINT size = sizeof(RAWINPUT);
    UINT count = GetRawInputBuffer(NULL, &size, headerSize);
    if (count == (UINT)-1) return 0x6D010000u | (GetLastError() & 0xffffu);
    if (count || size) return 0x6D020000u;

    RAWINPUT input;
    input.header.dwSize = 0x12345678u;
    size = sizeof(input);
    count = GetRawInputBuffer(&input, &size, headerSize);
    if (count == (UINT)-1) return 0x6D030000u | (GetLastError() & 0xffffu);
    if (count || size || input.header.dwSize != 0x12345678u) return 0x6D040000u;

    count = GetRawInputBuffer(&input, &size, 0);
    if (count != (UINT)-1 || GetLastError() != ERROR_INVALID_PARAMETER) return 0x6D050000u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

void WINAPI ChildEntry(void) {
    ExitProcess(VerifyRawInput());
}
