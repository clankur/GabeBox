/*
 * Minimal UWP CoreApplication probe.  This file intentionally uses only the
 * Win32 C ABI and the small WinRT ABI declarations below.  It has no CRT,
 * desktop windowing, arbitrary child path, or fallback launch mechanism.
 *
 * CoreApplication / IFrameworkView and Storage interface UUIDs and vtable
 * method ordering are taken from Windows SDK 10.0.26100.0 headers.
 */

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef unsigned long DWORD;
typedef unsigned long ULONG;
typedef long LONG;
typedef long HRESULT;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HINSTANCE;
typedef WCHAR *PWSTR;
typedef const WCHAR *PCWSTR;
typedef struct { unsigned long Data1; unsigned short Data2; unsigned short Data3; BYTE Data4[8]; } GUID;
typedef GUID IID;
typedef const IID *REFIID;
typedef void *HSTRING;

#define WINAPI __stdcall
#define STDMETHODCALLTYPE __stdcall
#define TRUE 1
#define FALSE 0
#define S_OK ((HRESULT)0)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define RO_INIT_MULTITHREADED 1u
#define MAX_PATH 260u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define CREATE_ALWAYS 2u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define WAIT_OBJECT_0 0x00000000u
#define WAIT_FAILED 0xFFFFFFFFu
#define ERROR_SUCCESS 0u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_WRITE_FAULT 29u
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define EXPECTED_CHILD_EXIT 0x51A7u
#define REPORT_NAME L"probe-report.txt"
#define CHILD_NAME L"child.exe"

typedef __INTPTR_TYPE__ LONG_PTR;

typedef struct STARTUPINFOW {
    DWORD cb;
    PWSTR lpReserved;
    PWSTR lpDesktop;
    PWSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    BYTE *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;

typedef struct PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION;

__declspec(dllimport) HRESULT WINAPI RoInitialize(DWORD initType);
__declspec(dllimport) void WINAPI RoUninitialize(void);
__declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(HSTRING className, REFIID iid, void **factory);
__declspec(dllimport) HRESULT WINAPI WindowsCreateString(PCWSTR source, DWORD length, HSTRING *value);
__declspec(dllimport) HRESULT WINAPI WindowsDeleteString(HSTRING value);
__declspec(dllimport) PCWSTR WINAPI WindowsGetStringRawBuffer(HSTRING value, DWORD *length);
static LONG InterlockedIncrement(volatile LONG *value) {
    return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}
static LONG InterlockedDecrement(volatile LONG *value) {
    return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST);
}
__declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HINSTANCE module, PWSTR fileName, DWORD size);
__declspec(dllimport) DWORD WINAPI GetLastError(void);
__declspec(dllimport) void WINAPI SetLastError(DWORD error);
__declspec(dllimport) BOOL WINAPI CreateProcessW(PCWSTR applicationName, PWSTR commandLine,
    void *processAttributes, void *threadAttributes, BOOL inheritHandles, DWORD creationFlags,
    void *environment, PCWSTR currentDirectory, STARTUPINFOW *startup, PROCESS_INFORMATION *processInfo);
__declspec(dllimport) DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);
__declspec(dllimport) BOOL WINAPI GetExitCodeProcess(HANDLE process, DWORD *exitCode);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE handle);
__declspec(dllimport) HANDLE WINAPI CreateFileW(PCWSTR fileName, DWORD access, DWORD shareMode,
    void *securityAttributes, DWORD creationDisposition, DWORD flags, HANDLE templateFile);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD bytesToWrite,
    DWORD *bytesWritten, void *overlapped);
__declspec(dllimport) BOOL WINAPI FlushFileBuffers(HANDLE file);
__declspec(dllimport) void WINAPI ExitProcess(unsigned int exitCode);

typedef struct IUnknownLike IUnknownLike;
typedef struct IInspectableLike IInspectableLike;
typedef struct ICoreApplication ICoreApplication;
typedef struct ICoreApplicationExit ICoreApplicationExit;
typedef struct IFrameworkView IFrameworkView;
typedef struct IFrameworkViewSource IFrameworkViewSource;
typedef struct IApplicationDataStatics IApplicationDataStatics;
typedef struct IApplicationData IApplicationData;
typedef struct IStorageFolder IStorageFolder;
typedef struct IStorageItem IStorageItem;

