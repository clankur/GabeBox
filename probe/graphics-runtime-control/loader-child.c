/* Compare normal imports for a process outside the package with and without
   an adjacent SDL DLL. The only files involved are private test fixtures. */
#include "../../runtime/shared/child-process.h"
#include "sdl-version.h"

#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define PATH_CAPACITY 420u

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef void *Handle;

typedef struct {
    DWORD size;
    WCHAR *reserved, *desktop, *title;
    DWORD x, y, width, height, charsX, charsY, attributes, flags;
    unsigned short showWindow, reservedSize;
    unsigned char *reservedBytes;
    Handle input, output, error;
} StartupInfo;

typedef struct {
    Handle process, thread;
    DWORD processId, threadId;
} ProcessInfo;

_Static_assert(sizeof(StartupInfo) == 104 && sizeof(ProcessInfo) == 24, "x64 process ABI");

IMPORT WCHAR *WINAPI GetCommandLineW(void);
IMPORT Handle WINAPI GetModuleHandleW(const WCHAR *module);
IMPORT DWORD WINAPI GetModuleFileNameW(Handle module, WCHAR *path, DWORD capacity);
IMPORT DWORD WINAPI GetTempPathW(DWORD capacity, WCHAR *path);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT int WINAPI CreateDirectoryW(const WCHAR *path, void *security);
IMPORT int WINAPI RemoveDirectoryW(const WCHAR *path);
IMPORT int WINAPI CopyFileW(const WCHAR *source, const WCHAR *target, int failIfExists);
IMPORT int WINAPI DeleteFileW(const WCHAR *path);
IMPORT int WINAPI CreateProcessW(const WCHAR *image, WCHAR *command, void *processSecurity,
    void *threadSecurity, int inherit, DWORD flags, void *environment, const WCHAR *directory,
    StartupInfo *startup, ProcessInfo *process);
IMPORT DWORD WINAPI WaitForSingleObject(Handle process, DWORD timeout);
IMPORT int WINAPI GetExitCodeProcess(Handle process, DWORD *exitCode);
IMPORT int WINAPI TerminateProcess(Handle process, unsigned int exitCode);
IMPORT int WINAPI CloseHandle(Handle handle);
IMPORT __declspec(noreturn) void WINAPI ExitProcess(unsigned int code);

struct SDLVersion { unsigned char major, minor, patch; };
IMPORT void SDL_GetVersion(struct SDLVersion *version);
IMPORT const unsigned char *WINAPI glGetString(unsigned int name);

/* Keep a normal linker import of the desktop OpenGL ABI as well as SDL. */
static const unsigned char *(WINAPI *volatile glImport)(unsigned int) = glGetString;

static DWORD DirectoryLength(const WCHAR *path, DWORD length) {
    if (length >= PATH_CAPACITY) return 0;
    while (length && path[length - 1] != L'\\') --length;
    return length;
}

static DWORD Append(WCHAR *path, DWORD size, const WCHAR *suffix) {
    while (*suffix && size + 1 < PATH_CAPACITY) path[size++] = *suffix++;
    path[size] = L'\0';
    return *suffix ? 0 : size;
}

static int SameDirectory(const WCHAR *a, DWORD aLength, const WCHAR *b, DWORD bLength) {
    if (!aLength || aLength != bLength) return 0;
    for (DWORD index = 0; index < aLength; ++index) {
        if (a[index] != b[index]) return 0;
    }
    return 1;
}

static unsigned int InspectImports(void) {
    struct SDLVersion version = {0};
    SDL_GetVersion(&version);
    if (version.major == 255) return 0x6402;
    if (version.major != 2 || version.minor != SDL_EXPECTED_MINOR || version.patch != SDL_EXPECTED_PATCH)
        return 0x6403;
    if (!glImport) return 0x6404;
    WCHAR image[PATH_CAPACITY], sdl[PATH_CAPACITY], gl[PATH_CAPACITY];
    Handle sdlModule = GetModuleHandleW(L"SDL2.dll");
    Handle glModule = GetModuleHandleW(L"opengl32.dll");
    if (!sdlModule || !glModule) return 0x6404;
    DWORD imageDir = DirectoryLength(image, GetModuleFileNameW(0, image, PATH_CAPACITY));
    DWORD sdlDir = DirectoryLength(sdl, GetModuleFileNameW(sdlModule, sdl, PATH_CAPACITY));
    DWORD glDir = DirectoryLength(gl, GetModuleFileNameW(glModule, gl, PATH_CAPACITY));
    if (!imageDir || !SameDirectory(sdl, sdlDir, gl, glDir)) return 0x6405;
    return SameDirectory(image, imageDir, sdl, sdlDir) ? 0x6406 : PROBE_EXPECTED_CHILD_EXIT;
}

