#pragma once

#include "../../events/SDL_mouse_c.h"
#include "SDL_surface.h"

typedef struct XboxSdlCursor
{
    SDL_Cursor cursor;
    SDL_Surface *image;
    int hot_x;
    int hot_y;
    Uint32 shape;
    SDL_bool plain_rgba;
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
    static SDL_atomic_t next_shape;
    cursor->shape = (Uint32)SDL_AtomicAdd(&next_shape, 1) + 1;
    Uint8 r, g, b, a;
    SDL_GetSurfaceColorMod(cursor->image, &r, &g, &b);
    SDL_GetSurfaceAlphaMod(cursor->image, &a);
    cursor->plain_rgba = !SDL_HasColorKey(cursor->image) && r == 255 && g == 255 && b == 255 && a == 255;
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

static inline XboxSdlCursor *XboxSdlVisibleCursor(SDL_Window *window)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    if (!mouse->cursor_shown || mouse->focus != window || !mouse->CreateCursor ||
        (mouse->relative_mode && !mouse->relative_mode_cursor_visible)) {
        return NULL;
    }
    SDL_Cursor *active = SDL_GetCursor();
    return active ? (XboxSdlCursor *)active->driverdata : NULL;
}

static inline void XboxSdlBlitOffscreenCursor(SDL_Window *window, Uint8 *pixels, int width, int height)
{
    XboxSdlCursor *cursor = XboxSdlVisibleCursor(window);
    if (cursor) {
        SDL_Surface *target = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height,
                                                                 32, width * 4, SDL_PIXELFORMAT_RGBA32);
        if (!target) return;
        SDL_Rect position = { 0, 0, cursor->image->w, cursor->image->h };
        SDL_GetMouseState(&position.x, &position.y);
        position.x -= cursor->hot_x;
        position.y -= cursor->hot_y;
        SDL_BlitSurface(cursor->image, NULL, target, &position);
        SDL_FreeSurface(target);
    }
}
