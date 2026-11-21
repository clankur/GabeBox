#pragma once
#include "SDL_stdinc.h"

static int XboxSdlDisplayDimension(const char *value, int minimum, int maximum)
{
    if (!value || !*value || SDL_strlen(value) > 4) return 0;
    int pixels = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9') return 0;
        pixels = pixels * 10 + (*value - '0');
    }
    return pixels >= minimum && pixels <= maximum ? pixels : 0;
}

/* Only the host opts into a virtual desktop. Games retain their explicit fullscreen
   modes and logical rendering surfaces. SDL computes mouse positions from the window. */
static SDL_bool XboxSdlHostedDisplaySize(int *width, int *height)
{
    const char *mailbox = SDL_getenv("XBOX_SDL_HOST_MAILBOX");
    if (!mailbox || !*mailbox) return SDL_FALSE;
    int w = XboxSdlDisplayDimension(SDL_getenv("XBOX_SDL_RENDER_WIDTH"), 640, 1920);
    int h = XboxSdlDisplayDimension(SDL_getenv("XBOX_SDL_RENDER_HEIGHT"), 360, 1080);
    if (!w || !h) return SDL_FALSE;
    *width = w;
    *height = h;
    return SDL_TRUE;
}