static unsigned int RunSecondary(const WCHAR *path) {
    StartupInfo startup = {sizeof(startup), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ProcessInfo process;
    WCHAR command[] = L"runtime-loader-secondary";
    if (!CreateProcessW(path, command, 0, 0, 0, 0, 0, 0, &startup, &process))
        return 0x64150000u | (GetLastError() & 0xffffu);
    DWORD result;
    if (WaitForSingleObject(process.process, 3000) != 0) {
        TerminateProcess(process.process, 0x6407);
        result = 0x6407;
    } else if (!GetExitCodeProcess(process.process, &result)) {
        result = 0x64160000u | (GetLastError() & 0xffffu);
    }
    CloseHandle(process.thread);
    CloseHandle(process.process);
    return result;
}

static unsigned int TestLoaderOrder(void) {
    static const WCHAR digits[] = L"0123456789abcdef";
    WCHAR source[PATH_CAPACITY], target[PATH_CAPACITY];
    WCHAR originalDll[PATH_CAPACITY], localDll[PATH_CAPACITY];
    DWORD sourceSize = GetModuleFileNameW(0, source, PATH_CAPACITY);
    DWORD packageSize = DirectoryLength(source, sourceSize);
    DWORD tempSize = GetTempPathW(PATH_CAPACITY, target);
    if (!packageSize || !tempSize || tempSize >= PATH_CAPACITY) return 0x6408;
    DWORD dirSize = Append(target, tempSize, L"XboxRuntimeControl-");
    if (!dirSize || dirSize + 9 >= PATH_CAPACITY) return 0x6408;
    DWORD pid = GetCurrentProcessId();
    for (int bit = 28; bit >= 0; bit -= 4) target[dirSize++] = digits[(pid >> bit) & 15];
    target[dirSize] = L'\0';
    for (DWORD index = 0; index < packageSize; ++index) originalDll[index] = source[index];
    for (DWORD index = 0; index < dirSize; ++index) localDll[index] = target[index];
    if (!Append(originalDll, packageSize, L"probe-local-sdl.dll") ||
            !Append(localDll, dirSize, L"\\SDL2.dll")) return 0x6408;
    if (!CreateDirectoryW(target, 0)) return 0x64110000u | (GetLastError() & 0xffffu);

    unsigned int result;
    if (!Append(target, dirSize, L"\\loader.exe") || !CopyFileW(source, target, 1)) {
        result = 0x64120000u | (GetLastError() & 0xffffu);
    } else {
        result = RunSecondary(target);
        if (result == PROBE_EXPECTED_CHILD_EXIT) {
            if (CopyFileW(originalDll, localDll, 1)) {
                result = RunSecondary(target);
                DeleteFileW(localDll);
            } else {
                result = 0x64130000u | (GetLastError() & 0xffffu);
            }
        }
        DeleteFileW(target);
    }
    target[dirSize] = L'\0';
    RemoveDirectoryW(target);
    return result;
}

void WINAPI ChildEntry(void) {
    const WCHAR *command = GetCommandLineW();
    const WCHAR *argument = command;
    for (const WCHAR *cursor = command; *cursor; ++cursor) {
        if (*cursor == L' ') argument = cursor + 1;
    }
    const WCHAR *mode = L"runtime-loader-secondary";
    while (*mode && *argument == *mode) { ++mode; ++argument; }
    ExitProcess(!*mode && !*argument ? InspectImports() : TestLoaderOrder());
}
