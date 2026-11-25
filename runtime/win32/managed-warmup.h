#pragma once
#include <windows.h>
#include "loader-trace-output.h"

/* Managed first-use warm-up bootstrap for natively hosted CoreCLR titles.

   CoreCLR runs DOTNET_STARTUP_HOOKS only when the host executes an assembly's Main
   (Assembly::ExecuteMainMethod). Godot .NET never does: an exported game calls
   hostfxr_initialize_for_dotnet_command_line for the game assembly, requests
   hdt_load_assembly_and_get_function_pointer, and loads GodotPlugins.Game.Main through it.
   When XBOX_WIN32_MANAGED_WARMUP names the packaged XboxManagedWarmup.dll, the engine's
   hostfxr_get_runtime_delegate is wrapped. After the host's first successful load through
   that delegate (the game assembly is then loaded), the same delegate loads
   XboxManagedWarmup.Entry.Start with the default ComponentEntryPoint signature. Start
   only starts a background thread and returns; any failure is recorded and the host's
   own result is returned unchanged. */

typedef int (*ManagedWarmupGetDelegate)(void *context, int type, void **delegate);
typedef int (*ManagedWarmupLoad)(const WCHAR *assembly, const WCHAR *type, const WCHAR *method,
    const WCHAR *delegateType, void *reserved, void **delegate);
typedef int (*ManagedWarmupEntry)(void *args, int size);

enum { ManagedWarmupLoadAssemblyDelegate = 5 }; /* hdt_load_assembly_and_get_function_pointer */

static PVOID volatile managedWarmupGetDelegate;
static PVOID volatile managedWarmupLoad;
static volatile LONG managedWarmupStarted;

static inline char *ManagedWarmupHex(char *out, DWORD value) {
    static const char digits[] = "0123456789abcdef";
    *out++ = '0';
    *out++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 0xf];
    return out;
}

static inline void ManagedWarmupTrace(int load, int entry) {
    static const char prefix[] = "managed-warmup bootstrap=hostfxr load=";
    static const char middle[] = " entry=";
    char line[96];
    char *out = line;
    for (const char *text = prefix; *text;) *out++ = *text++;
    out = ManagedWarmupHex(out, (DWORD)load);
    for (const char *text = middle; *text;) *out++ = *text++;
    out = ManagedWarmupHex(out, (DWORD)entry);
    *out++ = '\r';
    *out++ = '\n';
    AppendTrace(line, (DWORD)(out - line));
}

static int ManagedWarmupLoadAssembly(const WCHAR *assembly, const WCHAR *type, const WCHAR *method,
        const WCHAR *delegateType, void *reserved, void **delegate) {
    const ManagedWarmupLoad original =
        (ManagedWarmupLoad)InterlockedCompareExchangePointer(&managedWarmupLoad, NULL, NULL);
    const int status = original(assembly, type, method, delegateType, reserved, delegate);
    if (status >= 0 && !InterlockedExchange(&managedWarmupStarted, 1)) {
        const DWORD error = GetLastError();
        WCHAR path[520];
        const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_MANAGED_WARMUP", path, ARRAYSIZE(path));
        int load = (int)0x80070002; /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
        int entry = -1;
        void *start = NULL;
        if (length && length < ARRAYSIZE(path))
            load = original(path, L"XboxManagedWarmup.Entry, XboxManagedWarmup", L"Start", NULL, NULL, &start);
        if (load >= 0 && start) entry = ((ManagedWarmupEntry)start)(NULL, 0);
        ManagedWarmupTrace(load, entry);
        SetLastError(error);
    }
    return status;
}

static int ManagedWarmupGetRuntimeDelegate(void *context, int type, void **delegate) {
    const ManagedWarmupGetDelegate original =
        (ManagedWarmupGetDelegate)InterlockedCompareExchangePointer(&managedWarmupGetDelegate, NULL, NULL);
    const int status = original(context, type, delegate);
    if (status >= 0 && type == ManagedWarmupLoadAssemblyDelegate && delegate && *delegate) {
        const PVOID first = InterlockedCompareExchangePointer(&managedWarmupLoad, *delegate, NULL);
        if (!first || first == *delegate) *delegate = (void *)ManagedWarmupLoadAssembly;
    }
    return status;
}

/* Called with the export the engine resolved; returns it, or the wrapper when the profile
   selected the warm-up and the export is hostfxr's runtime-delegate entry. */
static inline FARPROC ManagedWarmupExport(HMODULE module, const char *name, FARPROC resolved) {
    static const char wanted[] = "hostfxr_get_runtime_delegate";
    if (!resolved || !module || (ULONG_PTR)name <= 0xffffu || name[0] != 'h') return resolved;
    unsigned index = 0;
    while (wanted[index] && name[index] == wanted[index]) ++index;
    if (name[index] != wanted[index]) return resolved;
    const DWORD error = GetLastError();
    FARPROC result = resolved;
    if (GetEnvironmentVariableW(L"XBOX_WIN32_MANAGED_WARMUP", NULL, 0) > 1 &&
            module == GetModuleHandleW(L"hostfxr.dll")) {
        const PVOID first = InterlockedCompareExchangePointer(&managedWarmupGetDelegate, (PVOID)resolved, NULL);
        if (!first || first == (PVOID)resolved) result = (FARPROC)(void *)ManagedWarmupGetRuntimeDelegate;
    }
    SetLastError(error);
    return result;
}
