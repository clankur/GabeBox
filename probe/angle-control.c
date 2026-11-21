#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "angle-control.h"

// EGL_ANGLE_platform_angle and EGL_ANGLE_platform_angle_d3d extension values.
#define PLATFORM_ANGLE 0x3202
#define PLATFORM_ANGLE_TYPE 0x3203
#define PLATFORM_ANGLE_TYPE_D3D11 0x3208
#define PLATFORM_ANGLE_DEVICE_TYPE 0x3209
#define PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE 0x320A

static GLuint compile_shader(GLenum kind, const GLchar *source) {
    GLuint shader = glCreateShader(kind);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, 0);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

__declspec(dllexport) void __cdecl RunAngleControl(AngleControlResult *result) {
    const EGLint platform[] = {
        PLATFORM_ANGLE_TYPE, PLATFORM_ANGLE_TYPE_D3D11,
        PLATFORM_ANGLE_DEVICE_TYPE, PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE, EGL_NONE,
    };
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    const EGLint pbuffer[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const GLfloat points[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLBoolean initialized = EGL_FALSE;
    EGLBoolean current = EGL_FALSE;
    EGLConfig config;
    GLuint vertex = 0, fragment = 0, program = 0;
    EGLint major = 0, minor = 0, count = 0;
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    result->stage = 1;
    if (!platform_display) goto finish;
    result->stage = 2;
    display = platform_display(PLATFORM_ANGLE, EGL_DEFAULT_DISPLAY, platform);
    if (display == EGL_NO_DISPLAY) goto finish;
    result->stage = 3;
    initialized = eglInitialize(display, &major, &minor);
    if (!initialized) goto finish;
    result->egl_major = major;
    result->egl_minor = minor;
    result->stage = 4;
    if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, config_attribs, &config, 1, &count) || count != 1)
        goto finish;
    result->stage = 5;
    surface = eglCreatePbufferSurface(display, config, pbuffer);
    if (surface == EGL_NO_SURFACE) goto finish;
    result->stage = 6;
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) goto finish;
    result->stage = 7;
    current = eglMakeCurrent(display, surface, surface, context);
    if (!current) goto finish;

    result->stage = 8;
    vertex = compile_shader(GL_VERTEX_SHADER, "attribute vec2 point; void main() { gl_Position = vec4(point, 0.0, 1.0); }");
    fragment = compile_shader(GL_FRAGMENT_SHADER, "precision mediump float; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }");
    if (!vertex || !fragment) goto finish;
    result->stage = 9;
    program = glCreateProgram();
    if (!program) goto finish;
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "point");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) goto finish;
    result->stage = 10;
    glUseProgram(program);
    glViewport(0, 0, 4, 4);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, points);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, result->pixel);
    if (result->pixel[0] == 0 && result->pixel[1] == 255 && result->pixel[2] == 0 && result->pixel[3] == 255)
        result->stage = 11;

finish:
    result->egl_error = eglGetError();
    if (current) {
        result->gl_error = glGetError();
        if (program) glDeleteProgram(program);
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (initialized) eglTerminate(display);
    eglReleaseThread();
}
