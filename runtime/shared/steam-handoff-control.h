#pragma once

#include <windows.h>

enum {
    STEAM_HANDOFF_REPORT = 0x6f000000,
    STEAM_HANDOFF_MODULE = 1u << 16,
    STEAM_HANDOFF_EXPORTS = 1u << 17,
    STEAM_HANDOFF_PIPE = 1u << 18,
    STEAM_HANDOFF_USER = 1u << 19,
    STEAM_HANDOFF_LOGGED_ON = 1u << 20,
    STEAM_HANDOFF_RELEASED = 1u << 21,
    STEAM_HANDOFF_SDK_REPORT = 0x70000000,
    STEAM_HANDOFF_SDK_HIVE = 1u << 16,
    STEAM_HANDOFF_SDK_DISCOVERY = 1u << 17,
    STEAM_HANDOFF_SDK_MODULE = 1u << 18,
    STEAM_HANDOFF_SDK_INITIALIZED = 1u << 19,
    STEAM_HANDOFF_SDK_LOGGED_ON = 1u << 20,
    STEAM_HANDOFF_SDK_MATCHED = 1u << 21,
    STEAM_HANDOFF_SDK_INIT_CALLED = 1u << 22,
    STEAM_HANDOFF_SDK_MAPPED = 1u << 23,
};

#ifdef __cplusplus
extern "C" {
#endif
DWORD SteamHandoffDiscovery(void);
DWORD WINAPI XboxSteamInitialize(BOOL retain);
#ifdef __cplusplus
}
#endif
