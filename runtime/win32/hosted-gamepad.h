#pragma once
#include <stddef.h>
#include "loader-trace-output.h"
#include "../shared/sdl-host-protocol.h"

static inline SdlHostMailbox *MapHostedGamepad(LPCWSTR path) {
    HANDLE file = CreateFile2FromAppW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    const SIZE_T length = offsetof(SdlHostMailbox, keyboard);
    LARGE_INTEGER size;
    const BOOL valid = GetFileSizeEx(file, &size) && size.QuadPart >= (LONGLONG)length;
    HANDLE mapping = valid ? CreateFileMappingFromApp(file, NULL, PAGE_READWRITE, 0, NULL) : NULL;
    CloseHandle(file);
    if (!mapping) return NULL;
    SdlHostMailbox *mailbox = MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, length);
    CloseHandle(mapping);
    if (mailbox && SdlHostLoad(&mailbox->magic) != SDL_HOST_MAGIC) {
        UnmapViewOfFile(mailbox);
        return NULL;
    }
    return mailbox;
}
