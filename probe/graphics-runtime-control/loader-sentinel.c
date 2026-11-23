/* Supply a recognizable version if the loader chooses the adjacent test DLL. */
struct SDLVersion { unsigned char major, minor, patch; };

__declspec(dllexport) void SDL_GetVersion(struct SDLVersion *version) {
    *version = (struct SDLVersion){255, 0, 0};
}
