/* ABI adapters for legacy shell paths and waveform sound. */
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u
#define SND_MEMORY 0x4u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_OUTOFMEMORY 14u

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Module;
typedef void (WINAPI *Procedure)(void);

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

static Procedure SystemExport(const WCHAR *dll, const char *name) {
    Module module = GetModuleHandleW(dll);
    if (!module) module = LoadLibraryExW(dll, 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : 0;
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
