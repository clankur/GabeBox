"""Enable Mesa surfaceless EGL in SDL's offscreen driver for opted-in launches."""

from pathlib import Path
import sys


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f'Unexpected pinned SDL source near {old!r}')
    return source.replace(old, new, 1)


def add_surfaceless(tree):
    offscreen = tree / 'src/video/offscreen'
    context = offscreen / 'SDL_offscreenopengles.c'
    source = context.read_text(encoding='utf-8')
    old = '#include "SDL_offscreenwindow.h"'
    source = replace_once(source, old, old + '\n#include "SDL_hints.h"\n#include "SDL_opengl.h"\n#include "../../sdl-startup-trace.h"')
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
    }''')
    old = '    return SDL_EGL_SwapBuffers(_this, offscreen_wind->egl_surface);'
    source = replace_once(source, old, '''    if (offscreen_wind->egl_surface == EGL_NO_SURFACE) {
        return SDL_SetError("Surfaceless OpenGL has no presentation surface");
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
    window.write_text(source, encoding='utf-8')


if __name__ == '__main__':
    add_surfaceless(Path(sys.argv[1]))
