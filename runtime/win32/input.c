/* Translate desktop input calls that are absent on Xbox Developer Mode. */
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define MAPVK_VK_TO_CHAR 2u
#define DEAD_KEY 0x80000000u
#define LOCALE_IDEFAULTANSICODEPAGE 0x1004u
#define LOCALE_RETURN_NUMBER 0x20000000u
#define WC_NO_BEST_FIT_CHARS 0x400u
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u
#define ERROR_NOT_SUPPORTED 50u
#define IMAGE_CURSOR 2u
#define LR_LOADFROMFILE 0x10u
#define MAX_IME_FILENAME 260u

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Module;
typedef void (WINAPI *Procedure)(void);
typedef unsigned int (WINAPI *GetWideImeFilename)(void *, WCHAR *, unsigned int);

IMPORT Module WINAPI LoadLibraryExW(const WCHAR *name, void *file, DWORD flags);
IMPORT Module WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT Procedure WINAPI GetProcAddress(Module module, const char *name);
IMPORT int WINAPI FreeLibrary(Module module);
IMPORT int WINAPI WideCharToMultiByte(unsigned int codePage, DWORD flags, const WCHAR *wide, int wideCount,
    char *encoded, int size, const char *substitute, int *usedDefault);
IMPORT int WINAPI GetLocaleInfoW(DWORD locale, DWORD field, WCHAR *result, int capacity);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI SetLastError(DWORD error);

Procedure XboxUser32Export(const char *name) {
    Module user = GetModuleHandleW(L"user32.dll");
    if (!user) user = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return user ? GetProcAddress(user, name) : 0;
}

static unsigned int ImeFilenameFromWide(GetWideImeFilename wide, void *layout, char *buffer, unsigned int size) {
    WCHAR filename[MAX_IME_FILENAME];
    const unsigned int length = wide(layout, 0, 0);
    if (!length || length >= MAX_IME_FILENAME || wide(layout, filename, length + 1) != length) return 0;
    const int bytes = WideCharToMultiByte(0, 0, filename, (int)length, 0, 0, 0, 0);
    if (bytes <= 0) return 0;
    if (!size) return (unsigned int)bytes;
    if (!buffer) return 0;
    if ((unsigned int)bytes >= size || WideCharToMultiByte(0, 0, filename, (int)length,
            buffer, bytes, 0, 0) != bytes) {
        buffer[0] = 0;
        return 0;
    }
    buffer[bytes] = 0;
    return (unsigned int)bytes;
}

unsigned int WINAPI XboxImmGetIMEFileNameA(void *layout, char *buffer, unsigned int size) {
    typedef unsigned int (WINAPI *GetAnsiImeFilename)(void *, char *, unsigned int);
    const DWORD previous = GetLastError();
    Module imm = LoadLibraryExW(L"imm32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!imm) {
        SetLastError(previous);
        return 0;
    }
    const GetAnsiImeFilename ansi = (GetAnsiImeFilename)GetProcAddress(imm, "ImmGetIMEFileNameA");
    unsigned int result = 0;
    DWORD error = previous;
    if (ansi) {
        SetLastError(previous);
        result = ansi(layout, buffer, size);
        error = GetLastError();
    } else {
        const GetWideImeFilename wide = (GetWideImeFilename)GetProcAddress(imm, "ImmGetIMEFileNameW");
        if (wide) result = ImeFilenameFromWide(wide, layout, buffer, size);
    }
    FreeLibrary(imm);
    SetLastError(error);
    return result;
}

void *WINAPI XboxLoadCursorFromFileW(const WCHAR *path) {
    typedef void *(WINAPI *LoadCursor)(const WCHAR *);
    typedef void *(WINAPI *LoadImage)(Module, const WCHAR *, unsigned int, int, int, unsigned int);
    LoadCursor native = (LoadCursor)XboxUser32Export("LoadCursorFromFileW");
    if (native) return native(path);
    LoadImage loadImage = (LoadImage)XboxUser32Export("LoadImageW");
    if (loadImage) return loadImage(0, path, IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE);
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

int WINAPI XboxGetPointerPenInfo(unsigned int pointerId, void *penInfo) {
    typedef int (WINAPI *GetPenInfo)(unsigned int, void *);
    GetPenInfo native = (GetPenInfo)XboxUser32Export("GetPointerPenInfo");
    if (native) return native(pointerId, penInfo);
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

unsigned int WINAPI XboxMapVirtualKeyExA(unsigned int code, unsigned int mapType, void *layout) {
    typedef unsigned int (WINAPI *MapKey)(unsigned int, unsigned int, void *);
    MapKey ansi = (MapKey)XboxUser32Export("MapVirtualKeyExA");
    if (ansi) return ansi(code, mapType, layout);
    MapKey wide = (MapKey)XboxUser32Export("MapVirtualKeyExW");
    if (!wide) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    unsigned int mapped = wide(code, mapType, layout);
    WCHAR character = (WCHAR)mapped;
    if (mapType != MAPVK_VK_TO_CHAR || character <= 0x7f) return mapped;

    DWORD codePage = 0;
    if (layout) {
        DWORD language = (DWORD)((unsigned long long)layout & 0xffffu);
        if (!GetLocaleInfoW(language, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                (WCHAR *)&codePage, sizeof(codePage) / sizeof(WCHAR))) return 0;
    }
    unsigned char encoded[2];
    int substituted = 0;
    const int size = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, &character, 1,
        (char *)encoded, sizeof(encoded), 0, &substituted);
    if (!size || substituted) return 0;
    const unsigned int ansiCode = size == 1 ? encoded[0] : ((unsigned int)encoded[0] << 8) | encoded[1];
    return (mapped & DEAD_KEY) | ansiCode;
}
