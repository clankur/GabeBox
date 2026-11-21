#pragma once

#include "SDL_opengl.h"
#include "SDL_surface.h"
#include "SDL_atomic.h"
#include "sdl-gl-trace.h"
#include "sdl-offscreen-cursor.h"

/* This is a back buffer for one SDL window and its own GL context. */
static int XboxSdlCreateFramebuffer(_THIS, OFFSCREEN_Window *offscreen, SDL_GLContext context)
{
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers = SDL_EGL_GetProcAddress(_this, "glGenFramebuffers");
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = SDL_EGL_GetProcAddress(_this, "glBindFramebuffer");
    PFNGLGENRENDERBUFFERSPROC gen_renderbuffers = SDL_EGL_GetProcAddress(_this, "glGenRenderbuffers");
    PFNGLBINDRENDERBUFFERPROC bind_renderbuffer = SDL_EGL_GetProcAddress(_this, "glBindRenderbuffer");
    PFNGLRENDERBUFFERSTORAGEPROC storage = SDL_EGL_GetProcAddress(_this, "glRenderbufferStorage");
    PFNGLFRAMEBUFFERRENDERBUFFERPROC attach = SDL_EGL_GetProcAddress(_this, "glFramebufferRenderbuffer");
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check = SDL_EGL_GetProcAddress(_this, "glCheckFramebufferStatus");
    SDL_Window *window = offscreen->sdl_window;
    GLuint color = 0, depth_stencil = 0, framebuffer = 0;
    if (!gen_framebuffers || !bind_framebuffer || !gen_renderbuffers || !bind_renderbuffer || !storage || !attach || !check ||
        window->w <= 0 || window->h <= 0 || window->w > 4096 || window->h > 4096) {
        return SDL_SetError("Unsupported surfaceless framebuffer configuration");
    }

    gen_framebuffers(1, &framebuffer);
    gen_renderbuffers(1, &color);
    gen_renderbuffers(1, &depth_stencil);
    if (!framebuffer || !color || !depth_stencil) {
        return SDL_SetError("Mesa could not allocate surfaceless buffers");
    }
    bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
    bind_renderbuffer(GL_RENDERBUFFER, color);
    storage(GL_RENDERBUFFER, GL_RGBA8, window->w, window->h);
    attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    bind_renderbuffer(GL_RENDERBUFFER, depth_stencil);
    storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, window->w, window->h);
    attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
    bind_renderbuffer(GL_RENDERBUFFER, 0);
    if (check(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return SDL_SetError("Mesa surfaceless framebuffer is incomplete");
    }

    offscreen->xbox_context = context;
    offscreen->xbox_framebuffer = framebuffer;
    offscreen->xbox_color = color;
    offscreen->xbox_depth_stencil = depth_stencil;
    offscreen->xbox_width = window->w;
    offscreen->xbox_height = window->h;
    offscreen->xbox_capture_attempted = SDL_FALSE;
    offscreen->xbox_first_swap = 0;
    return 0;
}