typedef struct IInspectableVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
} IInspectableVtbl;

struct IInspectableLike { const IInspectableVtbl *lpVtbl; };

typedef struct ICoreApplicationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    void *get_Id, *add_Suspending, *remove_Suspending, *add_Resuming, *remove_Resuming;
    void *get_Properties, *GetCurrentView;
    HRESULT (STDMETHODCALLTYPE *Run)(ICoreApplication *, IFrameworkViewSource *);
    void *RunWithActivationFactories;
} ICoreApplicationVtbl;
struct ICoreApplication { const ICoreApplicationVtbl *lpVtbl; };

typedef struct ICoreApplicationExitVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    HRESULT (STDMETHODCALLTYPE *Exit)(ICoreApplicationExit *);
    void *add_Exiting, *remove_Exiting;
} ICoreApplicationExitVtbl;
struct ICoreApplicationExit { const ICoreApplicationExitVtbl *lpVtbl; };

typedef struct IFrameworkViewVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IFrameworkView *, IInspectableLike *);
    HRESULT (STDMETHODCALLTYPE *SetWindow)(IFrameworkView *, IInspectableLike *);
    HRESULT (STDMETHODCALLTYPE *Load)(IFrameworkView *, HSTRING);
    HRESULT (STDMETHODCALLTYPE *Run)(IFrameworkView *);
    HRESULT (STDMETHODCALLTYPE *Uninitialize)(IFrameworkView *);
} IFrameworkViewVtbl;
struct IFrameworkView { const IFrameworkViewVtbl *lpVtbl; };

typedef struct IFrameworkViewSourceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    HRESULT (STDMETHODCALLTYPE *CreateView)(IFrameworkViewSource *, IFrameworkView **);
} IFrameworkViewSourceVtbl;
struct IFrameworkViewSource { const IFrameworkViewSourceVtbl *lpVtbl; };

typedef struct IApplicationDataStaticsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    HRESULT (STDMETHODCALLTYPE *get_Current)(IApplicationDataStatics *, IApplicationData **);
} IApplicationDataStaticsVtbl;
struct IApplicationDataStatics { const IApplicationDataStaticsVtbl *lpVtbl; };

typedef struct IApplicationDataVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
    void *get_Version, *SetVersionAsync, *ClearAllAsync, *ClearAsync, *get_LocalSettings, *get_RoamingSettings;
    HRESULT (STDMETHODCALLTYPE *get_LocalFolder)(IApplicationData *, IStorageFolder **);
} IApplicationDataVtbl;
struct IApplicationData { const IApplicationDataVtbl *lpVtbl; };

struct IStorageFolder { const void *lpVtbl; };
struct IStorageItem { const void *lpVtbl; };

/* Slot numbers in the x64 WinRT ABI, independently checked against SDK 26100. */
_Static_assert(sizeof(STARTUPINFOW) == 104 && sizeof(PROCESS_INFORMATION) == 24, "x64 process ABI");
_Static_assert(__builtin_offsetof(ICoreApplicationVtbl, Run) == 13 * sizeof(void *), "ICoreApplication.Run");
_Static_assert(__builtin_offsetof(IApplicationDataVtbl, get_LocalFolder) == 12 * sizeof(void *), "IApplicationData.LocalFolder");
enum { STORAGE_ITEM_GET_PATH = 12 };

