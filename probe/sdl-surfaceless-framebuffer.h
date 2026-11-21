#pragma once

#include "SDL_opengl.h"
#include "SDL_surface.h"
#include "SDL_atomic.h"
#include "sdl-gl-trace.h"
#include "sdl-offscreen-cursor.h"

#ifdef _WIN32
extern void *XboxSdlCreateGpu(void *egl, void *display, void *context, void *flush, unsigned color, int w, int h);
extern SDL_bool XboxSdlBindHostWindow(SDL_Window *window);
extern int XboxSdlGpuFrame(void *state, unsigned color, Uint32 shape, int x, int y, int w, int h, int pitch, const void *rgba);
extern void XboxSdlDestroyGpu(void *state);
static void XboxSdlRecreateGpu(_THIS, OFFSCREEN_Window *window)
{
    XboxSdlDestroyGpu(window->xbox_gpu);
    window->xbox_gpu = XboxSdlCreateGpu(_this->egl_data->egl_dll_handle, _this->egl_data->egl_display,
        window->xbox_context, SDL_EGL_GetProcAddress(_this, "glFlush"), window->xbox_color, window->xbox_width, window->xbox_height);
}
#endif

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
    offscreen->xbox_get_integer = SDL_EGL_GetProcAddress(_this, "glGetIntegerv");
    offscreen->xbox_pixel_store = SDL_EGL_GetProcAddress(_this, "glPixelStorei");
    offscreen->xbox_read_pixels = SDL_EGL_GetProcAddress(_this, "glReadPixels");
    offscreen->xbox_bind_framebuffer = SDL_EGL_GetProcAddress(_this, "glBindFramebuffer");
    offscreen->xbox_bind_buffer = SDL_EGL_GetProcAddress(_this, "glBindBuffer");
    offscreen->xbox_pack_invert = SDL_GL_ExtensionSupported("GL_MESA_pack_invert");
    offscreen->xbox_readback_samples = SDL_getenv("XBOX_SDL_TRACE_FILE") ? 120 : 0;
    offscreen->xbox_readback_ticks = 0;
    offscreen->xbox_width = window->w;
    offscreen->xbox_height = window->h;
#ifdef _WIN32
    XboxSdlRecreateGpu(_this, offscreen);
#endif
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
#ifdef _WIN32
        XboxSdlRecreateGpu(_this, offscreen);