static void XboxSdlResizeFramebuffer(_THIS, OFFSCREEN_Window *offscreen)
{
    SDL_Window *window = offscreen->sdl_window;
    if ((window->w == offscreen->xbox_width && window->h == offscreen->xbox_height) ||
        window->w <= 0 || window->h <= 0 || window->w > 4096 || window->h > 4096 ||
        !offscreen->xbox_context || offscreen->xbox_context != SDL_GL_GetCurrentContext()) return;

    typedef void (APIENTRY *GetInteger)(GLenum, GLint *);
    GetInteger get_integer = SDL_EGL_GetProcAddress(_this, "glGetIntegerv");
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = SDL_EGL_GetProcAddress(_this, "glBindFramebuffer");
    PFNGLBINDRENDERBUFFERPROC bind_renderbuffer = SDL_EGL_GetProcAddress(_this, "glBindRenderbuffer");
    PFNGLRENDERBUFFERSTORAGEPROC storage = SDL_EGL_GetProcAddress(_this, "glRenderbufferStorage");
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check = SDL_EGL_GetProcAddress(_this, "glCheckFramebufferStatus");
    if (!get_integer || !bind_framebuffer || !bind_renderbuffer || !storage || !check) return;

    GLint old_renderbuffer, old_draw;
    get_integer(GL_RENDERBUFFER_BINDING, &old_renderbuffer);
    get_integer(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    bind_renderbuffer(GL_RENDERBUFFER, offscreen->xbox_color);
    storage(GL_RENDERBUFFER, GL_RGBA8, window->w, window->h);
    bind_renderbuffer(GL_RENDERBUFFER, offscreen->xbox_depth_stencil);
    storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, window->w, window->h);
    bind_framebuffer(GL_DRAW_FRAMEBUFFER, offscreen->xbox_framebuffer);
    if (check(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        offscreen->xbox_width = window->w;
        offscreen->xbox_height = window->h;
        XboxSdlTrace("sdl_surfaceless_resize=ready\n");
    } else {
        storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, offscreen->xbox_width, offscreen->xbox_height);
        bind_renderbuffer(GL_RENDERBUFFER, offscreen->xbox_color);
        storage(GL_RENDERBUFFER, GL_RGBA8, offscreen->xbox_width, offscreen->xbox_height);
        XboxSdlTrace("sdl_surfaceless_resize=error\n");
    }
    bind_framebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
    bind_renderbuffer(GL_RENDERBUFFER, old_renderbuffer);
}

static void APIENTRY XboxSdlBindFramebuffer(GLenum target, GLuint framebuffer)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    SDL_Window *window = SDL_GL_GetCurrentWindow();
    OFFSCREEN_Window *offscreen = window ? window->driverdata : NULL;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = device ? SDL_EGL_GetProcAddress(device, "glBindFramebuffer") : NULL;
    if (bind_framebuffer) {
        if (!framebuffer && offscreen && offscreen->xbox_context == SDL_GL_GetCurrentContext()) {
            framebuffer = offscreen->xbox_framebuffer;
        }
        bind_framebuffer(target, framebuffer);
    }
}

static void *XboxSdlGetProcAddress(_THIS, const char *name)
{
    void *function = SDL_EGL_GetProcAddress(_this, name);
    if (function && SDL_GetHintBoolean("XBOX_SDL_TRACE_GL", SDL_FALSE)) {
        void *traced = XboxSdlTracedGlFunction(name);
        if (traced) return traced;
    }
    if (_this->gl_allow_no_surface && SDL_GetHintBoolean("XBOX_SDL_SURFACELESS", SDL_FALSE) && function &&
        (SDL_strcmp(name, "glBindFramebuffer") == 0 || SDL_strcmp(name, "glBindFramebufferEXT") == 0)) {
        return (void *)XboxSdlBindFramebuffer;
    }
    return function;
}

extern Uint8 *XboxSdlBeginHostFrame(SDL_Window *window, int width, int height);
extern void XboxSdlEndHostFrame(SDL_bool success);

