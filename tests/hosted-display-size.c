#include "../runtime/sdl/sdl-host-display-size.h"
#include <assert.h>

/* Build with SDL2's sdl2-config --cflags --libs and run in a fresh process. */
int main(void)
{
    int w = 800, h = 600;
    SDL_setenv("XBOX_SDL_HOST_MAILBOX", "", 1);
    SDL_setenv("XBOX_SDL_RENDER_WIDTH", "1920", 1);
    SDL_setenv("XBOX_SDL_RENDER_HEIGHT", "1080", 1);
    assert(!XboxSdlHostedDisplaySize(&w, &h) && w == 800 && h == 600);
    SDL_setenv("XBOX_SDL_HOST_MAILBOX", "test-mailbox", 1);
    assert(XboxSdlHostedDisplaySize(&w, &h) && w == 1920 && h == 1080);
    SDL_setenv("XBOX_SDL_RENDER_HEIGHT", "720x", 1);
    assert(!XboxSdlHostedDisplaySize(&w, &h) && w == 1920 && h == 1080);
    SDL_setenv("XBOX_SDL_RENDER_WIDTH", "854", 1);
    SDL_setenv("XBOX_SDL_RENDER_HEIGHT", "480", 1);
    assert(XboxSdlHostedDisplaySize(&w, &h) && w == 854 && h == 480);
    return 0;
}