#endif
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
    SDL_bool own_context = offscreen && offscreen->xbox_context && offscreen->xbox_context == SDL_GL_GetCurrentContext();
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = own_context
        ? offscreen->xbox_bind_framebuffer : (device ? SDL_EGL_GetProcAddress(device, "glBindFramebuffer") : NULL);
    if (bind_framebuffer) {
        if (!framebuffer && own_context) {
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

static SDL_bool XboxSdlReadFrame(OFFSCREEN_Window *offscreen, Uint8 *pixels)
{
    typedef void (APIENTRY *GetInteger)(GLenum, GLint *);
    typedef void (APIENTRY *PixelStore)(GLenum, GLint);
    typedef void (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GetInteger get_integer = offscreen->xbox_get_integer;
    PixelStore pixel_store = offscreen->xbox_pixel_store;
    ReadPixels read_pixels = offscreen->xbox_read_pixels;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = offscreen->xbox_bind_framebuffer;
    PFNGLBINDBUFFERPROC bind_buffer = offscreen->xbox_bind_buffer;
    GLint old_read, old_pack_buffer;
    const GLenum pack_options[] = { GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS,
                                    GL_PACK_SKIP_ROWS, GL_PACK_INVERT_MESA };
    const GLint desired_pack[] = { 4, 0, 0, 0, 1 };
    const int options = offscreen->xbox_pack_invert ? 5 : 4;
    GLint old_pack[SDL_arraysize(pack_options)];
    int pitch = offscreen->xbox_width * 4;
    if (!get_integer || !pixel_store || !read_pixels || !bind_framebuffer || !bind_buffer) return SDL_FALSE;
    SDL_memset(pixels, 0xCD, 4);
    get_integer(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    get_integer(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    if ((GLuint)old_read != offscreen->xbox_framebuffer)
        bind_framebuffer(GL_READ_FRAMEBUFFER, offscreen->xbox_framebuffer);
    if (old_pack_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    for (int i = 0; i < options; ++i) {
        get_integer(pack_options[i], &old_pack[i]);
        if (old_pack[i] != desired_pack[i]) pixel_store(pack_options[i], desired_pack[i]);
    }
    Uint64 start = offscreen->xbox_readback_samples ? SDL_GetPerformanceCounter() : 0;
    read_pixels(0, 0, offscreen->xbox_width, offscreen->xbox_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (offscreen->xbox_readback_samples) {
        offscreen->xbox_readback_ticks += SDL_GetPerformanceCounter() - start;
        if (!--offscreen->xbox_readback_samples) {
            char timing[96];
            SDL_snprintf(timing, sizeof(timing), "sdl_read_pixels_avg_us=%.0f\nsdl_read_pixels_inverted=%d\n",
                         offscreen->xbox_readback_ticks * (1000000.0 / SDL_GetPerformanceFrequency()) / 120,
                         (int)offscreen->xbox_pack_invert);
            XboxSdlTrace(timing);
        }
    }
    for (int i = 0; i < options; ++i) {
        if (old_pack[i] != desired_pack[i]) pixel_store(pack_options[i], old_pack[i]);
    }
    if (old_pack_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, old_pack_buffer);
    if ((GLuint)old_read != offscreen->xbox_framebuffer) bind_framebuffer(GL_READ_FRAMEBUFFER, old_read);

    if (pixels[0] == 0xCD && pixels[1] == 0xCD && pixels[2] == 0xCD && pixels[3] == 0xCD) return SDL_FALSE;

    if (!offscreen->xbox_pack_invert) {
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
    }
    XboxSdlBlitOffscreenCursor(offscreen->sdl_window, pixels, offscreen->xbox_width, offscreen->xbox_height);
    return SDL_TRUE;
}

static void XboxSdlPresentHostFrame(OFFSCREEN_Window *offscreen)
{
    if (offscreen->xbox_context != SDL_GL_GetCurrentContext()) return;
#ifdef _WIN32
    if (offscreen->xbox_gpu && XboxSdlBindHostWindow(offscreen->sdl_window)) {
        XboxSdlCursor *cursor = XboxSdlVisibleCursor(offscreen->sdl_window);
        int x = 0, y = 0;
        if (cursor) {
            SDL_GetMouseState(&x, &y);
            x -= cursor->hot_x;
            y -= cursor->hot_y;
        }
        if (XboxSdlGpuFrame(offscreen->xbox_gpu, offscreen->xbox_color, cursor ? cursor->shape : 0, x, y,
                cursor ? cursor->image->w : 0, cursor ? cursor->image->h : 0, cursor ? cursor->image->pitch : 0,
                cursor && cursor->plain_rgba ? cursor->image->pixels : NULL)) return;
    }
#endif
    Uint8 *pixels = XboxSdlBeginHostFrame(offscreen->sdl_window, offscreen->xbox_width, offscreen->xbox_height);
    if (pixels) XboxSdlEndHostFrame(XboxSdlReadFrame(offscreen, pixels));
}

static void XboxSdlCaptureFrame(OFFSCREEN_Window *offscreen)
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
    if (pixels && XboxSdlReadFrame(offscreen, pixels)) {
        surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, offscreen->xbox_width, offscreen->xbox_height,
                                                     32, pitch, SDL_PIXELFORMAT_RGBA32);
    }
    XboxSdlTrace(surface && SDL_SaveBMP(surface, path) == 0 ? "sdl_offscreen_frame=ready\n" : "sdl_offscreen_frame=error\n");
    SDL_FreeSurface(surface);
    SDL_free(pixels);
}
