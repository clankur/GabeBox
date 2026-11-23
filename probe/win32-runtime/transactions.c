/* Map the Win32 transaction API to the operating system's transaction service. */
#define IMPORT __declspec(dllimport)
#define WINAPI __stdcall
#define TRANSACTION_ALL_ACCESS 0x001f003fu
#define OBJ_INHERIT 2u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_NOT_SUPPORTED 50u
#define INVALID_HANDLE_VALUE ((Handle)(long long)-1)

typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef long NativeStatus;
typedef void *Handle;
typedef void (WINAPI *Procedure)(void);

typedef struct {
    DWORD size;
    void *descriptor;
    int inherit;
} SecurityAttributes;

typedef struct {
    unsigned short length;
    unsigned short maximumLength;
    const WCHAR *buffer;
} NativeString;

typedef struct {
    DWORD length;
    Handle directory;
    const NativeString *name;
    DWORD attributes;
    void *descriptor;
    void *qualityOfService;
} NativeAttributes;

IMPORT Handle WINAPI GetModuleHandleW(const WCHAR *name);
IMPORT Procedure WINAPI GetProcAddress(Handle module, const char *name);
IMPORT void WINAPI SetLastError(DWORD error);

static DWORD Win32Error(Handle native, NativeStatus status) {
    typedef DWORD (WINAPI *TranslateError)(NativeStatus);
    TranslateError translate = (TranslateError)GetProcAddress(native, "RtlNtStatusToDosError");
    return translate ? translate(status) : ERROR_NOT_SUPPORTED;
}

Handle WINAPI XboxCreateTransaction(const SecurityAttributes *security, void *unitOfWork, DWORD options,
        DWORD isolation, DWORD isolationFlags, DWORD timeout, const WCHAR *description) {
    typedef NativeStatus (WINAPI *Create)(Handle *, DWORD, const NativeAttributes *, void *, Handle, DWORD,
        DWORD, DWORD, const long long *, const NativeString *);
    const Handle native = GetModuleHandleW(L"ntdll.dll");
    Create create = native ? (Create)GetProcAddress(native, "NtCreateTransaction") : 0;
    if (!create) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }
    if (security && security->size != sizeof(SecurityAttributes)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    unsigned int length = 0;
    while (description && length < 32767 && description[length]) ++length;
    if (length == 32767) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const NativeString label = {(unsigned short)(length * 2), (unsigned short)((length + 1) * 2), description};
    const NativeAttributes attributes = {sizeof(attributes), 0, 0,
        security && security->inherit ? OBJ_INHERIT : 0, security ? security->descriptor : 0, 0};
    const long long due = -(long long)timeout * 10000;
    Handle transaction = 0;
    const NativeStatus status = create(&transaction, TRANSACTION_ALL_ACCESS, &attributes, unitOfWork, 0, options,
        isolation, isolationFlags, timeout && timeout != 0xffffffffu ? &due : 0, description ? &label : 0);
    if (status >= 0) return transaction;
    SetLastError(Win32Error(native, status));
    return INVALID_HANDLE_VALUE;
}

int WINAPI XboxCommitTransaction(Handle transaction) {
    typedef NativeStatus (WINAPI *Commit)(Handle, unsigned char);
    const Handle native = GetModuleHandleW(L"ntdll.dll");
    Commit commit = native ? (Commit)GetProcAddress(native, "NtCommitTransaction") : 0;
    if (!commit) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    const NativeStatus status = commit(transaction, 1);
    if (status >= 0) return 1;
    SetLastError(Win32Error(native, status));
    return 0;
}
