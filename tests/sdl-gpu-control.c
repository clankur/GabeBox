/* Same-package child for the SDL GPU presenter control. Launch from the package
   alongside SDL2.dll and the pinned Mesa DLLs, with XBOX_SDL_HOST_MAILBOX set
   by the normal host. No Steam credentials, installed games, or saves needed. */
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_opengl.h"
#include "../probe/sdl-gpu-protocol.h"

enum { width = 1920, height = 1080, fps = 60, duration = 30 };
typedef void (APIENTRY *Color)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *Clear)(GLbitfield);
typedef void (APIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *Enable)(GLenum);
typedef const GLubyte *(APIENTRY *GetString)(GLenum);
static Color color;
static Clear clear;
static Scissor scissor;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static SdlGpuMailbox *exchange;

static void record(const char *line)
{
    DWORD written;
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, line, (DWORD)SDL_strlen(line), &written, NULL);
}
static int configure(void)
{
    WCHAR path[2048], *filename;
    DWORD n = GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX", path, ARRAYSIZE(path));
    if (!n || n + 16 >= ARRAYSIZE(path)) return 0;
    SDL_wcslcpy(path + n, L".control.txt", ARRAYSIZE(path) - n);
    log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    SDL_wcslcpy(path + n, L".gpu", ARRAYSIZE(path) - n);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        HANDLE mapping = CreateFileMappingFromApp(file, NULL, PAGE_READONLY, 0, NULL);
        if (mapping) {
            exchange = (SdlGpuMailbox *)MapViewOfFileFromApp(mapping, FILE_MAP_READ, 0, sizeof(SdlGpuMailbox));
            CloseHandle(mapping);
        }
        CloseHandle(file);
    }
    n = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) return 0;
    filename = path + n;
    while (filename > path && filename[-1] != L'\\') --filename;
    if (filename == path) return 0;
    size_t left = ARRAYSIZE(path) - (size_t)(filename - path);
    if (left < 16) return 0;
    SDL_wcslcpy(filename, L"opengl32.dll", left);
    if (!SetEnvironmentVariableW(L"SDL_VIDEO_GL_DRIVER", path)) return 0;
    SDL_wcslcpy(filename, L"libEGL.dll", left);
    return SetEnvironmentVariableW(L"SDL_VIDEO_EGL_DRIVER", path) &&
           SetEnvironmentVariableW(L"SDL_VIDEODRIVER", L"offscreen") &&
           SetEnvironmentVariableW(L"SDL_VIDEO_WIN_D3DCOMPILER", L"none") &&
           SetEnvironmentVariableW(L"XBOX_SDL_SURFACELESS", L"1");
}
static void fill(int x, int y, int w, int h, float r, float g, float b)
{
    scissor(x, y, w, h);
    color(r, g, b, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
}
static void draw(unsigned frame)
{
    /* The top-left red and top-right green distinguish vertical inversion and
       channel swaps. A yellow line moves continuously. The 12 center squares
       encode the frame number, least significant bit at the left. */
    fill(0, height / 2, width / 2, height / 2, 0.85f, 0.04f, 0.08f);
    fill(width / 2, height / 2, width / 2, height / 2, 0.04f, 0.75f, 0.08f);
    fill(0, 0, width / 2, height / 2, 0.04f, 0.12f, 0.90f);
    fill(width / 2, 0, width / 2, height / 2, 0.13f, 0.15f, 0.17f);
    fill((int)((frame * 9) % (width - 12)), 0, 12, height, 1.0f, 0.85f, 0);
    for (unsigned bit = 0; bit < 12; ++bit) {
        float v = (frame & (1u << bit)) ? 1.0f : 0;
        fill(300 + bit * 110, height / 2 - 45, 90, 90, v, v, v);
    }
}
static void sample(const char *phase, Uint64 elapsed, Uint64 freq, unsigned rendered)
{
    char line[240];
    LONG generation = 0, accepted = 0, active = 0;
    LONG64 sequence = 0;
    if (exchange && exchange->magic == SDL_GPU_MAGIC) {
        generation = exchange->generation;
        accepted = exchange->accepted_generation;
        active = exchange->gpu_active;
        for (unsigned i = 0; i < SDL_GPU_SLOTS; ++i)
            if (sequence < exchange->sequence[i]) sequence = exchange->sequence[i];
    }
    SDL_snprintf(line, sizeof(line), "%s elapsed_ms=%llu rendered=%u gpu_submitted=%llu generation=%ld accepted=%ld gpu_active=%ld\n",
                 phase, (unsigned long long)(elapsed * 1000 / freq), rendered, (unsigned long long)sequence,
                 generation, accepted, active);
    record(line);
}
static int run_control(void)
{
    SDL_SetMainReady();
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    int exit_code = 1;
    if (!configure()) { record("configuration_failed\n"); goto end; }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) goto error;
    window = SDL_CreateWindow("Mesa FHD control", 0, 0, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window || !(context = SDL_GL_CreateContext(window)) || SDL_GL_MakeCurrent(window, context)) goto error;
    SDL_GL_SetSwapInterval(0);
    SDL_ShowCursor(SDL_DISABLE);
    GetString get_string = (GetString)SDL_GL_GetProcAddress("glGetString");
    Enable enable = (Enable)SDL_GL_GetProcAddress("glEnable");
    color = (Color)SDL_GL_GetProcAddress("glClearColor");
    clear = (Clear)SDL_GL_GetProcAddress("glClear");
    scissor = (Scissor)SDL_GL_GetProcAddress("glScissor");
    if (!get_string || !enable || !color || !clear || !scissor) goto error;
    char renderer[256];
    SDL_snprintf(renderer, sizeof(renderer), "control_renderer=%s size=%dx%d target_fps=%d duration_s=%d\n",
                 (const char *)get_string(GL_RENDERER), width, height, fps, duration);
    record(renderer);
    enable(GL_SCISSOR_TEST);
    Uint64 start = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency(), last = start;
    unsigned frame = 0;
    for (;;) {
        Uint64 now = SDL_GetPerformanceCounter();
        if ((now - start) >= duration * freq) break;
        SDL_Event event;
        while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) goto finished;
        draw(frame++);
        SDL_GL_SwapWindow(window);
        now = SDL_GetPerformanceCounter();
        if (now - last >= 5 * freq) { sample("sample", now - start, freq, frame); last = now; }
        /* Use a wall-clock schedule and skip elapsed pacing intervals without
           submitting a burst of duplicate frames after a stall. */
        Uint64 deadline = start + (((now - start) * fps / freq + 1) * freq + fps - 1) / fps;
        while (now < deadline) {
            Uint32 remaining_ms = (Uint32)((deadline - now) * 1000 / freq);
            if (remaining_ms > 1) SDL_Delay(remaining_ms - 1);
            now = SDL_GetPerformanceCounter();
        }
    }
finished:
    sample("finished", SDL_GetPerformanceCounter() - start, freq, frame);
    exit_code = 0;
    goto end;
error:
    record("control_error="); record(SDL_GetError()); record("\n");
end:
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (exchange) UnmapViewOfFile(exchange);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    return exit_code;
}

#ifdef SDL_GPU_FREESTANDING
void WINAPI XboxSdlGpuControlEntry(void) { ExitProcess((UINT)run_control()); }
#else
int main(void) { return run_control(); }
#endif
