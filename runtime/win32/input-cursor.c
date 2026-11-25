#define WIN32_LEAN_AND_MEAN
#include <windows.h>

DECLSPEC_IMPORT HANDLE WINAPI CreateFile2FromAppW(LPCWSTR, DWORD, DWORD, DWORD,
    LPCREATEFILE2_EXTENDED_PARAMETERS);
DECLSPEC_IMPORT BOOL WINAPI DeleteFileFromAppW(LPCWSTR);

typedef void (WINAPI *Procedure)(void);
extern Procedure XboxUser32Export(const char *name);

static LONG cursorFileNumber;

static DWORD ReadLE32(const BYTE *bytes) {
    return (DWORD)bytes[0] | (DWORD)bytes[1] << 8 | (DWORD)bytes[2] << 16 | (DWORD)bytes[3] << 24;
}

static WCHAR *AppendHex(WCHAR *end, ULONGLONG value, int digits) {
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        *end++ = L"0123456789abcdef"[(value >> shift) & 15];
    return end;
}

static BOOL TemporaryCursorPath(WCHAR path[MAX_PATH]) {
    static const WCHAR prefix[] = L"gabebox-ani-";
    static const WCHAR suffix[] = L".ani";
    const DWORD length = GetTempPathW(MAX_PATH, path);
    if (!length) return FALSE;
    if (length > MAX_PATH - (ARRAYSIZE(prefix) - 1 + 32 + ARRAYSIZE(suffix))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    WCHAR *end = path + length;
    for (const WCHAR *part = prefix; *part;) *end++ = *part++;
    end = AppendHex(end, GetCurrentProcessId(), 8);
    end = AppendHex(end, GetTickCount64(), 16);
    end = AppendHex(end, (DWORD)InterlockedIncrement(&cursorFileNumber), 8);
    for (const WCHAR *part = suffix; (*end++ = *part++);) {}
    return TRUE;
}

HICON WINAPI XboxCreateIconFromResourceEx(PBYTE resource, DWORD size, BOOL icon, DWORD version,
        int width, int height, UINT flags) {
    typedef HICON (WINAPI *CreateIcon)(PBYTE, DWORD, BOOL, DWORD, int, int, UINT);
    typedef HANDLE (WINAPI *LoadImage)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
    const DWORD previous = GetLastError();
    const CreateIcon native = (CreateIcon)XboxUser32Export("CreateIconFromResourceEx");
    if (native) {
        SetLastError(previous);
        return native(resource, size, icon, version, width, height, flags);
    }

    // The file fallback supplies an independently owned ANI cursor. Other resource
    // types and sharing flags require the native CreateIconFromResourceEx implementation.
    if (icon || version != 0x00030000 || flags || size > 16 * 1024 * 1024) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (!resource || size < 12 || (ULONG_PTR)resource % sizeof(DWORD) || width < 0 || height < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (ReadLE32(resource) != 0x46464952 /* RIFF */ || ReadLE32(resource + 8) != 0x4e4f4341 /* ACON */) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    const DWORD payload = ReadLE32(resource + 4);
    if (payload < 4 || (payload & 1) || payload != size - 8) {
        SetLastError(ERROR_INVALID_DATA);
        return NULL;
    }

    const LoadImage load = (LoadImage)XboxUser32Export("LoadImageW");
    if (!load) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    WCHAR path[MAX_PATH];
    if (!TemporaryCursorPath(path)) return NULL;
    const HANDLE file = CreateFile2FromAppW(path, GENERIC_WRITE, 0, CREATE_NEW, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    DWORD written = 0;
    const BOOL stored = WriteFile(file, resource, size, &written, NULL);
    DWORD error = stored ? ERROR_WRITE_FAULT : GetLastError();
    const BOOL closed = CloseHandle(file);

    HICON result = NULL;
    if (stored && written == size) {
        if (closed) {
            SetLastError(previous);
            result = (HICON)load(NULL, path, IMAGE_CURSOR, width, height, LR_LOADFROMFILE);
        }
        error = GetLastError();
    }
    DeleteFileFromAppW(path);
    SetLastError(error);
    return result;
}
