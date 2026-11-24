#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"
#include "../../runtime/shared/sdl-host-protocol.h"

DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

static MSG delivered[12];
static UINT deliveredCount;

static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM key, LPARAM flags) {
    if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR) && deliveredCount < ARRAYSIZE(delivered))
        delivered[deliveredCount++] = (MSG){window, message, key, flags, 0, {0, 0}};
    return DefWindowProcW(window, message, key, flags);
}

static void Publish(SdlHostKeyboard *keyboard, SdlHostKey entry) {
    const UINT32 write = (UINT32)SdlHostLoad(&keyboard->write);
    keyboard->entries[write % SDL_HOST_KEY_QUEUE_SIZE] = entry;
    InterlockedExchange(&keyboard->write, (LONG)(write + 1));
}

static BOOL Take(HWND window, UINT message, WPARAM key, UINT flags) {
    MSG input;
    if (!PeekMessageW(&input, window, 0, 0, PM_REMOVE) || input.message != message ||
            input.wParam != key || (UINT)input.lParam != flags || TranslateMessage(&input)) return FALSE;
    const UINT before = deliveredCount;
    DispatchMessageW(&input);
    return deliveredCount == before + 1 && delivered[before].message == message && delivered[before].wParam == key;
}

static LONG PublishPointer(SdlHostMailbox *mailbox, LONG pointer) {
    InterlockedIncrement(&mailbox->input_serial);
    InterlockedExchange(&mailbox->input_pointer, pointer);
    return InterlockedIncrement(&mailbox->input_serial);
}

