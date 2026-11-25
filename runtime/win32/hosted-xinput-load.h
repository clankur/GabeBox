#pragma once
#include <windows.h>

/* A build-matched profile may select the basic hosted API for a desktop
   XInput search sequence. Leave qualified paths to the normal loader. */
static HMODULE LoadHostedXInput(LPCWSTR name) {
    if (!name) return NULL;
    if (CompareStringOrdinal(name, -1, L"xinput9_1_0.dll", -1, TRUE) != CSTR_EQUAL) {
        if (CompareStringOrdinal(name, -1, L"xinput1_4.dll", -1, TRUE) != CSTR_EQUAL &&
                CompareStringOrdinal(name, -1, L"xinput1_3.dll", -1, TRUE) != CSTR_EQUAL) return NULL;
        WCHAR enabled[2];
        if (GetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_XINPUT", enabled, ARRAYSIZE(enabled)) != 1 ||
                enabled[0] != L'1') return NULL;
    }
    if (GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) <= 1) return NULL;
    return LoadPackagedLibrary(L"XboxWin32XInput910.dll", 0);
}