static const IID IID_IUNKNOWN =
    {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID IID_IINSPECTABLE =
    {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const IID IID_IAGILEOBJECT =
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const IID IID_CORE_APPLICATION =
    {0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
static const IID IID_CORE_APPLICATION_EXIT =
    {0xcf86461d, 0x261e, 0x4b72, {0x9a, 0xcd, 0x44, 0xed, 0x2a, 0xce, 0x6a, 0x29}};
static const IID IID_FRAMEWORK_VIEW =
    {0xfaab5cd0, 0x8924, 0x45ac, {0xad, 0x0f, 0xa0, 0x8f, 0xae, 0x5d, 0x03, 0x24}};
static const IID IID_FRAMEWORK_VIEW_SOURCE =
    {0xcd770614, 0x65c4, 0x426c, {0x94, 0x94, 0x34, 0xfc, 0x43, 0x55, 0x48, 0x62}};
static const IID IID_APPLICATION_DATA_STATICS =
    {0x5612147b, 0xe843, 0x45e3, {0x94, 0xd8, 0x06, 0x16, 0x9e, 0x3c, 0x8e, 0x17}};
static const IID IID_STORAGE_ITEM =
    {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};

typedef struct ViewObject { IFrameworkView iface; volatile LONG refs; } ViewObject;
typedef struct ViewSourceObject { IFrameworkViewSource iface; volatile LONG refs; } ViewSourceObject;
static ViewObject g_view;
static ViewSourceObject g_source;

static BOOL GuidEqual(REFIID left, const IID *right) {
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    DWORD i;
    if (!left || !right) return FALSE;
    for (i = 0; i < (DWORD)sizeof(IID); ++i) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE GetIids(void *self, ULONG *count, IID **iids) {
    (void)self;
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = (IID *)0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE GetRuntimeClassName(void *self, HSTRING *name) {
    static const WCHAR className[] = L"Probe.FrameworkView";
    (void)self;
    if (!name) return E_POINTER;
    return WindowsCreateString(className, 19, name);
}

static HRESULT STDMETHODCALLTYPE GetTrustLevel(void *self, int *level) {
    (void)self;
    if (!level) return E_POINTER;
    *level = 0; /* BaseTrust */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ViewQueryInterface(void *self, REFIID iid, void **out) {
    ViewObject *object = (ViewObject *)self;
    if (!out) return E_POINTER;
    *out = (void *)0;
    if (GuidEqual(iid, &IID_IUNKNOWN) || GuidEqual(iid, &IID_IINSPECTABLE) ||
        GuidEqual(iid, &IID_IAGILEOBJECT) || GuidEqual(iid, &IID_FRAMEWORK_VIEW)) {
        *out = self;
        InterlockedIncrement(&object->refs);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ViewAddRef(void *self) {
    return (ULONG)InterlockedIncrement(&((ViewObject *)self)->refs);
}

static ULONG STDMETHODCALLTYPE ViewRelease(void *self) {
    LONG remaining = InterlockedDecrement(&((ViewObject *)self)->refs);
    if (remaining < 0) {
        InterlockedIncrement(&((ViewObject *)self)->refs);
        return 0;
    }
    return (ULONG)remaining;
}

static HRESULT RunProbe(void);

static HRESULT STDMETHODCALLTYPE ViewInitialize(IFrameworkView *self, IInspectableLike *applicationView) {
    (void)self; (void)applicationView;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ViewSetWindow(IFrameworkView *self, IInspectableLike *window) {
    (void)self; (void)window;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ViewLoad(IFrameworkView *self, HSTRING entryPoint) {
    (void)self; (void)entryPoint;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ViewRun(IFrameworkView *self) {
    HRESULT hr;
    static const WCHAR className[] = L"Windows.ApplicationModel.Core.CoreApplication";
    HSTRING hClass = (HSTRING)0;
    ICoreApplicationExit *coreExit = (ICoreApplicationExit *)0;
    (void)self;
    hr = RunProbe();
    if (SUCCEEDED(WindowsCreateString(className,
        (DWORD)(sizeof(className) / sizeof(className[0]) - 1), &hClass))) {
        if (SUCCEEDED(RoGetActivationFactory(hClass, &IID_CORE_APPLICATION_EXIT, (void **)&coreExit))) {
            coreExit->lpVtbl->Exit(coreExit);
            coreExit->lpVtbl->Release(coreExit);
        }
        WindowsDeleteString(hClass);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ViewUninitialize(IFrameworkView *self) { (void)self; return S_OK; }

static const IFrameworkViewVtbl g_view_vtbl = {
    ViewQueryInterface, ViewAddRef, ViewRelease, GetIids, GetRuntimeClassName, GetTrustLevel,
    ViewInitialize, ViewSetWindow, ViewLoad, ViewRun, ViewUninitialize
};

static HRESULT STDMETHODCALLTYPE SourceQueryInterface(void *self, REFIID iid, void **out) {
    ViewSourceObject *object = (ViewSourceObject *)self;
    if (!out) return E_POINTER;
    *out = (void *)0;
    if (GuidEqual(iid, &IID_IUNKNOWN) || GuidEqual(iid, &IID_IINSPECTABLE) ||
        GuidEqual(iid, &IID_IAGILEOBJECT) || GuidEqual(iid, &IID_FRAMEWORK_VIEW_SOURCE)) {
        *out = self;
        InterlockedIncrement(&object->refs);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE SourceAddRef(void *self) {
    return (ULONG)InterlockedIncrement(&((ViewSourceObject *)self)->refs);
}

static ULONG STDMETHODCALLTYPE SourceRelease(void *self) {
    LONG remaining = InterlockedDecrement(&((ViewSourceObject *)self)->refs);
    if (remaining < 0) {
        InterlockedIncrement(&((ViewSourceObject *)self)->refs);
        return 0;
    }
    return (ULONG)remaining;
}

static HRESULT STDMETHODCALLTYPE SourceCreateView(IFrameworkViewSource *self, IFrameworkView **view) {
    (void)self;
    if (!view) return E_POINTER;
    g_view.iface.lpVtbl = &g_view_vtbl;
    InterlockedIncrement(&g_view.refs);
    *view = &g_view.iface;
    return S_OK;
}

static const IFrameworkViewSourceVtbl g_source_vtbl = {
    SourceQueryInterface, SourceAddRef, SourceRelease, GetIids, GetRuntimeClassName, GetTrustLevel,
    SourceCreateView
};

static void AppendChar(WCHAR *buffer, DWORD capacity, DWORD *used, WCHAR value) {
    if (*used + 1 < capacity) { buffer[(*used)++] = value; buffer[*used] = 0; }
}

static void AppendText(WCHAR *buffer, DWORD capacity, DWORD *used, PCWSTR text) {
    while (*text) AppendChar(buffer, capacity, used, *text++);
}

static void AppendHex(WCHAR *buffer, DWORD capacity, DWORD *used, DWORD value) {
    static const WCHAR digits[] = L"0123456789ABCDEF";
    int shift;
    AppendText(buffer, capacity, used, L"0x");
    for (shift = 28; shift >= 0; shift -= 4) AppendChar(buffer, capacity, used, digits[(value >> shift) & 0xF]);
}

static HRESULT GetLocalStatePath(WCHAR *path, DWORD capacity) {
    static const WCHAR runtimeClass[] = L"Windows.Storage.ApplicationData";
    HSTRING className = (HSTRING)0;
    HSTRING folderPath = (HSTRING)0;
    IApplicationDataStatics *statics = (IApplicationDataStatics *)0;
    IApplicationData *appData = (IApplicationData *)0;
    IStorageFolder *folder = (IStorageFolder *)0;
    IStorageItem *item = (IStorageItem *)0;
    DWORD folderChars = 0;
    PCWSTR folderBuffer;
    HRESULT hr;
    hr = WindowsCreateString(runtimeClass, (DWORD)(sizeof(runtimeClass) / sizeof(runtimeClass[0]) - 1), &className);
    if (FAILED(hr)) goto done;
    hr = RoGetActivationFactory(className, &IID_APPLICATION_DATA_STATICS, (void **)&statics);
    if (FAILED(hr)) goto done;
    hr = statics->lpVtbl->get_Current(statics, &appData);
    if (FAILED(hr)) goto done;
    hr = appData->lpVtbl->get_LocalFolder(appData, &folder);
    if (FAILED(hr)) goto done;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(IStorageFolder *, REFIID, void **))((void **)folder->lpVtbl)[0])(folder, &IID_STORAGE_ITEM, (void **)&item);
    if (FAILED(hr)) goto done;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(IStorageItem *, HSTRING *))((void **)item->lpVtbl)[STORAGE_ITEM_GET_PATH])(item, &folderPath);
    if (FAILED(hr)) goto done;
    folderBuffer = WindowsGetStringRawBuffer(folderPath, &folderChars);
    if (!folderBuffer || folderChars + 1 >= capacity) { hr = (HRESULT)0x8007007AL; goto done; }
    {
        DWORD i;
        for (i = 0; i < folderChars; ++i) path[i] = folderBuffer[i];
        path[folderChars] = 0;
    }
done:
    if (item) ((ULONG (STDMETHODCALLTYPE *)(IStorageItem *))((void **)item->lpVtbl)[2])(item);
    if (folder) ((ULONG (STDMETHODCALLTYPE *)(IStorageFolder *))((void **)folder->lpVtbl)[2])(folder);
    if (appData) appData->lpVtbl->Release(appData);
    if (statics) statics->lpVtbl->Release(statics);
    if (folderPath) WindowsDeleteString(folderPath);
    if (className) WindowsDeleteString(className);
    return hr;
}

static HRESULT WriteReport(PCWSTR content) {
    static const BYTE bom[2] = {0xFF, 0xFE};
    WCHAR path[MAX_PATH];
    DWORD pathChars = 0;
    DWORD contentChars = 0;
    DWORD bytesWritten = 0;
    HANDLE file;
    HRESULT hr = GetLocalStatePath(path, MAX_PATH);
    if (FAILED(hr)) return hr;
    while (path[pathChars]) ++pathChars;
    if (pathChars + 1 + (DWORD)(sizeof(REPORT_NAME) / sizeof(REPORT_NAME[0])) >= MAX_PATH) return (HRESULT)0x8007007AL;
    path[pathChars++] = L'\\';
    AppendText(path, MAX_PATH, &pathChars, REPORT_NAME);
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, (void *)0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, (HANDLE)0);
    if (file == INVALID_HANDLE_VALUE) return (HRESULT)(0x80070000L | (GetLastError() & 0xFFFF));
    if (!WriteFile(file, bom, 2, &bytesWritten, (void *)0) || bytesWritten != 2) {
        hr = (HRESULT)(0x80070000L | (GetLastError() & 0xFFFF));
        CloseHandle(file);
        return hr;
    }
    while (content[contentChars]) ++contentChars;
    if (!WriteFile(file, content, contentChars * sizeof(WCHAR), &bytesWritten, (void *)0) ||
        bytesWritten != contentChars * sizeof(WCHAR)) {
        hr = (HRESULT)(0x80070000L | (GetLastError() & 0xFFFF));
    } else if (!FlushFileBuffers(file)) {
        hr = (HRESULT)(0x80070000L | (GetLastError() & 0xFFFF));
    } else {
        hr = S_OK;
    }
    CloseHandle(file);
    return hr;
}

static HRESULT RunProbe(void) {
    WCHAR report[1024];
    WCHAR modulePath[MAX_PATH];
    WCHAR childPath[MAX_PATH];
    DWORD used = 0;
    DWORD moduleChars;
    DWORD childChars;
    DWORD win32Error = ERROR_SUCCESS;
    DWORD waitResult = WAIT_FAILED;
    DWORD exitCode = 0;
    BOOL created;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HRESULT hr;

    report[0] = 0;
    AppendText(report, 1024, &used, L"probe=packaged-child-process\r\nstatus=starting\r\n");
    hr = WriteReport(report);
    if (FAILED(hr)) return hr;

    moduleChars = GetModuleFileNameW((HINSTANCE)0, modulePath, MAX_PATH);
    if (moduleChars == 0 || moduleChars >= MAX_PATH) {
        win32Error = GetLastError();
        if (win32Error == ERROR_SUCCESS) win32Error = ERROR_INSUFFICIENT_BUFFER;
        used = 0; report[0] = 0;
        AppendText(report, 1024, &used, L"probe=packaged-child-process\r\nstatus=path-error\r\nerror=");
        AppendHex(report, 1024, &used, win32Error);
        AppendText(report, 1024, &used, L"\r\n");
        return WriteReport(report);
    }
    childChars = moduleChars;
    while (childChars && modulePath[childChars - 1] != L'\\' && modulePath[childChars - 1] != L'/') --childChars;
    if (!childChars || childChars + (DWORD)(sizeof(CHILD_NAME) / sizeof(CHILD_NAME[0])) > MAX_PATH) {
        used = 0; report[0] = 0;
        AppendText(report, 1024, &used, L"probe=packaged-child-process\r\nstatus=path-error\r\nerror=invalid-module-path\r\n");
        return WriteReport(report);
    }
    {
        DWORD i;
        for (i = 0; i < childChars; ++i) childPath[i] = modulePath[i];
        childPath[childChars] = 0;
    }
    AppendText(childPath, MAX_PATH, &childChars, CHILD_NAME);

    startup.cb = sizeof(startup);
    startup.lpReserved = (PWSTR)0;
    startup.lpDesktop = (PWSTR)0;
    startup.lpTitle = (PWSTR)0;
    startup.dwX = startup.dwY = startup.dwXSize = startup.dwYSize = 0;
    startup.dwXCountChars = startup.dwYCountChars = 0;
    startup.dwFillAttribute = 0;
    startup.dwFlags = 0;
    startup.wShowWindow = 0;
    startup.cbReserved2 = 0;
    startup.lpReserved2 = (BYTE *)0;
    startup.hStdInput = startup.hStdOutput = startup.hStdError = (HANDLE)0;
    process.hProcess = process.hThread = (HANDLE)0;
    process.dwProcessId = process.dwThreadId = 0;

    SetLastError(ERROR_SUCCESS);
    created = CreateProcessW(childPath, (PWSTR)0, (void *)0, (void *)0, FALSE,
        0, (void *)0, (PCWSTR)0, &startup, &process);
    if (!created) {
        win32Error = GetLastError();
    } else {
        waitResult = WaitForSingleObject(process.hProcess, 3000);
        if (waitResult == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(process.hProcess, &exitCode)) win32Error = GetLastError();
        } else if (waitResult == WAIT_FAILED) {
            win32Error = GetLastError();
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    used = 0; report[0] = 0;
    AppendText(report, 1024, &used, L"probe=packaged-child-process\r\nchild_path=");
    AppendText(report, 1024, &used, childPath);
    AppendText(report, 1024, &used, L"\r\ncreate_process=");
    AppendText(report, 1024, &used, created ? L"success\r\n" : L"failure\r\n");
    AppendText(report, 1024, &used, L"win32_error="); AppendHex(report, 1024, &used, win32Error);
    AppendText(report, 1024, &used, L"\r\nwait_result="); AppendHex(report, 1024, &used, waitResult);
    AppendText(report, 1024, &used, L"\r\nexpected_exit_code="); AppendHex(report, 1024, &used, EXPECTED_CHILD_EXIT);
    AppendText(report, 1024, &used, L"\r\nchild_exit_code="); AppendHex(report, 1024, &used, exitCode);
    AppendText(report, 1024, &used, L"\r\nexit_code_matches=");
    AppendText(report, 1024, &used,
        (created && waitResult == WAIT_OBJECT_0 && exitCode == EXPECTED_CHILD_EXIT) ? L"yes\r\n" : L"no\r\n");
    hr = WriteReport(report);
    return hr;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand) {
    static const WCHAR coreClass[] = L"Windows.ApplicationModel.Core.CoreApplication";
    HSTRING className = (HSTRING)0;
    ICoreApplication *coreApp = (ICoreApplication *)0;
    HRESULT hr;
    (void)instance; (void)previous; (void)commandLine; (void)showCommand;

    g_view.iface.lpVtbl = &g_view_vtbl;
    g_view.refs = 1;
    g_source.iface.lpVtbl = &g_source_vtbl;
    g_source.refs = 1;

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) return (int)hr;
    hr = WindowsCreateString(coreClass, (DWORD)(sizeof(coreClass) / sizeof(coreClass[0]) - 1), &className);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(className, &IID_CORE_APPLICATION, (void **)&coreApp);
    if (SUCCEEDED(hr)) hr = coreApp->lpVtbl->Run(coreApp, &g_source.iface);
    if (coreApp) coreApp->lpVtbl->Release(coreApp);
    if (className) WindowsDeleteString(className);
    RoUninitialize();
    return (int)hr;
}
