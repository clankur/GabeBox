#pragma once

static SDL_bool XboxSdlPrefComponent(const char *name)
{
    const size_t length = name ? SDL_strlen(name) : 0;
    if (length == 0 || name[length - 1] == '.' || name[length - 1] == ' ') {
        return SDL_FALSE;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || SDL_strchr("\\/:*?\"<>|", *p)) {
            return SDL_FALSE;
        }
    }
    return SDL_TRUE;
}

static SDL_bool XboxSdlPrefDirectory(const WCHAR *path)
{
    DWORD attributes;
    if (CreateDirectoryW(path, NULL)) {
        return SDL_TRUE;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return SDL_FALSE;
    }
    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static char *XboxSdlAppLocalPrefPath(const char *org, const char *app)
{
    WCHAR path[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"XBOX_SDL_PREFERENCES_ROOT", path, SDL_arraysize(path));
    char *result = NULL;
    if (!length) {
        return NULL;
    }
    XboxSdlTrace("sdl_pref_path_local=begin\n");
    if (length < SDL_arraysize(path) && XboxSdlPrefComponent(app) &&
        (!org || !*org || XboxSdlPrefComponent(org))) {
        WCHAR *wide_org = WIN_UTF8ToStringW(org ? org : "");
        WCHAR *wide_app = WIN_UTF8ToStringW(app);
        if (wide_org && wide_app && length + SDL_wcslen(wide_org) + SDL_wcslen(wide_app) + 4 <= SDL_arraysize(path)) {
            SDL_bool parent_ready = SDL_TRUE;
            if (*wide_org) {
                SDL_wcslcat(path, L"\\", SDL_arraysize(path));
                SDL_wcslcat(path, wide_org, SDL_arraysize(path));
                parent_ready = XboxSdlPrefDirectory(path);
            }
            if (parent_ready) {
                SDL_wcslcat(path, L"\\", SDL_arraysize(path));
                SDL_wcslcat(path, wide_app, SDL_arraysize(path));
                if (XboxSdlPrefDirectory(path)) {
                    SDL_wcslcat(path, L"\\", SDL_arraysize(path));
                    result = WIN_StringToUTF8W(path);
                }
            }
        }
        SDL_free(wide_org);
        SDL_free(wide_app);
    }
    if (result) {
        SDL_ClearError();
    }
    XboxSdlTrace(result ? "sdl_pref_path_local=ready\n" : "sdl_pref_path_local=error\n");
    return result;
}