static unsigned VerifyKeyboard(SdlHostMailbox *mailbox) {
    SdlHostKeyboard *keyboard = &mailbox->keyboard;
    const HINSTANCE module = GetModuleHandleW(NULL);
    const WNDCLASSW cls = {0, OnMessage, 0, 0, module, NULL, NULL, NULL, NULL, L"HostedKeyboardControl"};
    if (!RegisterClassW(&cls)) return 0x75510000u;
    const HWND window = CreateWindowExW(0, cls.lpszClassName, L"Keyboard", WS_OVERLAPPEDWINDOW,
        10, 10, 320, 240, NULL, NULL, module, NULL);
    if (!window) return 0x75510001u;
    MSG pending;
    if (PeekMessageW(&pending, window, 0, 0, PM_NOREMOVE) || SdlHostLoad(&keyboard->available)) return 0x75520000u;
    ShowWindow(window, SW_SHOW);
    if (PeekMessageW(&pending, window, 0, 0, PM_NOREMOVE) || !SdlHostLoad(&keyboard->available)) return 0x75520001u;

    const UINT key = MapVirtualKeyW(0x1e, MAPVK_VSC_TO_VK_EX);
    if (!key || key > 255) return 0x75530000u;
    Publish(keyboard, (SdlHostKey){225, 1, {0}});
    Publish(keyboard, (SdlHostKey){4, 1, {0}});
    Publish(keyboard, (SdlHostKey){0, 0, "\xc3\xa9\xf0\x9f\x99\x82"});
    Publish(keyboard, (SdlHostKey){4, 0, {0}});
    if (!PeekMessageW(&pending, window, 0, 0, PM_NOREMOVE) || pending.message != WM_KEYDOWN ||
            GetKeyState(VK_SHIFT) || GetKeyState(VK_LSHIFT)) return 0x75540000u;
    if (!Take(window, WM_KEYDOWN, VK_SHIFT, 0x002a0001) ||
            GetKeyState(VK_SHIFT) >= 0 || GetKeyState(VK_LSHIFT) >= 0 || GetKeyState(VK_RSHIFT)) return 0x75540001u;
    BYTE states[256];
    if (!Take(window, WM_KEYDOWN, key, 0x001e0001) || !GetKeyboardState(states) ||
            states[key] != 0x80 || states[VK_SHIFT] != 0x80) return 0x75540002u;
    if (!Take(window, WM_CHAR, 0x00e9, 1) || !Take(window, WM_CHAR, 0xd83d, 1) ||
            !Take(window, WM_CHAR, 0xde42, 1) || !Take(window, WM_KEYUP, key, 0xc01e0001) ||
            GetKeyState(key) || GetKeyState(VK_SHIFT) >= 0) return 0x75550000u;

    Publish(keyboard, (SdlHostKey){228, 1, {0}});
    if (!Take(window, WM_KEYDOWN, VK_CONTROL, 0x011d0001) || GetKeyState(VK_RCONTROL) >= 0)
        return 0x75580000u;
    const LONG press = PublishPointer(mailbox, SDL_HOST_POINTER_LEFT | 1536 | (1536 << 12));
    const WPARAM pressed = MK_LBUTTON | MK_SHIFT | MK_CONTROL;
    if (!PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || pending.message != WM_MOUSEMOVE ||
            pending.wParam != pressed || !PeekMessageW(&pending, window, 0, 0, PM_REMOVE) ||
            pending.message != WM_LBUTTONDOWN || pending.wParam != pressed ||
            SdlHostLoad(&mailbox->input_ack) != press ||
            (USHORT)GetAsyncKeyState(VK_LBUTTON) != 0x8001 ||
            (USHORT)GetAsyncKeyState(VK_LBUTTON) != 0x8000) return 0x75580001u;
    const LONG dragPosition = 2496 | (2112 << 12);
    const LONG drag = PublishPointer(mailbox, SDL_HOST_POINTER_LEFT | dragPosition);
    if (!PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || pending.message != WM_MOUSEMOVE ||
            pending.wParam != pressed || !LOWORD(pending.lParam) || !HIWORD(pending.lParam) ||
            SdlHostLoad(&mailbox->input_ack) != drag ||
            (USHORT)GetAsyncKeyState(VK_LBUTTON) != 0x8000) return 0x75580004u;
    Publish(keyboard, (SdlHostKey){228, 0, {0}});
    if (!Take(window, WM_KEYUP, VK_CONTROL, 0xc11d0001) || GetKeyState(VK_CONTROL)) return 0x75580002u;
    const LONG release = PublishPointer(mailbox, dragPosition);
    if (!PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || pending.message != WM_LBUTTONUP ||
            pending.wParam != MK_SHIFT || SdlHostLoad(&mailbox->input_ack) != release ||
            GetAsyncKeyState(VK_LBUTTON) || PeekMessageW(&pending, window, 0, 0, PM_REMOVE)) return 0x75580003u;

    Publish(keyboard, (SdlHostKey){0, 0, "discard"});
    if (!PeekMessageW(&pending, window, 0, 0, PM_NOREMOVE) || pending.message != WM_CHAR) return 0x75560000u;
    InterlockedExchange(&keyboard->reset, SdlHostLoad(&keyboard->write));
    Publish(keyboard, (SdlHostKey){0, 0, "\xc0\x80"});
    Publish(keyboard, (SdlHostKey){0, 0, "R"});
    if (!Take(window, WM_KEYUP, VK_SHIFT, 0xc02a0001) || GetKeyState(VK_SHIFT) || !Take(window, WM_CHAR, 'R', 1) ||
            PeekMessageW(&pending, window, 0, 0, PM_REMOVE) ||
            SdlHostLoad(&keyboard->read) != SdlHostLoad(&keyboard->write)) return 0x75560001u;

    Publish(keyboard, (SdlHostKey){0, 0, "discard"});
    ShowWindow(window, SW_HIDE);
    if (PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || SdlHostLoad(&keyboard->available)) return 0x75570000u;
    ShowWindow(window, SW_SHOW);
    if (PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || !SdlHostLoad(&keyboard->available)) return 0x75570001u;
    Publish(keyboard, (SdlHostKey){0, 0, "discard"});
    ShowWindow(window, SW_HIDE);
    ShowWindow(window, SW_SHOW);
    if (PeekMessageW(&pending, window, 0, 0, PM_REMOVE) || !SdlHostLoad(&keyboard->available) ||
            !DestroyWindow(window) || !UnregisterClassW(cls.lpszClassName, module)) return 0x75570002u;
    return PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned RunControl(void) {
    WCHAR path[512];
    DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    if (!length || length >= ARRAYSIZE(path) - 48) return 0x75500000u;
    const WCHAR prefix[] = L"xbox-window-keyboard-";
    for (UINT i = 0; prefix[i]; ++i) path[length++] = prefix[i];
    const DWORD token = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4) path[length++] = L"0123456789abcdef"[(token >> shift) & 15];
    path[length] = 0;
    CREATEFILE2_EXTENDED_PARAMETERS options = {0};
    options.dwSize = sizeof(options);
    options.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_NEW, &options);
    if (file == INVALID_HANDLE_VALUE) return 0x75500001u;
    const HANDLE mapping = CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, sizeof(SdlHostMailbox), NULL);
    SdlHostMailbox *mailbox = mapping ? MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(*mailbox)) : NULL;
    unsigned result = 0x75500002u;
    if (mailbox && SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_DXGI", L"1") &&
            SetEnvironmentVariableW(L"XBOX_WIN32_HOSTED_KEYBOARD", L"1") &&
            SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path)) {
        InterlockedExchange(&mailbox->magic, SDL_HOST_MAGIC);
        result = VerifyKeyboard(mailbox);
    }
    if (mailbox) UnmapViewOfFile(mailbox);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    return result;
}

void WINAPI ChildEntry(void) {
    ExitProcess(RunControl());
}
