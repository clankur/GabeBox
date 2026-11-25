#define WIN32_LEAN_AND_MEAN
#include <stdbool.h>
#include <windows.h>

DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD,
    LPCREATEFILE2_EXTENDED_PARAMETERS);

typedef struct CrashReceipt {
    DWORD version;
    DWORD processId;
    FILETIME utc;
} CrashReceipt;

static volatile LONG pending;

/* The legacy report payload belongs to the game. The local receipt records
   that the game asked to report a crash, without copying its opaque payload. */
void __cdecl XboxReportBug(const void *report) {
    if (!report) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    InterlockedIncrement(&pending);
    DWORD status = ERROR_NOT_SUPPORTED;
    WCHAR path[1024];
    DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_CRASH_REPORT", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path)) {
        HANDLE file = CreateFile2FromAppW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            status = GetLastError();
        } else {
            LARGE_INTEGER size;
            if (!GetFileSizeEx(file, &size)) {
                status = GetLastError();
            } else if (size.QuadPart + sizeof(CrashReceipt) > 64 * 1024) {
                status = ERROR_DISK_FULL;
            } else {
                CrashReceipt receipt = {1, GetCurrentProcessId(), {0, 0}};
                GetSystemTimeAsFileTime(&receipt.utc);
                DWORD written = 0;
                status = !WriteFile(file, &receipt, sizeof(receipt), &written, NULL) ? GetLastError() :
                    (written == sizeof(receipt) ? ERROR_SUCCESS : ERROR_WRITE_FAULT);
            }
            CloseHandle(file);
        }
    }
    InterlockedDecrement(&pending);
    SetLastError(status);
}

bool __cdecl XboxWaitReportComplete(void) {
    return InterlockedCompareExchange(&pending, 0, 0) == 0;
}
