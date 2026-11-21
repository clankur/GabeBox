#include <windows.h>
#include <jni.h>
#include <string>

namespace {
HMODULE sdl;
void *window;

template<class Function>
Function resolve(const char *name) {
    return reinterpret_cast<Function>(GetProcAddress(sdl, name));
}
void (*mainReady)();
int (*init)(unsigned);
void *(*createWindow)(const char *, int, int, int, int, unsigned);
void (*destroyWindow)(void *);
void *(*createContext)(void *);
int (*makeCurrent)(void *, void *);
void *(*currentContext)();
void (*deleteContext)(void *);
void (*swapWindow)(void *);
int (*setInterval)(int);
int (*pollEvent)(void *);
unsigned (*mouseState)(int *, int *);
void (*warpMouse)(void *, int, int);
void *(*createSurface)(void *, int, int, int, int, unsigned, unsigned, unsigned, unsigned);
void (*freeSurface)(void *);
void *(*createCursor)(void *, int, int);
void (*setCursor)(void *);
void *(*defaultCursor)();
void (*freeCursor)(void *);

bool loadSdl() {
    if (sdl) return true;
    sdl = LoadPackagedLibrary(L"SDL2.dll", 0);
    if (!sdl) return false;
    mainReady = resolve<decltype(mainReady)>("SDL_SetMainReady");
    init = resolve<decltype(init)>("SDL_InitSubSystem");
    createWindow = resolve<decltype(createWindow)>("SDL_CreateWindow");
    destroyWindow = resolve<decltype(destroyWindow)>("SDL_DestroyWindow");
    createContext = resolve<decltype(createContext)>("SDL_GL_CreateContext");
    makeCurrent = resolve<decltype(makeCurrent)>("SDL_GL_MakeCurrent");
    currentContext = resolve<decltype(currentContext)>("SDL_GL_GetCurrentContext");
    deleteContext = resolve<decltype(deleteContext)>("SDL_GL_DeleteContext");
    swapWindow = resolve<decltype(swapWindow)>("SDL_GL_SwapWindow");
    setInterval = resolve<decltype(setInterval)>("SDL_GL_SetSwapInterval");
    pollEvent = resolve<decltype(pollEvent)>("SDL_PollEvent");
    mouseState = resolve<decltype(mouseState)>("SDL_GetMouseState");
    warpMouse = resolve<decltype(warpMouse)>("SDL_WarpMouseInWindow");
    createSurface = resolve<decltype(createSurface)>("SDL_CreateRGBSurfaceFrom");
    freeSurface = resolve<decltype(freeSurface)>("SDL_FreeSurface");
    createCursor = resolve<decltype(createCursor)>("SDL_CreateColorCursor");
    setCursor = resolve<decltype(setCursor)>("SDL_SetCursor");
    defaultCursor = resolve<decltype(defaultCursor)>("SDL_GetDefaultCursor");
    freeCursor = resolve<decltype(freeCursor)>("SDL_FreeCursor");
    if (mainReady && init && createWindow && destroyWindow && createContext && makeCurrent &&
        currentContext && deleteContext && swapWindow && setInterval && pollEvent && mouseState &&
        warpMouse && createSurface && freeSurface && createCursor && setCursor && defaultCursor && freeCursor) return true;
    FreeLibrary(sdl);
    sdl = nullptr;
    return false;
}
}