static SDL_bool XboxSdlReadFrame(_THIS, OFFSCREEN_Window *offscreen, Uint8 *pixels)
{
    typedef void (APIENTRY *GetInteger)(GLenum, GLint *);
    typedef void (APIENTRY *PixelStore)(GLenum, GLint);
    typedef void (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GetInteger get_integer = SDL_EGL_GetProcAddress(_this, "glGetIntegerv");
    PixelStore pixel_store = SDL_EGL_GetProcAddress(_this, "glPixelStorei");
    ReadPixels read_pixels = SDL_EGL_GetProcAddress(_this, "glReadPixels");
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = SDL_EGL_GetProcAddress(_this, "glBindFramebuffer");
    PFNGLBINDBUFFERPROC bind_buffer = SDL_EGL_GetProcAddress(_this, "glBindBuffer");
    GLint old_read, old_pack_buffer;
    const GLenum pack_options[] = { GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS };
    GLint old_pack[SDL_arraysize(pack_options)];
    int pitch = offscreen->xbox_width * 4;
    SDL_Surface *surface;
    if (!get_integer || !pixel_store || !read_pixels || !bind_framebuffer || !bind_buffer) return SDL_FALSE;
    SDL_memset(pixels, 0xCD, 4);
    get_integer(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    get_integer(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    bind_framebuffer(GL_READ_FRAMEBUFFER, offscreen->xbox_framebuffer);
    bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    for (int i = 0; i < SDL_arraysize(pack_options); ++i) {
        get_integer(pack_options[i], &old_pack[i]);
        pixel_store(pack_options[i], i == 0 ? 4 : 0);
    }
    read_pixels(0, 0, offscreen->xbox_width, offscreen->xbox_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int i = 0; i < SDL_arraysize(pack_options); ++i) {
        pixel_store(pack_options[i], old_pack[i]);
    }
    bind_buffer(GL_PIXEL_PACK_BUFFER, old_pack_buffer);
    bind_framebuffer(GL_READ_FRAMEBUFFER, old_read);

    if (pixels[0] == 0xCD && pixels[1] == 0xCD && pixels[2] == 0xCD && pixels[3] == 0xCD) return SDL_FALSE;

    Uint8 *scratch = SDL_stack_alloc(Uint8, pitch);
    if (!scratch) return SDL_FALSE;
    for (int y = 0; y < offscreen->xbox_height / 2; ++y) {
        Uint8 *top = pixels + (size_t)y * pitch;
        Uint8 *bottom = pixels + (size_t)(offscreen->xbox_height - y - 1) * pitch;
        SDL_memcpy(scratch, top, pitch);
        SDL_memcpy(top, bottom, pitch);
        SDL_memcpy(bottom, scratch, pitch);
    }
    SDL_stack_free(scratch);
    surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, offscreen->xbox_width, offscreen->xbox_height,
                                                 32, pitch, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        XboxSdlBlitOffscreenCursor(offscreen->sdl_window, surface);
        SDL_FreeSurface(surface);
    }
    return SDL_TRUE;
}

static void XboxSdlPresentHostFrame(_THIS, OFFSCREEN_Window *offscreen)
{
    if (offscreen->xbox_context != SDL_GL_GetCurrentContext()) return;
    Uint8 *pixels = XboxSdlBeginHostFrame(offscreen->sdl_window, offscreen->xbox_width, offscreen->xbox_height);
    if (pixels) XboxSdlEndHostFrame(XboxSdlReadFrame(_this, offscreen, pixels));
}

static void XboxSdlCaptureFrame(_THIS, OFFSCREEN_Window *offscreen)
{
    if (offscreen->xbox_capture_attempted || offscreen->xbox_context != SDL_GL_GetCurrentContext()) return;
    if (!offscreen->xbox_first_swap) offscreen->xbox_first_swap = offscreen->xbox_last_swap;
    const char *delay_hint = SDL_GetHint("XBOX_SDL_OPENGL_FRAME_DELAY_MS");
    int delay = delay_hint ? SDL_clamp(SDL_atoi(delay_hint), 0, 15000) : 0;
    if (offscreen->xbox_last_swap - offscreen->xbox_first_swap < (Uint64)delay) return;
    offscreen->xbox_capture_attempted = SDL_TRUE;

    char path[1024];
    const char *trace = SDL_getenv("XBOX_SDL_TRACE_FILE");
    size_t path_length = trace ? SDL_strlen(trace) : 0;
    if (path_length <= 4 || path_length >= sizeof(path) || SDL_strcmp(trace + path_length - 4, ".txt") != 0) return;
    SDL_strlcpy(path, trace, sizeof(path));
    SDL_strlcpy(path + path_length - 4, ".bmp", 5);
    int pitch = offscreen->xbox_width * 4;
    Uint8 *pixels = SDL_malloc((size_t)offscreen->xbox_height * pitch);
    SDL_Surface *surface = NULL;
    if (pixels && XboxSdlReadFrame(_this, offscreen, pixels)) {
        surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, offscreen->xbox_width, offscreen->xbox_height,
                                                     32, pitch, SDL_PIXELFORMAT_RGBA32);
    }
    XboxSdlTrace(surface && SDL_SaveBMP(surface, path) == 0 ? "sdl_offscreen_frame=ready\n" : "sdl_offscreen_frame=error\n");
    SDL_FreeSurface(surface);
    SDL_free(pixels);
}
