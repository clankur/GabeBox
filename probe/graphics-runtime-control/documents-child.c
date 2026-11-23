/* Exercise a scoped Documents path through a redirected Shell32 import. */
#include "../child-process.h"

#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define PATH_CAPACITY 260
#define CSIDL_PERSONAL 5
#define DONT_VERIFY 0x4000
#define CREATE_FOLDER 0x8000
#define INVALID_HANDLE ((void *)(long long)-1)

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef long (WINAPI *FolderPath)(void *, int, void *, DWORD, WCHAR *);

IMPORT long WINAPI SHGetFolderPathW(void *window, int folder, void *token, DWORD mode, WCHAR *path);
IMPORT DWORD WINAPI GetTempPathW(DWORD capacity, WCHAR *path);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetFileAttributesW(const WCHAR *path);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *value, DWORD capacity);
IMPORT int WINAPI SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value);
IMPORT void *WINAPI CreateFile2(const WCHAR *path, DWORD access, DWORD share, DWORD disposition, const void *parameters);
IMPORT int WINAPI WriteFile(void *file, const void *bytes, DWORD size, DWORD *written, void *overlapped);
IMPORT int WINAPI ReadFile(void *file, void *bytes, DWORD size, DWORD *read, void *overlapped);
IMPORT int WINAPI CloseHandle(void *handle);
IMPORT int WINAPI DeleteFileW(const WCHAR *path);
IMPORT int WINAPI RemoveDirectoryW(const WCHAR *path);
IMPORT void *WINAPI LoadLibraryExW(const WCHAR *path, void *file, DWORD flags);
IMPORT void (WINAPI *GetProcAddress(void *module, const char *name))(void);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

static int SamePath(const WCHAR *left, const WCHAR *right) {
    for (; *left == *right; ++left, ++right) {
        if (!*left) return 1;
    }
    return 0;
}

static int SaveAndReload(const WCHAR *file) {
    void *handle = CreateFile2(file, 0x40000000u, 0, 1, 0);
    if (handle == INVALID_HANDLE) return 0;
    const DWORD value = 0x1234abcdu;
    DWORD transferred = 0;
    int ok = WriteFile(handle, &value, sizeof(value), &transferred, 0) && transferred == sizeof(value);
    CloseHandle(handle);
    if (ok) {
        handle = CreateFile2(file, 0x80000000u, 0, 3, 0);
        ok = handle != INVALID_HANDLE;
        if (ok) {
            DWORD read = 0;
            ok = ReadFile(handle, &read, sizeof(read), &transferred, 0) &&
                transferred == sizeof(read) && read == value;
            CloseHandle(handle);
        }
    }
    return DeleteFileW(file) && ok;
}

static unsigned CheckDocuments(void) {
    const WCHAR variable[] = L"XBOX_WIN32_DOCUMENTS";
    if (GetEnvironmentVariableW(variable, 0, 0) && !SetEnvironmentVariableW(variable, 0)) return 1;
    void *shell = LoadLibraryExW(L"shell32.dll", 0, 0x800u);
    const FolderPath native = shell ? (FolderPath)GetProcAddress(shell, "SHGetFolderPathW") : 0;
    WCHAR output[PATH_CAPACITY], original[PATH_CAPACITY];
    original[0] = 0;
    const long underlying = native ? native(0, CSIDL_PERSONAL, 0, 0, original) : (long)0x80004001u;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL, 0, 0, output) != underlying || !SamePath(output, original)) return 2;

    WCHAR directory[PATH_CAPACITY], file[PATH_CAPACITY];
    const DWORD prefix = GetTempPathW(PATH_CAPACITY, directory);
    if (!prefix || prefix >= PATH_CAPACITY - 48) return 3;
    DWORD length = prefix;
    for (const WCHAR *name = L"xbox-documents-control-"; *name; ++name) directory[length++] = *name;
    const DWORD process = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4) directory[length++] = L"0123456789abcdef"[(process >> shift) & 15];
    directory[length] = 0;
    if (GetFileAttributesW(directory) != 0xffffffffu || !SetEnvironmentVariableW(variable, directory)) return 3;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL, 0, 0, output) >= 0 || output[0]) return 4;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL | DONT_VERIFY, 0, 0, output) || !SamePath(output, directory) ||
            GetFileAttributesW(directory) != 0xffffffffu) return 5;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL | CREATE_FOLDER | DONT_VERIFY, 0, 0, output) >= 0 || output[0]) return 6;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL | CREATE_FOLDER, 0, 0, output) || !SamePath(output, directory)) {
        RemoveDirectoryW(directory);
        return 7;
    }

    unsigned error = 0;
    if (SHGetFolderPathW(0, CSIDL_PERSONAL, 0, 1, output) || !SamePath(output, directory)) error = 8;
    else {
        for (DWORD i = 0; i < length; ++i) file[i] = output[i];
        for (const WCHAR *name = L"\\fixture.save"; *name; ++name) file[length++] = *name;
        file[length] = 0;
        if (!SaveAndReload(file)) error = 9;
    }
    if (!RemoveDirectoryW(directory) && !error) error = 10;
    return error;
}

void WINAPI ChildEntry(void) {
    const unsigned result = CheckDocuments();
    ExitProcess(result ? 0x6D000000u | result : PROBE_EXPECTED_CHILD_EXIT);
}