extern "C" {
JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_open(JNIEnv *env, jclass, jstring directory) {
    const auto length = env->GetStringLength(directory);
    if (length <= 0 || length >= 1000) return JNI_FALSE;
    wchar_t root[1024]{};
    env->GetStringRegion(directory, 0, length, reinterpret_cast<jchar *>(root));
    if (env->ExceptionCheck()) return JNI_FALSE;
    const auto egl = std::wstring(root) + L"\\libEGL.dll";
    const auto gl = std::wstring(root) + L"\\opengl32.dll";
    if (!SetEnvironmentVariableW(L"SDL_VIDEODRIVER", L"offscreen") ||
        !SetEnvironmentVariableW(L"SDL_VIDEO_GL_DRIVER", gl.c_str()) ||
        !SetEnvironmentVariableW(L"SDL_VIDEO_EGL_DRIVER", egl.c_str()) ||
        !SetEnvironmentVariableW(L"SDL_VIDEO_WIN_D3DCOMPILER", L"none") ||
        !SetEnvironmentVariableW(L"XBOX_SDL_SURFACELESS", L"1") ||
        !loadSdl()) return JNI_FALSE;
    mainReady();
    return init(0x20) == 0 ? JNI_TRUE : JNI_FALSE;  // SDL_INIT_VIDEO
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_create(JNIEnv *, jclass, jint width, jint height) {
    if (!window && width > 0 && height > 0 && width <= 1920 && height <= 1080) {
        window = createWindow("Slay the Spire", 0, 0, width, height, 0x0a);  // SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN
    }
    return window ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_destroy(JNIEnv *, jclass) {
    if (window) destroyWindow(window);
    window = nullptr;
}

JNIEXPORT jobject JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_openContext(JNIEnv *env, jclass) {
    auto context = window ? createContext(window) : nullptr;
    if (!context) return nullptr;
    auto handle = env->NewDirectByteBuffer(context, 1);
    if (!handle) deleteContext(context);
    return handle;
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_makeContextCurrent(JNIEnv *env, jclass, jobject handle) {
    auto context = handle ? env->GetDirectBufferAddress(handle) : nullptr;
    return makeCurrent(context ? window : nullptr, context) == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_contextIsCurrent(JNIEnv *env, jclass, jobject handle) {
    return handle && currentContext() == env->GetDirectBufferAddress(handle) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_closeContext(JNIEnv *env, jclass, jobject handle) {
    if (handle) deleteContext(env->GetDirectBufferAddress(handle));
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_present(JNIEnv *, jclass) {
    if (!window || !currentContext()) return JNI_FALSE;
    swapWindow(window);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_swapInterval(JNIEnv *, jclass, jint interval) {
    if (currentContext()) setInterval(interval);
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_pollClose(JNIEnv *, jclass) {
    union alignas(void*) { unsigned type; unsigned char event[56]; } input{};  // SDL2_Event, x64
    while (pollEvent(&input) > 0) {
        if (input.type == 0x100) return JNI_TRUE;  // SDL_QUIT
    }
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_mouse(JNIEnv *env, jclass, jobject coordinates) {
    auto point = static_cast<int*>(env->GetDirectBufferAddress(coordinates));
    return point && env->GetDirectBufferCapacity(coordinates) >= 8 ? mouseState(point, point + 1) : 0;
}

JNIEXPORT jobject JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_makeCursor(
    JNIEnv *env, jclass, jint width, jint height, jint hotX, jint hotY, jobject pixels) {
    if (!window || !pixels || width <= 0 || height <= 0 || width > 256 || height > 256 ||
        env->GetDirectBufferCapacity(pixels) < width * height) return nullptr;
    auto data = env->GetDirectBufferAddress(pixels);
    if (!data) return nullptr;
    // LWJGL's Windows Cursor adapter supplies top-down ARGB pixels and hotspot.
    auto surface = createSurface(data, width, height, 32, width * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    if (!surface) return nullptr;
    auto cursor = createCursor(surface, hotX, hotY);
    freeSurface(surface);
    if (!cursor) return nullptr;
    auto handle = env->NewDirectByteBuffer(cursor, 1);
    if (!handle) freeCursor(cursor);
    return handle;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_selectCursor(JNIEnv *env, jclass, jobject handle) {
    if (window) setCursor(handle ? env->GetDirectBufferAddress(handle) : defaultCursor());
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_releaseCursor(JNIEnv *env, jclass, jobject handle) {
    if (handle) freeCursor(env->GetDirectBufferAddress(handle));
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_warpCursor(JNIEnv *, jclass, jint x, jint y) {
    if (window) warpMouse(window, x, y);
}
}
