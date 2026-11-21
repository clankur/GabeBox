#pragma once

#include <cstddef>

template <typename GetProc, typename Checkpoint>
static void RunMesaIndexedControl(GetProc getProc, Checkpoint checkpoint, unsigned char (&pixel)[4]) {
    using Handle = unsigned (APIENTRY *)(unsigned);
    using ShaderSource = void (APIENTRY *)(unsigned, int, const char *const *, const int *);
    using ShaderCommand = void (APIENTRY *)(unsigned);
    using Attach = void (APIENTRY *)(unsigned, unsigned);
    using Status = void (APIENTRY *)(unsigned, unsigned, int *);
    using NewProgram = unsigned (APIENTRY *)();
    using BindAttribute = void (APIENTRY *)(unsigned, unsigned, const char *);
    using GenerateBuffers = void (APIENTRY *)(int, unsigned *);
    using BindBuffer = void (APIENTRY *)(unsigned, unsigned);
    using BufferData = void (APIENTRY *)(unsigned, std::ptrdiff_t, const void *, unsigned);
    using VertexPointer = void (APIENTRY *)(unsigned, int, unsigned, unsigned char, int, const void *);
    using Viewport = void (APIENTRY *)(int, int, int, int);
    using Draw = void (APIENTRY *)(unsigned, int, unsigned, const void *);
    using Finish = void (APIENTRY *)();
    using Read = void (APIENTRY *)(int, int, int, int, unsigned, unsigned, void *);

    constexpr unsigned vertexShader = 0x8B31, fragmentShader = 0x8B30;
    constexpr unsigned compileStatus = 0x8B81, linkStatus = 0x8B82;
    constexpr unsigned arrayBuffer = 0x8892, indexBuffer = 0x8893, staticDraw = 0x88E4;
    constexpr unsigned floatType = 0x1406, unsignedShort = 0x1403, unsignedByte = 0x1401;
    constexpr unsigned triangles = 0x0004, rgba = 0x1908;
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
    const auto viewport = reinterpret_cast<Viewport>(getProc("glViewport"));
    const auto draw = reinterpret_cast<Draw>(getProc("glDrawElements"));
    const auto finish = reinterpret_cast<Finish>(getProc("glFinish"));
    const auto read = reinterpret_cast<Read>(getProc("glReadPixels"));
    if (!createShader || !shaderSource || !compile || !shaderStatus || !createProgram || !attach || !bindAttribute ||
            !link || !programStatus || !use || !generate || !bind || !upload || !enableAttribute || !vertexPointer ||
            !viewport || !draw || !finish || !read) return;

    // The caller owns a fresh context and releases these test objects with it.
    const auto buildShader = [&](unsigned kind, const char *source) {
        const auto shader = createShader(kind);
        if (!shader) return 0u;
        shaderSource(shader, 1, &source, nullptr);
        compile(shader);
        int compiled = 0;
        shaderStatus(shader, compileStatus, &compiled);
        return compiled ? shader : 0u;
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
    int linked = 0;
    programStatus(program, linkStatus, &linked);
    if (!linked) return;

    constexpr float points[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    constexpr unsigned short indices[] = { 0, 1, 2 };
    unsigned buffers[2]{};
    generate(2, buffers);
    bind(arrayBuffer, buffers[0]);
    upload(arrayBuffer, sizeof(points), points, staticDraw);
    bind(indexBuffer, buffers[1]);
    upload(indexBuffer, sizeof(indices), indices, staticDraw);
    use(program);
    viewport(0, 0, 4, 4);
    enableAttribute(0);
    vertexPointer(0, 2, floatType, 0, 0, nullptr);
    checkpoint(10);
    draw(triangles, 3, unsignedShort, nullptr);
    checkpoint(11);
    finish();
    checkpoint(12);
    read(1, 1, 1, 1, rgba, unsignedByte, pixel);
    if (pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255) checkpoint(13);
}
