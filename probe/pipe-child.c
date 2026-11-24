#include <windows.h>

#include "../runtime/shared/pipe-protocol.h"

void WINAPI PipeChildEntry(void) {
    const WCHAR *command = GetCommandLineW();
    const WCHAR *suffix = command;
    while (*command) {
        if (*command == L' ') suffix = command + 1;
        ++command;
    }
    BOOL lifetime = command - suffix == PROBE_PIPE_SUFFIX_LENGTH + 1 && *suffix == L'L';
    if (lifetime) ++suffix;
    if (command - suffix != PROBE_PIPE_SUFFIX_LENGTH) ExitProcess(1);

    WCHAR name[ARRAYSIZE(PROBE_PIPE_PREFIX) + PROBE_PIPE_SUFFIX_LENGTH];
    DWORD prefixLength = ARRAYSIZE(PROBE_PIPE_PREFIX) - 1;
    for (DWORD i = 0; i < prefixLength; ++i) name[i] = PROBE_PIPE_PREFIX[i];
    for (DWORD i = 0; i < PROBE_PIPE_SUFFIX_LENGTH; ++i) {
        WCHAR value = suffix[i];
        if (!((value >= L'0' && value <= L'9') || (value >= L'A' && value <= L'F'))) ExitProcess(1);
        name[prefixLength + i] = value;
    }
    name[prefixLength + PROBE_PIPE_SUFFIX_LENGTH] = 0;

    DWORD request = GetCurrentProcessId() ^ PROBE_PIPE_HELLO;
    DWORD reply = 0;
    DWORD length = 0;
    if (lifetime) {
        ProbeLifetimeHello hello = {request, (DWORD)GetTickCount64()};
        if (!CallNamedPipeW(name, &hello, sizeof(hello), &reply, sizeof(reply), &length, 5000))
            ExitProcess(GetLastError());
        ExitProcess(length == sizeof(reply) ? PROBE_LIFETIME_EXIT(reply, hello.token) : 2u);
    }
    if (!CallNamedPipeW(name, &request, sizeof(request), &reply, sizeof(reply), &length, 5000)) {
        ExitProcess(GetLastError());
    }
    ExitProcess(length == sizeof(reply) && reply == (request ^ PROBE_PIPE_REPLY) ? PROBE_PIPE_EXIT : 2u);
}
