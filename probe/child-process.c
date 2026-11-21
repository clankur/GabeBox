#include "child-process.h"

typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef unsigned short *PWSTR;
typedef const unsigned short *PCWSTR;

typedef struct STARTUPINFOW {
    DWORD cb;
    PWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    unsigned char *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;

typedef struct PROCESS_INFORMATION {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

_Static_assert(sizeof(STARTUPINFOW) == 104 && sizeof(PROCESS_INFORMATION) == 24, "x64 process ABI");

__declspec(dllimport) int __stdcall CreateProcessW(PCWSTR applicationName, PWSTR commandLine,
    void *processAttributes, void *threadAttributes, int inheritHandles, DWORD creationFlags,
    void *environment, PCWSTR currentDirectory, STARTUPINFOW *startup, PROCESS_INFORMATION *processInfo);
__declspec(dllimport) DWORD __stdcall GetLastError(void);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE handle, DWORD milliseconds);
__declspec(dllimport) int __stdcall GetExitCodeProcess(HANDLE process, DWORD *exitCode);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE handle);

__declspec(dllexport) void __stdcall RunPackagedChild(PCWSTR path, ChildProcessResult *result) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;

    startup.cb = sizeof(startup);
    startup.lpReserved = startup.lpDesktop = startup.lpTitle = (PWSTR)0;
    startup.dwX = startup.dwY = startup.dwXSize = startup.dwYSize = 0;
    startup.dwXCountChars = startup.dwYCountChars = 0;
    startup.dwFillAttribute = startup.dwFlags = 0;
    startup.wShowWindow = startup.cbReserved2 = 0;
    startup.lpReserved2 = (void *)0;
    startup.hStdInput = startup.hStdOutput = startup.hStdError = (HANDLE)0;

    result->error = result->exitCode = 0;
    result->waitResult = 0xffffffffu;
    result->created = CreateProcessW(path, (PWSTR)0, (void *)0, (void *)0, 0,
        0, (void *)0, (PCWSTR)0, &startup, &process);
    if (!result->created) {
        result->error = GetLastError();
        return;
    }

    result->waitResult = WaitForSingleObject(process.hProcess, 3000);
    if (result->waitResult == 0 && !GetExitCodeProcess(process.hProcess, &result->exitCode)) {
        result->error = GetLastError();
    } else if (result->waitResult == 0xffffffffu) {
        result->error = GetLastError();
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
