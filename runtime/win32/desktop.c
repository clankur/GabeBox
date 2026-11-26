/* ABI adapters for legacy desktop APIs. */
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u
#define SND_MEMORY 0x4u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_OUTOFMEMORY 14u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_DIRECTORY 267u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define PATH_CAPACITY 260u
#define CSIDL_PERSONAL 0x0005
#define CSIDL_APPDATA 0x001a
#define CSIDL_FLAG_NO_ALIAS 0x1000
#define CSIDL_FLAG_DONT_VERIFY 0x4000
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0u
#define SHGFP_TYPE_DEFAULT 1u
#define KF_FLAG_DONT_VERIFY 0x4000u
#define MAX_ENVIRONMENT_VALUE 32768u

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Module;
typedef void (WINAPI *Procedure)(void);
typedef struct {
    DWORD data1;
    unsigned short data2, data3;
    unsigned char data4[8];
} FolderId;

IMPORT Module WINAPI LoadLibraryExW(const WCHAR *name, void *file, DWORD flags);
IMPORT Module WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT Procedure WINAPI GetProcAddress(Module module, const char *name);
IMPORT void *WINAPI GetProcessHeap(void);
IMPORT void *WINAPI HeapAlloc(void *heap, DWORD flags, unsigned long long size);
IMPORT int WINAPI HeapFree(void *heap, DWORD flags, void *memory);
IMPORT int WINAPI MultiByteToWideChar(unsigned int page, DWORD flags, const char *text, int length,
    WCHAR *output, int capacity);
IMPORT void WINAPI SetLastError(DWORD error);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *value, DWORD capacity);
IMPORT DWORD WINAPI GetFileAttributesW(const WCHAR *path);
IMPORT int WINAPI CreateDirectoryW(const WCHAR *path, void *security);
IMPORT void *WINAPI CoTaskMemAlloc(unsigned long long size);
IMPORT void WINAPI CoTaskMemFree(void *memory);

static Procedure SystemExport(const WCHAR *dll, const char *name) {
    Module module = GetModuleHandleW(dll);
    if (!module) module = LoadLibraryExW(dll, 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : 0;
}

static long PathError(DWORD error) {
    return (long)(0x80070000u | (error & 0xffffu));
}

long WINAPI XboxEnableThemeDialogTexture(void *window, DWORD flags) {
    typedef long (WINAPI *EnableTexture)(void *, DWORD);
    const DWORD previousError = GetLastError();
    const EnableTexture enable = (EnableTexture)SystemExport(L"uxtheme.dll", "EnableThemeDialogTexture");
    SetLastError(previousError);
    // The Xbox runtime has no themed dialog background provider.
    return enable ? enable(window, flags) : (long)0x80004001u; // E_NOTIMPL
}

static long EnsureProfileDirectory(const WCHAR *path, int folder) {
    if (folder & CSIDL_FLAG_DONT_VERIFY) return 0;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES && (folder & CSIDL_FLAG_CREATE)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            if (!CreateDirectoryW(path, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
                return PathError(GetLastError());
            attributes = GetFileAttributesW(path);
        }
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) return PathError(GetLastError());
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return PathError(ERROR_ACCESS_DENIED);
    return attributes & FILE_ATTRIBUTE_DIRECTORY ? 0 : PathError(ERROR_DIRECTORY);
}

long WINAPI XboxSHGetFolderPathW(void *window, int folder, void *token, DWORD mode, WCHAR *output) {
    typedef long (WINAPI *GetFolderPath)(void *, int, void *, DWORD, WCHAR *);
    if (!output) return PathError(ERROR_INVALID_PARAMETER);
    output[0] = 0;
    const DWORD previousError = GetLastError();
    const int modifiers = CSIDL_FLAG_CREATE | CSIDL_FLAG_DONT_VERIFY | CSIDL_FLAG_NO_ALIAS;
    const WCHAR *profilePath = 0;
    if (!token && (mode == SHGFP_TYPE_CURRENT || mode == SHGFP_TYPE_DEFAULT)) {
        switch (folder & ~modifiers) {
        case CSIDL_PERSONAL: profilePath = L"XBOX_WIN32_DOCUMENTS"; break;
        case CSIDL_APPDATA: profilePath = L"XBOX_WIN32_ROAMING_APPDATA"; break;
        }
    }
    if (profilePath) {
        WCHAR path[PATH_CAPACITY];
        const DWORD length = GetEnvironmentVariableW(profilePath, path, PATH_CAPACITY);
        if (length) {
            long result;
            if (length >= PATH_CAPACITY) result = PathError(ERROR_INSUFFICIENT_BUFFER);
            else if ((folder & (CSIDL_FLAG_CREATE | CSIDL_FLAG_DONT_VERIFY)) ==
                    (CSIDL_FLAG_CREATE | CSIDL_FLAG_DONT_VERIFY)) result = PathError(ERROR_INVALID_PARAMETER);
            else result = EnsureProfileDirectory(path, folder);
            if (!result) {
                for (DWORD i = 0; i <= length; ++i) output[i] = path[i];
            }
            SetLastError(previousError);
            return result;
        }
    }
    const GetFolderPath getFolderPath = (GetFolderPath)SystemExport(L"shell32.dll", "SHGetFolderPathW");
    SetLastError(previousError);
    return getFolderPath ? getFolderPath(window, folder, token, mode, output) : (long)0x80004001u;
}

static int IsLocalLow(const FolderId *id) {
    static const FolderId localLow = {0xa520a1a4u, 0x1780, 0x4ff6, {0xbd, 0x18, 0x16, 0x73, 0x43, 0xc5, 0xaf, 0x16}};
    if (!id || id->data1 != localLow.data1 || id->data2 != localLow.data2 || id->data3 != localLow.data3) return 0;
    for (unsigned i = 0; i < sizeof(localLow.data4); ++i) {
        if (id->data4[i] != localLow.data4[i]) return 0;
    }
    return 1;
}

static long CopyLocalLow(DWORD flags, DWORD capacity, WCHAR **output) {
    if (capacity > MAX_ENVIRONMENT_VALUE) return PathError(ERROR_INSUFFICIENT_BUFFER);
    WCHAR *path = CoTaskMemAlloc((unsigned long long)capacity * sizeof(WCHAR));
    if (!path) return PathError(ERROR_OUTOFMEMORY);
    const DWORD length = GetEnvironmentVariableW(L"XBOX_WIN32_LOCAL_LOW", path, capacity);
    long result = 0;
    if (!length) result = PathError(GetLastError());
    else if (length >= capacity) result = PathError(ERROR_INSUFFICIENT_BUFFER);
    else if (!(flags & KF_FLAG_DONT_VERIFY)) {
        const DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) result = PathError(GetLastError());
        else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) result = PathError(ERROR_ACCESS_DENIED);
        else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) result = PathError(ERROR_DIRECTORY);
    }
    if (result) CoTaskMemFree(path);
    else *output = path;
    return result;
}

