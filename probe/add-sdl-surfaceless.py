"""Enable Mesa surfaceless EGL in SDL's offscreen driver for opted-in launches."""

from pathlib import Path
import shutil
import sys


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f'Unexpected pinned SDL source near {old!r}')
    return source.replace(old, new, 1)


def add_surfaceless(tree):
    offscreen = tree / 'src/video/offscreen'
    for filename in ('sdl-surfaceless-framebuffer.h', 'sdl-gl-trace.h', 'sdl-offscreen-cursor.h',
                     'sdl-gpu-producer.cpp', 'sdl-gpu-protocol.h', 'mesa-glinterop.h', 'sdl-swap-clock.h'):
        shutil.copyfile(Path(__file__).with_name(filename), offscreen / filename)

    cmake = tree / 'CMakeLists.txt'
    source = cmake.read_text(encoding='utf-8')
    old = '    list(APPEND SOURCE_FILES ${VIDEO_OFFSCREEN_SOURCES})'
    source = replace_once(source, old, old + '''
    if(WIN32)
      set(SDL_GPU_SOURCE ${SDL2_SOURCE_DIR}/src/video/offscreen/sdl-gpu-producer.cpp)
      list(APPEND SOURCE_FILES ${SDL_GPU_SOURCE})
      if(MSVC)
        set_source_files_properties(${SDL_GPU_SOURCE} PROPERTIES COMPILE_FLAGS "/EHs-c- /GR-")
      endif()
    endif()''')
    cmake.write_text(source, encoding='utf-8')

    video = offscreen / 'SDL_offscreenvideo.c'
    source = video.read_text(encoding='utf-8')
    old = '#include "SDL_offscreenvideo.h"'
    source = replace_once(source, old, old + '\n#include "SDL_hints.h"\n#include "sdl-offscreen-cursor.h"')
    old = '    device->GL_SwapWindow = OFFSCREEN_GLES_SwapWindow;'
    source = replace_once(source, old, '    device->SetWindowSize = OFFSCREEN_GLES_SetWindowSize;\n' + old)
    old = '    mode.h = 768;'
    source = replace_once(source, old, old + '''
    if (SDL_GetHintBoolean("XBOX_SDL_HOST_GAMEPAD", SDL_FALSE)) {
        mode.w = 1280;
        mode.h = 720;
    }''')
    old = '    /* We\'re done! */'
    source = replace_once(source, old, '''    if (SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE)) {
        XboxSdlInitOffscreenCursor();
    }

''' + old)
    video.write_text(source, encoding='utf-8')

    events = offscreen / 'SDL_offscreenevents.c'
    source = events.read_text(encoding='utf-8')
    old = '#include "SDL_offscreenevents_c.h"'
    source = replace_once(source, old, old + '\n\nextern void XboxSdlPollHostInput(void);')
    old = 'void OFFSCREEN_PumpEvents(_THIS)\n{'
    source = replace_once(source, old, old + '\n    XboxSdlPollHostInput();')
    events.write_text(source, encoding='utf-8')

    main = tree / 'src/SDL.c'
    source = main.read_text(encoding='utf-8')
    old = '        if (SDL_HelperWindowCreate() < 0) {'
    source = replace_once(source, old, '''        const char *driver = SDL_GetHint(SDL_HINT_VIDEODRIVER);
        const SDL_bool offscreen = driver && SDL_strcmp(driver, "offscreen") == 0 &&
            SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE);
        if (offscreen) {
            XboxSdlTrace("sdl_offscreen_helper_window=skipped\\n");
        } else if (SDL_HelperWindowCreate() < 0) {''')
    old = '    /* Clear the error message */'
    source = replace_once(source, old, '''    if (SDL_GetHintBoolean("XBOX_SDL_HOST_GAMEPAD", SDL_FALSE)) {
        /* The host supplies the controller. Avoid desktop discovery and haptics. */
        flags &= ~SDL_INIT_HAPTIC;
        SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

''' + old)
    main.write_text(source, encoding='utf-8')

    window_header = offscreen / 'SDL_offscreenwindow.h'
    source = window_header.read_text(encoding='utf-8')
    source = '#include "sdl-swap-clock.h"\n' + source
    old = '    EGLSurface egl_surface;'
    source = replace_once(source, old, old + '''
    SDL_GLContext xbox_context;
    unsigned xbox_framebuffer;
    unsigned xbox_color, xbox_depth_stencil;
    void *xbox_get_integer, *xbox_pixel_store, *xbox_read_pixels;
    void *xbox_bind_framebuffer, *xbox_bind_buffer;
    void *xbox_gpu;
    int xbox_gpu_retry;
    Uint64 xbox_gpu_retry_after;
    SDL_bool xbox_pack_invert;
    unsigned xbox_readback_samples;
    Uint64 xbox_readback_ticks;
    int xbox_width, xbox_height;
    XboxSdlSwapClock xbox_swap_clock;
    Uint64 xbox_last_swap;
    Uint64 xbox_first_swap;
    SDL_bool xbox_capture_attempted;''')
    window_header.write_text(source, encoding='utf-8')

    context_header = offscreen / 'SDL_offscreenopengles.h'
    source = context_header.read_text(encoding='utf-8')
    source = replace_once(source, '#define OFFSCREEN_GLES_GetProcAddress  SDL_EGL_GetProcAddress',
                          'extern void *OFFSCREEN_GLES_GetProcAddress(_THIS, const char *name);')
    source = replace_once(source, '#define OFFSCREEN_GLES_SetSwapInterval SDL_EGL_SetSwapInterval',
                          'extern int OFFSCREEN_GLES_SetSwapInterval(_THIS, int interval);')
    old = 'extern int OFFSCREEN_GLES_SetSwapInterval(_THIS, int interval);'
    source = replace_once(source, old, old + '\nextern void OFFSCREEN_GLES_SetWindowSize(_THIS, SDL_Window *window);')
    context_header.write_text(source, encoding='utf-8')

    context = offscreen / 'SDL_offscreenopengles.c'
    source = context.read_text(encoding='utf-8')
    old = '#include "SDL_offscreenwindow.h"'
    source = replace_once(source, old, old + '\n#include "SDL_hints.h"\n#include "../../sdl-startup-trace.h"'
                          '\n#include "SDL_timer.h"\n#include "sdl-surfaceless-framebuffer.h"')
    old = '/* EGL implementation of SDL OpenGL support */'
    source = replace_once(source, old, old + '''

void *OFFSCREEN_GLES_GetProcAddress(_THIS, const char *name)
{
    return XboxSdlGetProcAddress(_this, name);
}

void OFFSCREEN_GLES_SetWindowSize(_THIS, SDL_Window *window)
{
    OFFSCREEN_Window *offscreen = window->driverdata;
    if (offscreen && offscreen->xbox_context && offscreen->xbox_context == SDL_GL_GetCurrentContext()) {
        XboxSdlResizeFramebuffer(_this, offscreen);
    }
}

int OFFSCREEN_GLES_SetSwapInterval(_THIS, int interval)
{
    if (_this->egl_data && _this->gl_allow_no_surface && SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE)) {
        if (interval < 0 || interval > 4) {
            return SDL_SetError("Unsupported offscreen swap interval");
        }
        char value[40];
        SDL_snprintf(value, sizeof(value), "sdl_swap_interval=%d\\n", interval);
        XboxSdlTrace(value);
        _this->egl_data->egl_swapinterval = interval;
        return 0;
    }
    return SDL_EGL_SetSwapInterval(_this, interval);
}''')
    old = '    int ret = SDL_EGL_LoadLibraryOnly(_this, path);'
    source = replace_once(source, old, '''    int ret;
    if (SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE)) {
        XboxSdlTrace("sdl_surfaceless_egl=begin\\n");
        ret = SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY, 0);
        if (ret == 0) {
            if (SDL_EGL_HasExtension(_this, SDL_EGL_DISPLAY_EXTENSION, "EGL_KHR_surfaceless_context")) {
                _this->egl_data->is_offscreen = SDL_TRUE;
                ret = SDL_EGL_ChooseConfig(_this);
                _this->gl_allow_no_surface = ret == 0;
            } else {
                ret = SDL_SetError("Mesa is missing EGL_KHR_surfaceless_context");
            }
        }
        if (ret != 0) {
            SDL_EGL_UnloadLibrary(_this);
        }
        XboxSdlTrace(ret == 0 ? "sdl_surfaceless_egl=ready\\n" : "sdl_surfaceless_egl=error\\n");
        return ret;
    }
    ret = SDL_EGL_LoadLibraryOnly(_this, path);''')
    old = '    context = SDL_EGL_CreateContext(_this, offscreen_window->egl_surface);'
    source = replace_once(source, old, old + '''
    if (context && offscreen_window->egl_surface == EGL_NO_SURFACE) {
        typedef const GLubyte *(APIENTRY *GetString)(GLenum name);
        GetString get_string = (GetString)SDL_EGL_GetProcAddress(_this, "glGetString");
        if (!get_string || !get_string(GL_VERSION)) {
            SDL_EGL_DeleteContext(_this, context);
            SDL_SetError("Mesa did not make a surfaceless OpenGL context current");
            return NULL;
        }
        XboxSdlTrace("sdl_surfaceless_context=ready\\n");
        if (XboxSdlCreateFramebuffer(_this, offscreen_window, context) < 0) {
            XboxSdlTrace("sdl_surfaceless_framebuffer=error\\n");
            SDL_EGL_DeleteContext(_this, context);
            return NULL;
        }
        XboxSdlTrace("sdl_surfaceless_framebuffer=ready\\n");
    }''')
    old = '    return SDL_EGL_SwapBuffers(_this, offscreen_wind->egl_surface);'
    source = replace_once(source, old, '''    if (offscreen_wind->egl_surface == EGL_NO_SURFACE) {
        int interval = _this->egl_data->egl_swapinterval;
        XboxSdlSwapClock *clock = &offscreen_wind->xbox_swap_clock;
        if (interval > 0) {
            if (!clock->frequency) clock->frequency = SDL_GetPerformanceFrequency();
            Uint64 now = SDL_GetPerformanceCounter();
            Uint64 deadline = XboxSdlNextSwap(clock, now, interval);
            while (now < deadline) {
                Uint64 remaining = deadline - now;
                SDL_Delay((Uint32)((remaining * 1000 + clock->frequency - 1) / clock->frequency));
                now = SDL_GetPerformanceCounter();
            }
        } else {
            clock->interval = 0;
        }
        offscreen_wind->xbox_last_swap = SDL_GetTicks64();
        XboxSdlResizeFramebuffer(_this, offscreen_wind);
        XboxSdlPresentHostFrame(offscreen_wind);
        XboxSdlCaptureFrame(offscreen_wind);
        return 0;
    }
''' + old)
    context.write_text(source, encoding='utf-8')

    window = offscreen / 'SDL_offscreenwindow.c'
    source = window.read_text(encoding='utf-8')
    old = '        offscreen_window->egl_surface = SDL_EGL_CreateOffscreenSurface'
    source = replace_once(source, old, '''        if (SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE) && _this->gl_allow_no_surface) {
            offscreen_window->egl_surface = EGL_NO_SURFACE;
            return 0;
        }

''' + old)
    old = '#include "../SDL_egl_c.h"'
    source = replace_once(source, old, old + '\n#include "SDL_hints.h"')
    old = '        SDL_EGL_DestroySurface(_this, offscreen_window->egl_surface);'
    source = replace_once(source, old, '''#ifdef _WIN32
        extern void XboxSdlDestroyGpu(void *state);
        XboxSdlDestroyGpu(offscreen_window->xbox_gpu);
#endif
''' + old)
    window.write_text(source, encoding='utf-8')


if __name__ == '__main__':
    add_surfaceless(Path(sys.argv[1]))
