#pragma once
#include <windows.h>

/* This desktop library is requested by games that only need the basic XInput API.
   A build-matched profile can opt in to hosting the newer XInput names too
   (XBOX_WIN32_HOSTED_XINPUT=1), for desktop search sequences such as Unity's and
   SDL's that try xinput1_4.dll and xinput1_3.dll before xinput9_1_0.dll.
   Leave qualified paths and other XInput versions to the normal loader. */
static BOOL HostedXInputName(LPCWSTR name) {
    if (CompareStringOrdinal(name, -1, L"xinput9_1_0.dll", -1, TRUE) == CSTR_EQUAL) return TRUE;
    if (CompareStringOrdinal(name, -1, L"xinput1_4.dll", -1, TRUE) != CSTR_EQUAL &&
            CompareStringOrdinal(name, -1, L"xinput1_3.dll", -1, TRUE) != CSTR_EQUAL) return FALSE;
    WCHAR enabled[2];
    return GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", enabled, ARRAYSIZE(enabled)) == 1 && enabled[0] == L'1';
}

static HMODULE LoadHostedXInput(LPCWSTR name) {
    if (!name) return NULL;
    const DWORD previous = GetLastError();
    const BOOL hosted = HostedXInputName(name) && GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) > 1;
    SetLastError(previous);
    return hosted ? LoadPackagedLibrary(L"XboxWin32XInput910.dll", 0) : NULL;
}