// Unity resolves FOLDERID_LocalAppDataLow for its persistent data path. A profile can scope it per title.
long WINAPI XboxSHGetKnownFolderPath(const FolderId *id, DWORD flags, void *token, WCHAR **output) {
    typedef long (WINAPI *KnownFolder)(const FolderId *, DWORD, void *, WCHAR **);
    if (!output) return PathError(ERROR_INVALID_PARAMETER);
    *output = 0;
    const DWORD previousError = GetLastError();
    if (!token && !(flags & ~KF_FLAG_DONT_VERIFY) && IsLocalLow(id)) {
        const DWORD capacity = GetEnvironmentVariableW(L"XBOX_WIN32_LOCAL_LOW", 0, 0);
        if (capacity > 1) {
            const long result = CopyLocalLow(flags, capacity, output);
            SetLastError(previousError);
            return result;
        }
    }
    const KnownFolder native = (KnownFolder)SystemExport(L"shell32.dll", "SHGetKnownFolderPath");
    SetLastError(previousError);
    return native ? native(id, flags, token, output) : (long)0x80004001u;
}

static WCHAR *WideCopy(const char *text) {
    if (!text) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int characters = MultiByteToWideChar(0, 0, text, -1, 0, 0);
    if (!characters) return 0;
    WCHAR *wide = HeapAlloc(GetProcessHeap(), 0, (unsigned long long)characters * sizeof(WCHAR));
    if (!wide) {
        SetLastError(ERROR_OUTOFMEMORY);
        return 0;
    }
    if (MultiByteToWideChar(0, 0, text, -1, wide, characters)) return wide;
    const DWORD error = GetLastError();
    HeapFree(GetProcessHeap(), 0, wide);
    SetLastError(error);
    return 0;
}

static void ReleaseWideCopy(WCHAR *wide) {
    const DWORD error = GetLastError();
    HeapFree(GetProcessHeap(), 0, wide);
    SetLastError(error);
}

void *WINAPI XboxILCreateFromPathA(const char *path) {
    typedef void *(WINAPI *CreateList)(const WCHAR *);
    typedef long (WINAPI *ParseName)(const WCHAR *, void *, void **, unsigned long, unsigned long *);
    CreateList createList = (CreateList)SystemExport(L"shell32.dll", "ILCreateFromPathW");
    ParseName parseName = (ParseName)SystemExport(L"shell32.dll", "SHParseDisplayName");
    if (!createList && !parseName) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    WCHAR *wide = WideCopy(path);
    if (!wide) return 0;
    void *list = 0;
    if (createList) list = createList(wide);
    else if (parseName(wide, 0, &list, 0, 0) < 0) list = 0;
    ReleaseWideCopy(wide);
    return list;
}

int WINAPI XboxPlaySoundA(const char *sound, Module resource, DWORD flags) {
    typedef int (WINAPI *PlaySound)(const WCHAR *, Module, DWORD);
    PlaySound widePlaySound = (PlaySound)SystemExport(L"winmm.dll", "PlaySoundW");
    if (!widePlaySound) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    /* The API also accepts buffers and integer resource identifiers. */
    if (!sound || (flags & SND_MEMORY) || (unsigned long long)sound <= 0xffffu)
        return widePlaySound((const WCHAR *)sound, resource, flags);
    WCHAR *wide = WideCopy(sound);
    if (!wide) return 0;
    const int played = widePlaySound(wide, resource, flags);
    ReleaseWideCopy(wide);
    return played;
}
