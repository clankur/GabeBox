#pragma once
#include <windows.h>

/* This desktop library is requested by games that only need the basic XInput API.
   Keep explicit paths and other XInput versions within the normal loader. */
static HMODULE LoadHostedXInput(LPCWSTR name) {
    if (!name || CompareStringOrdinal(name, -1, L"xinput9_1_0.dll", -1, TRUE) != CSTR_EQUAL ||
            GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", NULL, 0) <= 1) return NULL;
    return LoadPackagedLibrary(L"XboxWin32XInput910.dll", 0);
}
