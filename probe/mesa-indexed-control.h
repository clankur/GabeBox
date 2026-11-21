#pragma once

#include <GL/gl.h>
#include <cstddef>

template <typename GetProc, typename Checkpoint>
static void RunMesaIndexedControl(GetProc getProc, Checkpoint checkpoint, unsigned char (&pixel)[4]) {
    using Handle = GLuint (APIENTRY *)(GLenum);
    using ShaderSource = void (APIENTRY *)(GLuint, GLsizei, const char *const *, const GLint *);
    using ShaderCommand = void (APIENTRY *)(GLuint);
    using Attach = void (APIENTRY *)(GLuint, GLuint);
    using Status = void (APIENTRY *)(GLuint, GLenum, GLint *);
    using NewProgram = GLuint (APIENTRY *)();
    using BindAttribute = void (APIENTRY *)(GLuint, GLuint, const char *);
    using GenerateBuffers = void (APIENTRY *)(GLsizei, GLuint *);
    using BindBuffer = void (APIENTRY *)(GLenum, GLuint);
    using BufferData = void (APIENTRY *)(GLenum, std::ptrdiff_t, const void *, GLenum);
    using VertexPointer = void (APIENTRY *)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);

    constexpr GLenum vertexShader = 0x8B31, fragmentShader = 0x8B30;
    constexpr GLenum compileStatus = 0x8B81, linkStatus = 0x8B82;
    constexpr GLenum arrayBuffer = 0x8892, indexBuffer = 0x8893, staticDraw = 0x88E4;
    const auto createShader = reinterpret_cast<Handle>(getProc("glCreateShader"));
    const auto shaderSource = reinterpret_cast<ShaderSource>(getProc("glShaderSource"));
    const auto compile = reinterpret_cast<ShaderCommand>(getProc("glCompileShader"));
    const auto shaderStatus = reinterpret_cast<Status>(getProc("glGetShaderiv"));
    const auto createProgram = reinterpret_cast<NewProgram>(getProc("glCreateProgram"));
    const auto attach = reinterpret_cast<Attach>(getProc("glAttachShader"));
    const auto bindAttribute = reinterpret_cast<BindAttribute>(getProc("glBindAttribLocation"));
    const auto link = reinterpret_cast<ShaderCommand>(getProc("glLinkProgram"));
    const auto programStatus = reinterpret_cast<Status>(getProc("glGetProgramiv"));
    const auto use = reinterpret_cast<ShaderCommand>(getProc("glUseProgram"));
    const auto generate = reinterpret_cast<GenerateBuffers>(getProc("glGenBuffers"));
    const auto bind = reinterpret_cast<BindBuffer>(getProc("glBindBuffer"));
    const auto upload = reinterpret_cast<BufferData>(getProc("glBufferData"));
    const auto enableAttribute = reinterpret_cast<ShaderCommand>(getProc("glEnableVertexAttribArray"));
    const auto vertexPointer = reinterpret_cast<VertexPointer>(getProc("glVertexAttribPointer"));
    const auto viewport = reinterpret_cast<decltype(&glViewport)>(getProc("glViewport"));
    const auto draw = reinterpret_cast<decltype(&glDrawElements)>(getProc("glDrawElements"));
    const auto finish = reinterpret_cast<decltype(&glFinish)>(getProc("glFinish"));
    const auto read = reinterpret_cast<decltype(&glReadPixels)>(getProc("glReadPixels"));
    if (!createShader || !shaderSource || !compile || !shaderStatus || !createProgram || !attach || !bindAttribute ||
            !link || !programStatus || !use || !generate || !bind || !upload || !enableAttribute || !vertexPointer ||
            !viewport || !draw || !finish || !read) return;

    // The caller owns a fresh context and releases these test objects with it.
    const auto buildShader = [&](GLenum kind, const char *source) {
        const auto shader = createShader(kind);
        if (!shader) return GLuint{0};
        shaderSource(shader, 1, &source, nullptr);
        compile(shader);
        GLint compiled = GL_FALSE;
        shaderStatus(shader, compileStatus, &compiled);
        return compiled ? shader : GLuint{0};
    };
    checkpoint(9);
    const auto vertex = buildShader(vertexShader, "#version 120\nattribute vec2 point;"
        "void main() { gl_Position = vec4(point, 0.0, 1.0); }");
    const auto fragment = buildShader(fragmentShader, "#version 120\n"
        "void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }");
    if (!vertex || !fragment) return;
    const auto program = createProgram();
    if (!program) return;
    attach(program, vertex);
    attach(program, fragment);
    bindAttribute(program, 0, "point");
    link(program);
    GLint linked = GL_FALSE;
    programStatus(program, linkStatus, &linked);
    if (!linked) return;

    constexpr GLfloat points[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    constexpr GLushort indices[] = { 0, 1, 2 };
    GLuint buffers[2]{};
    generate(2, buffers);
    bind(arrayBuffer, buffers[0]);
    upload(arrayBuffer, sizeof(points), points, staticDraw);
    bind(indexBuffer, buffers[1]);
    upload(indexBuffer, sizeof(indices), indices, staticDraw);
    use(program);
    viewport(0, 0, 4, 4);
    enableAttribute(0);
    vertexPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    checkpoint(10);
    draw(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
    checkpoint(11);
    finish();
    checkpoint(12);
    read(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255) checkpoint(13);
}
