#include <windows.h>
#include <jni.h>

JNIEXPORT jint JNICALL Java_SlayConsoleDiagnostic_setGameDirectory(JNIEnv *env, jclass type, jstring directory) {
    (void)type;
    if (!directory) return ERROR_INVALID_PARAMETER;

    jsize length = (*env)->GetStringLength(env, directory);
    if (length <= 0 || length >= 32767) return ERROR_INVALID_NAME;
    HANDLE heap = GetProcessHeap();
    WCHAR *path = HeapAlloc(heap, 0, ((SIZE_T)length + 1) * sizeof(*path));
    if (!path) return ERROR_NOT_ENOUGH_MEMORY;

    (*env)->GetStringRegion(env, directory, 0, length, (jchar *)path);
    path[length] = L'\0';
    DWORD result = ERROR_INVALID_NAME;
    jsize end = 0;
    if (!(*env)->ExceptionCheck(env)) {
        while (path[end]) ++end;
    }
    if (length == end) {
        result = SetCurrentDirectoryW(path) ? ERROR_SUCCESS : GetLastError();
    }
    HeapFree(heap, 0, path);
    return (jint)result;
}
