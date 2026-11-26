#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XBOX_DESKTOP_DC ((HDC)(ULONG_PTR)0x584244000001ULL)
#define XBOX_DISPLAY_DC ((HDC)(ULONG_PTR)0x584244000002ULL)
#define XBOX_WINDOW_DPI 96

/* Private HWND and HDC values belong to this backend. */
typedef struct XboxWindow {
    HWND handle;
    WNDPROC procedure;
    HINSTANCE instance;
    RECT bounds;
    RECT normalBounds;
    DWORD style;
    DWORD extendedStyle;
    LONG_PTR userData;
    ATOM classAtom;
    UINT contexts;
    UINT positionChange;
    int pixelFormat;
    BOOL destroying;
    HICON smallIcon;
    HICON largeIcon;
    BOOL restoreMaximized;
    BOOL messageOnly;
    struct WindowProperty *properties;
} XboxWindow;

BOOL XboxWindowThread(void);
XboxWindow *XboxFindWindow(HWND handle);
BOOL WINAPI XboxDestroyWindow(HWND handle);
BOOL WINAPI XboxPostMessageW(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
BOOL WINAPI XboxSetWindowPos(HWND handle, HWND insertAfter, int x, int y, int width, int height, UINT flags);
void XboxPollWglHostEvents(void);
/* Hosted SDL input as desktop window messages. usage is a USB HID keyboard usage (SDL scancode). */
void XboxPostHostedKey(HWND window, UINT usage, BOOL down, BOOL repeat);
void XboxPostHostedText(HWND window, const char *utf8);
void XboxPostHostedPointer(HWND window, UINT message, LONG x, LONG y, int wheel);
BOOL XboxWglHostEventsActive(void);
void XboxResizeCurrentContext(XboxWindow *window);
RECT XboxDesktopBounds(void);
void XboxWindowTrace(const char *line);
void XboxWatchHostExit(void);
