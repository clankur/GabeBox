#pragma once

#include "SDL_endian.h"
#include "SDL_timer.h"

/* SDL_RenderPresent runs on the renderer's main thread. Capture a bounded
   sample before presenting, while the software back buffer is still valid. */
static void XboxSdlCaptureSoftwareFrame(SDL_Renderer *renderer)
{
    static unsigned attempts;
    static Uint32 last_attempt;
    static Uint32 first_present;
    static Uint32 delay;
    static SDL_bool started;
    static SDL_bool captured;
    char path[1024];
    int width, height;
    SDL_RendererInfo info;
    const char *trace = SDL_getenv("XBOX_SDL_TRACE_FILE");
    const char *driver;
    SDL_Surface *frame;
    Uint32 now;
    size_t length;
    if (captured || attempts >= 12 || !trace) {
        return;
    }
    length = SDL_strlen(trace);
    driver = SDL_GetCurrentVideoDriver();
    if (length <= 4 || length >= sizeof(path) || SDL_strcmp(trace + length - 4, ".txt") != 0 ||
        !driver || SDL_strcmp(driver, "dummy") != 0 || SDL_GetRenderTarget(renderer)) {
        return;
    }
    SDL_strlcpy(path, trace, sizeof(path));
    SDL_strlcpy(path + length - 4, ".bmp", 5);
    now = SDL_GetTicks();
    if (!started) {
        const char *setting = SDL_getenv("XBOX_SDL_SOFTWARE_FRAME_DELAY_MS");
        int requested = setting ? SDL_atoi(setting) : 0;
        delay = requested > 0 && requested <= 30000 ? (Uint32)requested : 0;
        first_present = now;
        started = SDL_TRUE;
    }
    if (now - first_present < delay) {
        return;
    }
    if (attempts && now - last_attempt < 250) {
        return;
    }
    last_attempt = now;
    ++attempts;
    if (SDL_GetRendererInfo(renderer, &info) < 0 || !(info.flags & SDL_RENDERER_SOFTWARE) ||
        SDL_GetRendererOutputSize(renderer, &width, &height) < 0 ||
        width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        XboxSdlTrace("sdl_software_frame=error\n");
        return;
    }

    frame = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (frame && SDL_RenderReadPixels(renderer, NULL, frame->format->format, frame->pixels, frame->pitch) == 0) {
        const Uint32 first = *(const Uint32 *)frame->pixels;
        SDL_bool nonuniform = SDL_FALSE;
        for (int y = 0; y < height && !nonuniform; ++y) {
            const Uint32 *row = (const Uint32 *)((const Uint8 *)frame->pixels + (size_t)y * frame->pitch);
            for (int x = 0; x < width; ++x) {
                if ((first ^ row[x]) & SDL_SwapLE32(0x00FFFFFFu)) {
                    nonuniform = SDL_TRUE;
                    break;
                }
            }
        }
        if (nonuniform) {
            captured = SDL_SaveBMP(frame, path) == 0;
            XboxSdlTrace(captured ? "sdl_software_frame=ready\n" : "sdl_software_frame=error\n");
        } else if (attempts == 12) {
            XboxSdlTrace("sdl_software_frame=uniform\n");
        }
    } else {
        XboxSdlTrace("sdl_software_frame=error\n");
    }
    SDL_FreeSurface(frame);
}
