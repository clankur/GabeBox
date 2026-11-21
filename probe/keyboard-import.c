#include <windows.h>

typedef UINT (WINAPI *MapKeyFn)(UINT, UINT, HKL);

// Expose the address solely to keep the regular PE import in this test DLL.
__declspec(dllexport) MapKeyFn ProbeKeyboardImport(void) {
#ifdef PROBE_WIDE_IMPORT
    return &MapVirtualKeyExW;
#else
    return &MapVirtualKeyExA;
#endif
}
