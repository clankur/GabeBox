#include "window.h"

/* GDI objects created against this backend's private device contexts.

   Hosted games receive XBOX_DESKTOP_DC, the display DC or a virtual window as
   their HDC, which the system GDI does not recognize. CreateDIBSection only
   consults the DC for DIB_PAL_COLORS, so a DIB_RGB_COLORS request is passed to
   the system with a null DC, as Windows allows. If the console's GDI still
   cannot create the section, a bounded 32-bit fallback keeps the completion
   contract the caller depends on: a nonnull bitmap handle and a zeroed,
   writable pixel buffer that stays valid until DeleteObject. Engines such as
   Godot write cursor and icon pixels through that pointer without checking it.
   Fallback handles are recognized only by DeleteObject here; other GDI calls
   given one fail as they would for any invalid handle. */

#define FALLBACK_SLOTS 32
#define FALLBACK_BASE 0x584247000100ULL
#define FALLBACK_MAX_BYTES (64u << 20)

typedef struct FallbackBitmap {
    void *pixels;
    SIZE_T size;
} FallbackBitmap;

static FallbackBitmap fallback[FALLBACK_SLOTS];
static SRWLOCK fallbackLock = SRWLOCK_INIT;
static volatile LONG tracedSystem, tracedFallback, tracedFailure;

static BOOL BackendDc(HDC dc) {
    return dc == XBOX_DESKTOP_DC || dc == XBOX_DISPLAY_DC || (dc && XboxFindWindow((HWND)dc));
}

static int FallbackIndex(HGDIOBJ object) {
    const ULONG_PTR value = (ULONG_PTR)object;
    if (value < FALLBACK_BASE || value >= FALLBACK_BASE + FALLBACK_SLOTS) return -1;
    return (int)(value - FALLBACK_BASE);
}

static SIZE_T FallbackSize(const BITMAPINFO *info) {
    const BITMAPINFOHEADER *header = &info->bmiHeader;
    if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biPlanes != 1 || header->biBitCount != 32 ||
            (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS) ||
            header->biWidth <= 0 || header->biHeight == 0 || header->biWidth > 16384 ||
            header->biHeight > 16384 || header->biHeight < -16384) return 0;
    const ULONGLONG rows = header->biHeight < 0 ? -(LONGLONG)header->biHeight : header->biHeight;
    const ULONGLONG size = (ULONGLONG)header->biWidth * 4u * rows;
    return size <= FALLBACK_MAX_BYTES ? (SIZE_T)size : 0;
}

HBITMAP WINAPI XboxCreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset) {
    if (bits) *bits = NULL;
    if (BackendDc(dc)) {
        if (usage != DIB_RGB_COLORS) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        dc = NULL;
    }
    HBITMAP bitmap = CreateDIBSection(dc, info, usage, bits, section, offset);
    if (bitmap) {
        if (!InterlockedExchange(&tracedSystem, 1)) XboxWindowTrace("win32_gdi_dib=system\n");
        return bitmap;
    }
    const DWORD error = GetLastError();
    const SIZE_T size = info && bits && !section && usage == DIB_RGB_COLORS ? FallbackSize(info) : 0;
    if (!size) {
        if (!InterlockedExchange(&tracedFailure, 1)) XboxWindowTrace("win32_gdi_dib=unsupported\n");
        SetLastError(error);
        return NULL;
    }
    void *pixels = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pixels) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    int slot = -1;
    AcquireSRWLockExclusive(&fallbackLock);
    for (int i = 0; i < FALLBACK_SLOTS; ++i) {
        if (!fallback[i].pixels) {
            fallback[i].pixels = pixels;
            fallback[i].size = size;
            slot = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&fallbackLock);
    if (slot < 0) {
        VirtualFree(pixels, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (!InterlockedExchange(&tracedFallback, 1)) XboxWindowTrace("win32_gdi_dib=backend\n");
    *bits = pixels;
    return (HBITMAP)(ULONG_PTR)(FALLBACK_BASE + (ULONGLONG)slot);
}

BOOL WINAPI XboxDeleteObject(HGDIOBJ object) {
    const int slot = FallbackIndex(object);
    if (slot < 0) return DeleteObject(object);
    AcquireSRWLockExclusive(&fallbackLock);
    void *pixels = fallback[slot].pixels;
    fallback[slot].pixels = NULL;
    fallback[slot].size = 0;
    ReleaseSRWLockExclusive(&fallbackLock);
    if (!pixels) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    VirtualFree(pixels, 0, MEM_RELEASE);
    return TRUE;
}
