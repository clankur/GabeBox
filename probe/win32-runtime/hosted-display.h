#pragma once

static const WCHAR hostedDisplayName[] = L"\\\\.\\DISPLAY1";
static const WCHAR hostedMonitorName[] = L"Xbox monitor";
enum { hostedRefreshRate = 60 };

static inline void CopyDisplayText(WCHAR *to, UINT length, const WCHAR *text) {
    UINT i = 0;
    for (; i + 1 < length && text[i]; ++i) to[i] = text[i];
    for (; i < length; ++i) to[i] = 0;
}

static inline LONG ReadDisplayDimension(LPCWSTR name, LONG fallback) {
    WCHAR text[8];
    DWORD length = GetEnvironmentVariableW(name, text, ARRAYSIZE(text));
    if (!length || length >= ARRAYSIZE(text)) return fallback;
    LONG result = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9' || result > 4096) return fallback;
        result = result * 10 + (text[i] - L'0');
    }
    return result >= 64 && result <= 4096 ? result : fallback;
}

static inline RECT HostedDesktopBounds(void) {
    return (RECT){0, 0, ReadDisplayDimension(L"XBOX_SDL_RENDER_WIDTH", 1920),
        ReadDisplayDimension(L"XBOX_SDL_RENDER_HEIGHT", 1080)};
}
