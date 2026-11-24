#include <windows.h>
#include <jni.h>
#include <array>
#include <cstring>
#include <deque>
#include <string>

namespace {
HMODULE sdl;
void *window;
void *gamepad;

struct MouseEvent {
    jint button, pressed, x, y, timestamp;
};
std::array<MouseEvent, 64> mouseEvents{};
size_t mouseEventHead, mouseEventCount;

void queueMouseEvent(MouseEvent event) {
    if (mouseEventCount == mouseEvents.size()) {
        mouseEventHead = (mouseEventHead + 1) % mouseEvents.size();
        --mouseEventCount;
    }
    mouseEvents[(mouseEventHead + mouseEventCount) % mouseEvents.size()] = event;
    ++mouseEventCount;
}

// SDL scancodes follow USB HID usage IDs. LWJGL 2 exposes PC set-1 key IDs.
int lwjglKey(int scan) {
    static constexpr unsigned char letters[] = {
        30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44
    };
    static constexpr unsigned char editing[] = {
        28,1,14,15,57,12,13,26,27,43,43,39,40,41,51,52,53,58
    };  // Return through Caps Lock.
    static constexpr unsigned char navigation[] = {
        183,70,197,210,199,201,211,207,209,205,203,208,200,69,181,55,74,78,156,
        79,80,81,75,76,77,71,72,73,82,83,43,221,222,141
    };  // Print Screen through keypad equals.
    static constexpr unsigned char modifiers[] = {29,42,56,219,157,54,184,220};
    if (scan >= 4 && scan <= 29) return letters[scan - 4];
    if (scan >= 30 && scan <= 39) return scan - 28;
    if (scan >= 40 && scan <= 57) return editing[scan - 40];
    if (scan >= 58 && scan <= 67) return scan + 1;  // F1-F10
    if (scan == 68 || scan == 69) return scan + 19;  // F11-F12
    if (scan >= 70 && scan <= 103) return navigation[scan - 70];
    if (scan >= 224 && scan <= 231) return modifiers[scan - 224];
    return 0;
}

struct KeyEvent { jint key, pressed, character, timestamp, repeat; };
std::deque<KeyEvent> keyboardEvents;
std::array<unsigned char, 256> keyboardState{};
bool keyboardActive;

void queueKey(KeyEvent event) {
    if (keyboardEvents.size() == 4096) keyboardEvents.pop_front();
    keyboardEvents.push_back(event);
}

void receiveKey(int scan, bool pressed, unsigned timestamp, int repeat) {
    const int key = lwjglKey(scan);
    if (!key) return;
    keyboardState[key] = pressed;
    int character = 0;
    if (pressed) {
        switch (scan) {
            case 40: case 88: character = 13; break;
            case 42: character = 8; break;
            case 43: character = 9; break;
            case 76: character = 127; break;
        }
    }
    queueKey({key, pressed, character, static_cast<jint>(timestamp), repeat});
}

void receiveText(const char (&text)[32], unsigned timestamp, bool followsKeyDown) {
    const auto end = static_cast<const char *>(std::memchr(text, 0, sizeof(text)));
    if (!end || end == text) return;
    wchar_t characters[32];
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(end - text),
                                    characters, 32);
    int first = 0;
    if (count && followsKeyDown && !keyboardEvents.empty() && !keyboardEvents.back().character) {
        keyboardEvents.back().character = characters[first++];
    }
    for (int i = first; i < count; ++i) {
        // KEY_NONE events carry composed text independently of keyboard layout.
        queueKey({0, 1, characters[i], static_cast<jint>(timestamp), 0});
    }
}

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
void *(*glAddress)(const char *);
int (*pollEvent)(void *);
void (*startTextInput)();
void (*stopTextInput)();
unsigned (*mouseState)(int *, int *);
void (*warpMouse)(void *, int, int);
void *(*createSurface)(void *, int, int, int, int, unsigned, unsigned, unsigned, unsigned);
void (*freeSurface)(void *);
void *(*createCursor)(void *, int, int);
void (*setCursor)(void *);
void *(*defaultCursor)();
void (*freeCursor)(void *);
int (*numJoysticks)();
int (*isGameController)(int);
void *(*openGameController)(int);
void (*closeGameController)(void *);
int (*gameControllerAttached)(void *);
short (*gameControllerAxis)(void *, int);
unsigned char (*gameControllerButton)(void *, int);

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
    glAddress = resolve<decltype(glAddress)>("SDL_GL_GetProcAddress");
    pollEvent = resolve<decltype(pollEvent)>("SDL_PollEvent");
    startTextInput = resolve<decltype(startTextInput)>("SDL_StartTextInput");
    stopTextInput = resolve<decltype(stopTextInput)>("SDL_StopTextInput");
    mouseState = resolve<decltype(mouseState)>("SDL_GetMouseState");
    warpMouse = resolve<decltype(warpMouse)>("SDL_WarpMouseInWindow");
    createSurface = resolve<decltype(createSurface)>("SDL_CreateRGBSurfaceFrom");
    freeSurface = resolve<decltype(freeSurface)>("SDL_FreeSurface");
    createCursor = resolve<decltype(createCursor)>("SDL_CreateColorCursor");
    setCursor = resolve<decltype(setCursor)>("SDL_SetCursor");
    defaultCursor = resolve<decltype(defaultCursor)>("SDL_GetDefaultCursor");
    freeCursor = resolve<decltype(freeCursor)>("SDL_FreeCursor");
    numJoysticks = resolve<decltype(numJoysticks)>("SDL_NumJoysticks");
    isGameController = resolve<decltype(isGameController)>("SDL_IsGameController");
    openGameController = resolve<decltype(openGameController)>("SDL_GameControllerOpen");
    closeGameController = resolve<decltype(closeGameController)>("SDL_GameControllerClose");
    gameControllerAttached = resolve<decltype(gameControllerAttached)>("SDL_GameControllerGetAttached");
    gameControllerAxis = resolve<decltype(gameControllerAxis)>("SDL_GameControllerGetAxis");
    gameControllerButton = resolve<decltype(gameControllerButton)>("SDL_GameControllerGetButton");
    if (mainReady && init && createWindow && destroyWindow && createContext && makeCurrent &&
        currentContext && deleteContext && swapWindow && setInterval && glAddress && pollEvent && startTextInput && stopTextInput && mouseState &&
        warpMouse && createSurface && freeSurface && createCursor && setCursor && defaultCursor && freeCursor &&
        numJoysticks && isGameController && openGameController && closeGameController && gameControllerAttached &&
        gameControllerAxis && gameControllerButton) return true;
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
    wchar_t controller[2]{};
    const bool hostedPad = GetEnvironmentVariableW(L"XBOX_SDL_HOST_GAMEPAD", controller, 2) == 1 && controller[0] == L'1';
    return init(hostedPad ? 0x2020 : 0x20) == 0 ? JNI_TRUE : JNI_FALSE;  // SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_create(JNIEnv *, jclass, jint width, jint height) {
    if (!window && width > 0 && height > 0 && width <= 1920 && height <= 1080) {
        window = createWindow("Slay the Spire", 0, 0, width, height, 0x0a);  // SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN
    }
    return window ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_destroy(JNIEnv *, jclass) {
    if (keyboardActive) stopTextInput();
    keyboardActive = false;
    keyboardEvents.clear();
    keyboardState.fill(0);
    if (window) destroyWindow(window);
    if (gamepad) closeGameController(gamepad);
    gamepad = nullptr;
    window = nullptr;
    mouseEventHead = mouseEventCount = 0;
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

JNIEXPORT jlong JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_lookupGl(JNIEnv *env, jclass, jstring name) {
    if (!currentContext()) return 0;
    auto chars = env->GetStringUTFChars(name, nullptr);
    if (!chars) return 0;
    auto address = glAddress(chars);
    env->ReleaseStringUTFChars(name, chars);
    return reinterpret_cast<jlong>(address);
}

JNIEXPORT jboolean JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_pollClose(JNIEnv *, jclass) {
    struct Motion { unsigned type, timestamp, windowId, which, state; int x, y; };
    struct Button { unsigned type, timestamp, windowId, which; unsigned char button, state, clicks, padding; int x, y; };
    struct Key { unsigned type, timestamp, windowId; unsigned char state, repeat, padding[2]; int scan; };
    struct Text { unsigned type, timestamp, windowId; char text[32]; };
    union alignas(void*) { unsigned type; Motion motion; Button button; Key key; Text text; unsigned char event[56]; } input{};
    bool followsKeyDown = false;
    while (pollEvent(&input) > 0) {
        if (input.type == 0x100) return JNI_TRUE;  // SDL_QUIT
        if (keyboardActive && (input.type == 0x300 || input.type == 0x301)) {
            receiveKey(input.key.scan, input.type == 0x300, input.key.timestamp, input.key.repeat);
        }
        if (keyboardActive && input.type == 0x303) receiveText(input.text.text, input.text.timestamp, followsKeyDown);
        followsKeyDown = keyboardActive && input.type == 0x300 && lwjglKey(input.key.scan) != 0;
        if (input.type == 0x400) {  // SDL_MOUSEMOTION
            const auto &motion = input.motion;
            queueMouseEvent({-1, 0, motion.x, motion.y, static_cast<jint>(motion.timestamp)});
        }
        if (input.type == 0x401 || input.type == 0x402) {  // SDL_MOUSEBUTTONDOWN or SDL_MOUSEBUTTONUP
            const auto &button = input.button;
            constexpr jint lwjglButtons[] = {0, 2, 1};  // SDL left, middle, right.
            if (button.button >= 1 && button.button <= 3) {
                queueMouseEvent({lwjglButtons[button.button - 1], button.state, button.x, button.y,
                                 static_cast<jint>(button.timestamp)});
            }
        }
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_setKeyboardActive(JNIEnv *, jclass, jboolean active) {
    if (!window || keyboardActive == (active != JNI_FALSE)) return;
    keyboardActive = active != JNI_FALSE;
    keyboardEvents.clear();
    keyboardState.fill(0);
    if (keyboardActive) startTextInput(); else stopTextInput();
}

JNIEXPORT void JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_pollKeyboard(JNIEnv *env, jobject, jobject buffer) {
    void *output = env->GetDirectBufferAddress(buffer);
    if (output && env->GetDirectBufferCapacity(buffer) >= 256)
        std::memcpy(output, keyboardState.data(), keyboardState.size());
}

JNIEXPORT jint JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_readKeyEvents(
    JNIEnv *env, jclass, jobject events, jint requested) {
    auto output = static_cast<jint*>(env->GetDirectBufferAddress(events));
    if (!output || requested <= 0 || env->GetDirectBufferCapacity(events) / 5 < requested) return 0;
    jint count = 0;
    while (count < requested && !keyboardEvents.empty()) {
        const auto event = keyboardEvents.front();
        keyboardEvents.pop_front();
        *output++ = event.key;
        *output++ = event.pressed;
        *output++ = event.character;
        *output++ = event.timestamp;
        *output++ = event.repeat;
        ++count;
    }
    return count;
}

JNIEXPORT jint JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_readMouseEvents(
    JNIEnv *env, jclass, jobject events, jint requested) {
    auto output = static_cast<jint*>(env->GetDirectBufferAddress(events));
    if (!output || requested <= 0 || env->GetDirectBufferCapacity(events) / 5 < requested) return 0;
    jint count = 0;
    while (count < requested && mouseEventCount) {
        const auto &event = mouseEvents[mouseEventHead];
        *output++ = event.button;
        *output++ = event.pressed;
        *output++ = event.x;
        *output++ = event.y;
        *output++ = event.timestamp;
        mouseEventHead = (mouseEventHead + 1) % mouseEvents.size();
        --mouseEventCount;
        ++count;
    }
    return count;
}

JNIEXPORT jint JNICALL Java_org_lwjgl_opengl_XboxSdlDisplay_mouse(JNIEnv *env, jclass, jobject coordinates) {
    auto point = static_cast<int*>(env->GetDirectBufferAddress(coordinates));
    return point && env->GetDirectBufferCapacity(coordinates) >= 8 ? mouseState(point, point + 1) : 0;
}

JNIEXPORT jint JNICALL Java_org_lwjgl_opengl_XboxSdlGamepad_readNative(JNIEnv *env, jclass, jobject axes) {
    auto values = static_cast<jint*>(env->GetDirectBufferAddress(axes));
    if (!window || !values || env->GetDirectBufferCapacity(axes) < 6) return -1;
    if (gamepad && !gameControllerAttached(gamepad)) {
        closeGameController(gamepad);
        gamepad = nullptr;
    }
    for (int index = 0; !gamepad && index < numJoysticks(); ++index) {
        if (isGameController(index)) gamepad = openGameController(index);
    }
    if (!gamepad) return -1;
    jint mask = 0;
    for (int button = 0; button < 15; ++button) mask |= gameControllerButton(gamepad, button) << button;
    for (int axis = 0; axis < 6; ++axis) values[axis] = gameControllerAxis(gamepad, axis);
    return mask;
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
