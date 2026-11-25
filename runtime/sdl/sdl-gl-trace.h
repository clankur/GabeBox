#pragma once

/* Log the first entry and return of each operation without consuming GL
   error state or touching game resources. */
#define XBOX_SDL_GL_TRACE(name, params, args)                                      \
    static void APIENTRY XboxSdlTrace##name params                               \
    {                                                                            \
        typedef void (APIENTRY *GlFunction) params;                                \
        static SDL_atomic_t first_call;                                           \
        SDL_VideoDevice *device = SDL_GetVideoDevice();                            \
        GlFunction original = device ? SDL_EGL_GetProcAddress(device, "gl" #name) : NULL; \
        const SDL_bool trace = SDL_AtomicCAS(&first_call, 0, 1);                   \
        if (trace) XboxSdlTrace("sdl_gl_call_" #name "=begin\n");                   \
        if (original) original args;                                             \
        if (trace) XboxSdlTrace(original ? "sdl_gl_call_" #name "=returned\n"       \
                                         : "sdl_gl_call_" #name "=unavailable\n"); \
    }

XBOX_SDL_GL_TRACE(Viewport, (GLint x, GLint y, GLsizei width, GLsizei height), (x, y, width, height))
XBOX_SDL_GL_TRACE(Clear, (GLbitfield mask), (mask))
XBOX_SDL_GL_TRACE(TexImage2D, (GLenum target, GLint level, GLint format, GLsizei width, GLsizei height,
                              GLint border, GLenum pixels_format, GLenum type, const void *pixels),
                   (target, level, format, width, height, border, pixels_format, type, pixels))
XBOX_SDL_GL_TRACE(CompileShader, (GLuint shader), (shader))
XBOX_SDL_GL_TRACE(LinkProgram, (GLuint program), (program))
XBOX_SDL_GL_TRACE(DrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices),
                   (mode, count, type, indices))
XBOX_SDL_GL_TRACE(DrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count))

#undef XBOX_SDL_GL_TRACE

static void *XboxSdlTracedGlFunction(const char *name)
{
#define XBOX_SDL_GL_MATCH(symbol) if (SDL_strcmp(name, "gl" #symbol) == 0) return (void *)XboxSdlTrace##symbol
    XBOX_SDL_GL_MATCH(Viewport);
    XBOX_SDL_GL_MATCH(Clear);
    XBOX_SDL_GL_MATCH(TexImage2D);
    XBOX_SDL_GL_MATCH(CompileShader);
    XBOX_SDL_GL_MATCH(LinkProgram);
    XBOX_SDL_GL_MATCH(DrawElements);
    XBOX_SDL_GL_MATCH(DrawArrays);
#undef XBOX_SDL_GL_MATCH
    return NULL;
}
