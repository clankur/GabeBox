#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef void (WINAPI *Procedure)(void);
extern Procedure XboxUser32Export(const char *name);

UINT WINAPI XboxGetRawInputBuffer(PRAWINPUT buffer, PUINT size, UINT headerSize) {
    typedef UINT (WINAPI *ReadBuffer)(PRAWINPUT, PUINT, UINT);
    typedef UINT (WINAPI *ReadRecord)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    typedef BOOL (WINAPI *Peek)(LPMSG, HWND, UINT, UINT, UINT);
    typedef LRESULT (WINAPI *FinishMessage)(HWND, UINT, WPARAM, LPARAM);
    typedef void (WINAPI *PostQuit)(int);

    const DWORD previous = GetLastError();
    const ReadBuffer native = (ReadBuffer)XboxUser32Export("GetRawInputBuffer");
    if (native) {
        SetLastError(previous);
        return native(buffer, size, headerSize);
    }
    if (!size || headerSize != sizeof(RAWINPUTHEADER) || (ULONG_PTR)buffer % sizeof(DWORD)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (UINT)-1;
    }
    const ReadRecord read = (ReadRecord)XboxUser32Export("GetRawInputData");
    Peek peek = (Peek)XboxUser32Export("PeekMessageW");
    if (!peek) peek = (Peek)XboxUser32Export("PeekMessageA");
    const FinishMessage finish = (FinishMessage)XboxUser32Export("DefWindowProcW");
    const PostQuit postQuit = (PostQuit)XboxUser32Export("PostQuitMessage");
    if (!read || !peek || !finish || !postQuit) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return (UINT)-1;
    }

    PRAWINPUT next = buffer;
    UINT remaining = buffer ? *size : 0;
    UINT count = 0;
    MSG message;
    while (peek(&message, NULL, WM_INPUT, WM_INPUT, PM_NOREMOVE | PM_QS_INPUT) && message.message == WM_INPUT) {
        const HRAWINPUT input = (HRAWINPUT)message.lParam;
        UINT needed = 0;
        if (read(input, RID_INPUT, NULL, &needed, headerSize) == (UINT)-1) return count ? count : (UINT)-1;
        if (needed < headerSize) {
            SetLastError(ERROR_INVALID_DATA);
            return count ? count : (UINT)-1;
        }
        if (!buffer) {
            *size = needed;
            SetLastError(previous);
            return 0;
        }
        if (needed > remaining) {
            if (count) break;
            *size = needed;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return (UINT)-1;
        }
        UINT capacity = remaining;
        const UINT copied = read(input, RID_INPUT, next, &capacity, headerSize);
        if (copied == (UINT)-1) return count ? count : (UINT)-1;
        const DWORD *recordSize = (const DWORD *)((const BYTE *)next + FIELD_OFFSET(RAWINPUTHEADER, dwSize));
        if (copied < headerSize || copied > remaining || *recordSize != copied) {
            SetLastError(ERROR_INVALID_DATA);
            return count ? count : (UINT)-1;
        }
        MSG removed;
        if (!peek(&removed, NULL, WM_INPUT, WM_INPUT, PM_REMOVE | PM_QS_INPUT)) break;
        if (removed.message == WM_QUIT) {
            postQuit((int)removed.wParam);
            break;
        }
        if (removed.message != WM_INPUT) break;
        if (GET_RAWINPUT_CODE_WPARAM(removed.wParam) == RIM_INPUT)
            finish(removed.hwnd, removed.message, removed.wParam, removed.lParam);
        if (removed.lParam != message.lParam) {
            SetLastError(ERROR_RETRY);
            return count ? count : (UINT)-1;
        }
        ++count;
        const ULONG_PTR end = (ULONG_PTR)next + copied;
        const ULONG_PTR mask = sizeof(ULONG_PTR) - 1;
        const ULONG_PTR following = (end + mask) & ~mask;
        const ULONG_PTR stride = following - (ULONG_PTR)next;
        if (stride >= remaining) break;
        remaining -= (UINT)stride;
        next = (PRAWINPUT)following;
    }
    if (!buffer) *size = 0;
    SetLastError(previous);
    return count;
}
