#pragma once

#include "../../events/SDL_mouse_c.h"
#include "SDL_surface.h"

typedef struct XboxSdlCursor
{
    SDL_Cursor cursor;
    SDL_Surface *image;
    int hot_x;
    int hot_y;
} XboxSdlCursor;

static SDL_Cursor *XboxSdlCreateOffscreenCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    XboxSdlCursor *cursor = SDL_calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
        return NULL;
    }
    cursor->image = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    if (!cursor->image) {
        SDL_free(cursor);
        return NULL;
    }
    SDL_SetSurfaceBlendMode(cursor->image, SDL_BLENDMODE_BLEND);
    cursor->cursor.driverdata = cursor;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;
    return &cursor->cursor;
}

static void XboxSdlFreeOffscreenCursor(SDL_Cursor *base)
{
    XboxSdlCursor *cursor = (XboxSdlCursor *)base;
    SDL_FreeSurface(cursor->image);
    SDL_free(cursor);
}

static inline void XboxSdlInitOffscreenCursor(void)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    mouse->CreateCursor = XboxSdlCreateOffscreenCursor;
    mouse->FreeCursor = XboxSdlFreeOffscreenCursor;
}

static inline void XboxSdlBlitOffscreenCursor(SDL_Window *window, SDL_Surface *target)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    if (!mouse->cursor_shown || mouse->focus != window || !mouse->CreateCursor ||
        (mouse->relative_mode && !mouse->relative_mode_cursor_visible)) {
        return;
    }
    SDL_Cursor *active = SDL_GetCursor();
    if (active && active->driverdata) {
        XboxSdlCursor *cursor = active->driverdata;
        SDL_Rect position = { 0, 0, cursor->image->w, cursor->image->h };
        SDL_GetMouseState(&position.x, &position.y);
        position.x -= cursor->hot_x;
        position.y -= cursor->hot_y;
        SDL_BlitSurface(cursor->image, NULL, target, &position);
    }
}
