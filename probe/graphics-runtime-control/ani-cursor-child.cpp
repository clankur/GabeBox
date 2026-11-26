#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../runtime/shared/child-process.h"
#include "ani-cursor-fixtures.h"

#ifndef PROBE_ANI_ADAPTER
extern "C" __declspec(dllimport) HANDLE WINAPI CreateFile2FromAppW(
    LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteFileFromAppW(LPCWSTR);
#endif

static UINT Failure(UINT stage, DWORD error = GetLastError()) {
    return 0x72000000u | (stage << 16) | (error & 0xffffu);
}

struct Module {
    HMODULE value;
    ~Module() { if (value) FreeLibrary(value); }

    template<class Proc> Proc Get(const char* name) const {
        return value ? reinterpret_cast<Proc>(reinterpret_cast<void*>(GetProcAddress(value, name))) : nullptr;
    }
};

using DestroyCursorProc = decltype(&DestroyCursor);

struct Cursor {
    DestroyCursorProc destroy;
    HCURSOR value = nullptr;
    ~Cursor() { if (value) destroy(value); }
};

#ifndef PROBE_ANI_ADAPTER
struct TemporaryCursorFile {
    WCHAR path[MAX_PATH];
    bool created = false;
    ~TemporaryCursorFile() { if (created) DeleteFileFromAppW(path); }

    DWORD Write(const BYTE* bytes, DWORD size) {
        const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
        if (!length) return GetLastError();
        if (length + 29 >= ARRAYSIZE(path)) return ERROR_FILENAME_EXCED_RANGE;
        WCHAR* end = path + length;
        for (auto prefix = L"gabebox-ani-"; *prefix;) *end++ = *prefix++;
        const auto token = static_cast<DWORD>(GetTickCount64()) ^ GetCurrentProcessId();
        for (int shift = 28; shift >= 0; shift -= 4) *end++ = L"0123456789abcdef"[(token >> shift) & 15];
        for (auto suffix = L".ani"; *suffix;) *end++ = *suffix++;
        *end = 0;
        const HANDLE file = CreateFile2FromAppW(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError();
        created = true;
        DWORD written = 0;
        const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
        DWORD error = ok ? (written == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT) : GetLastError();
        if (!CloseHandle(file) && error == ERROR_SUCCESS) error = GetLastError();
        return error;
    }

    bool Remove() {
        if (!DeleteFileFromAppW(path)) return false;
        created = false;
        return true;
    }
};
#endif

static UINT LoadCursorFromBytes(const Module& user, const BYTE* bytes, DWORD size, UINT stage, Cursor& cursor) {
#ifdef PROBE_ANI_ADAPTER
    (void)user;
    const auto loadCursor = [bytes, size] {
        return static_cast<HCURSOR>(CreateIconFromResourceEx(const_cast<BYTE*>(bytes), size, FALSE, 0x00030000,
            CURSOR_SIZE, CURSOR_SIZE, 0));
    };
#else
    const auto load = user.Get<decltype(&LoadImageW)>("LoadImageW");
    if (!load) return Failure(stage);
    TemporaryCursorFile file;
    const DWORD written = file.Write(bytes, size);
    if (written != ERROR_SUCCESS) return Failure(stage + 1, written);
    const auto loadCursor = [&] {
        return static_cast<HCURSOR>(load(nullptr, file.path, IMAGE_CURSOR, CURSOR_SIZE, CURSOR_SIZE, LR_LOADFROMFILE));
    };
#endif
    cursor.value = loadCursor();
    if (!cursor.value) return Failure(stage + 2);
    Cursor separate{cursor.destroy};
    separate.value = loadCursor();
    if (!separate.value) return Failure(stage + 3);
    if (separate.value == cursor.value) {
        separate.value = nullptr;
        return Failure(stage + 4, ERROR_INVALID_DATA);
    }
    if (!separate.destroy(separate.value)) return Failure(stage + 5);
    separate.value = nullptr;
#ifndef PROBE_ANI_ADAPTER
    if (!file.Remove()) return Failure(stage + 6);
#endif
    return PROBE_EXPECTED_CHILD_EXIT;
}

static UINT VerifyCursor(const Module& user, const Module& gdi, HCURSOR cursor, UINT frames, UINT stage) {
    const auto getInfo = user.Get<decltype(&GetIconInfo)>("GetIconInfo");
    const auto getObject = gdi.Get<decltype(&GetObjectW)>("GetObjectW");
    const auto deleteObject = gdi.Get<decltype(&DeleteObject)>("DeleteObject");
    if (!getInfo || !getObject || !deleteObject) return Failure(stage, ERROR_PROC_NOT_FOUND);
    ICONINFO info{};
    if (!getInfo(cursor, &info)) return Failure(stage + 1);
    BITMAP shape{};
    const bool expectedShape = !info.fIcon && info.xHotspot == HOTSPOT_X && info.yHotspot == HOTSPOT_Y &&
        info.hbmColor && getObject(info.hbmColor, sizeof(shape), &shape) == sizeof(shape) &&
        shape.bmWidth == CURSOR_SIZE && shape.bmHeight == CURSOR_SIZE;
    if (info.hbmMask) deleteObject(info.hbmMask);
    if (info.hbmColor) deleteObject(info.hbmColor);
    if (!expectedShape) return Failure(stage + 2, ERROR_INVALID_DATA);

    const auto createDC = gdi.Get<decltype(&CreateCompatibleDC)>("CreateCompatibleDC");
    const auto deleteDC = gdi.Get<decltype(&DeleteDC)>("DeleteDC");
    const auto createDib = gdi.Get<decltype(&CreateDIBSection)>("CreateDIBSection");
    const auto selectObject = gdi.Get<decltype(&SelectObject)>("SelectObject");
    const auto flush = gdi.Get<decltype(&GdiFlush)>("GdiFlush");
    const auto draw = user.Get<decltype(&DrawIconEx)>("DrawIconEx");
    if (!createDC || !deleteDC || !createDib || !selectObject || !flush || !draw)
        return Failure(stage + 3, ERROR_PROC_NOT_FOUND);
    const HDC dc = createDC(nullptr);
    if (!dc) return Failure(stage + 4);
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = CURSOR_SIZE;
    dib.bmiHeader.biHeight = -CURSOR_SIZE;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const HBITMAP bitmap = createDib(dc, &dib, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        const UINT failure = Failure(stage + 5);
        if (bitmap) deleteObject(bitmap);
        deleteDC(dc);
        return failure;
    }
    const HGDIOBJ oldBitmap = selectObject(dc, bitmap);
    UINT result = PROBE_EXPECTED_CHILD_EXIT;
    if (!oldBitmap || oldBitmap == HGDI_ERROR) {
        result = Failure(stage + 6);
    } else {
        for (UINT frame = 0; frame < frames; ++frame) {
            if (!draw(dc, 0, 0, cursor, CURSOR_SIZE, CURSOR_SIZE, frame, nullptr, DI_IMAGE) || !flush()) {
                result = Failure(stage + 7);
                break;
            }
            const DWORD expected = frame == 0 ? 0x00ff0000 : 0x000000ff;
            const auto* color = static_cast<const DWORD*>(pixels);
            for (UINT i = 0; i < CURSOR_SIZE * CURSOR_SIZE; ++i) {
                if ((color[i] & 0x00ffffff) != expected) {
                    result = Failure(stage + 8, ERROR_INVALID_DATA);
                    break;
                }
            }
            if (result != PROBE_EXPECTED_CHILD_EXIT) break;
        }
        selectObject(dc, oldBitmap);
    }
    deleteObject(bitmap);
    deleteDC(dc);
    return result;
}

static UINT VerifyAnimation() {
    const Module user{LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!user.value) return Failure(1);
    const auto destroy = user.Get<DestroyCursorProc>("DestroyCursor");
    if (!destroy) return Failure(2);
    Cursor single{destroy};
    UINT result = LoadCursorFromBytes(user, singleFrameCursor, sizeof(singleFrameCursor), 3, single);
    if (result != PROBE_EXPECTED_CHILD_EXIT) return result;
    Cursor animated{destroy};
    result = LoadCursorFromBytes(user, animatedCursor, sizeof(animatedCursor), 10, animated);
    if (result != PROBE_EXPECTED_CHILD_EXIT) return result;
    const Module gdi{LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!gdi.value) return Failure(17);
    result = VerifyCursor(user, gdi, single.value, 1, 18);
    if (result != PROBE_EXPECTED_CHILD_EXIT) return result;
    return VerifyCursor(user, gdi, animated.value, 2, 27);
}

extern "C" void WINAPI ChildEntry() {
    ExitProcess(VerifyAnimation());
}
