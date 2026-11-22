"""Add opt-in startup checkpoints to a pinned SDL2 source tree."""

import re
import shutil
import sys
from pathlib import Path


HOOKS = {
    'SDL.c': (
        ('int', 'SDL_Init', 'flags', 'init', 'value == 0'),
        ('int', 'SDL_InitSubSystem', 'flags', 'subsystem_init', 'value == 0'),
    ),
    'audio/SDL_audio.c': (
        ('int', 'SDL_AudioInit', 'driver_name', 'audio_init', 'value == 0'),
        ('SDL_AudioDeviceID', 'SDL_OpenAudioDevice', 'device, iscapture, desired, obtained, allowed_changes',
         'audio_device', 'value != 0'),
    ),
    'filesystem/windows/SDL_sysfilesystem.c': (
        ('char *', 'SDL_GetBasePath', '', 'base_path', 'value != NULL'),
        ('char *', 'SDL_GetPrefPath', 'org, app', 'pref_path', 'value != NULL'),
    ),
    'video/SDL_video.c': (
        ('int', 'SDL_VideoInit', 'driver_name', 'video_init', 'value == 0'),
        ('SDL_Window *', 'SDL_CreateWindow', 'title, x, y, w, h, flags', 'window', 'value != NULL'),
        ('int', 'SDL_GL_LoadLibrary', 'path', 'gl_library', 'value == 0'),
        ('SDL_GLContext', 'SDL_GL_CreateContext', 'window', 'gl_context', 'value != NULL'),
        ('int', 'SDL_GL_SetSwapInterval', 'interval', 'gl_swap_interval', 'value == 0'),
        ('int', 'SDL_ShowMessageBox', 'messageboxdata, buttonid', 'message_box', 'value == 0'),
    ),
    'render/SDL_render.c': (
        ('SDL_Renderer *', 'SDL_CreateRenderer', 'window, index, flags', 'renderer', 'value != NULL'),
    ),
}


def wrap_definition(source, return_type, function, arguments, stage, worked):
    result_type = r'\s*\*\s*' if return_type.endswith('*') else r'\s+'
    signature = re.escape(return_type.rstrip(' *')) + result_type + re.escape(function) + r'\(([^)]+)\)\s*\{'
    matches = list(re.finditer(r'^' + signature, source, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f'Expected one {function} definition in the pinned SDL source.')
    match = matches[0]
    parameters = match[1]
    implementation = f'XboxSdlOriginal{function[4:]}'
    flags = '    XboxSdlTraceInitFlags(flags);\n' if function == 'SDL_InitSubSystem' else ''
    preferences = '    if (!value) value = XboxSdlAppLocalPrefPath(org, app);\n' if function == 'SDL_GetPrefPath' else ''
    if function == 'SDL_CreateWindow':
        flags += ('    if ((flags & SDL_WINDOW_RESIZABLE) && !(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FOREIGN)))\n'
                  '        XboxSdlHostedDisplaySize(&w, &h);\n')
    wrapper = (
        f'static {return_type} {implementation}({parameters});\n\n'
        f'{return_type}\n{function}({parameters})\n{{\n'
        f'    XboxSdlTrace("sdl_{stage}=begin\\n");\n'
        f'{flags}'
        f'    {return_type} value = {implementation}({arguments});\n'
        f'{preferences}'
        f'    XboxSdlTrace({worked} ? "sdl_{stage}=ready\\n" : "sdl_{stage}=error\\n");\n'
        '    return value;\n}\n\n'
        f'static {return_type}\n{implementation}({parameters})\n{{'
    )
    return source[:match.start()] + wrapper + source[match.end():]


def add_startup_trace(tree):
    source_root = tree / 'src'
    shutil.copyfile(Path(__file__).with_name('sdl-startup-trace.h'), source_root / 'sdl-startup-trace.h')
    shutil.copyfile(Path(__file__).with_name('sdl-local-preferences.h'), source_root / 'sdl-local-preferences.h')
    shutil.copyfile(Path(__file__).with_name('sdl-software-frame.h'), source_root / 'sdl-software-frame.h')
    for header in ('sdl-host-protocol.h', 'sdl-host-runtime.h'):
        shutil.copyfile(Path(__file__).with_name(header), source_root / 'render' / header)
    shutil.copyfile(Path(__file__).with_name('sdl-host-display-size.h'), source_root / 'sdl-host-display-size.h')
    for driver in ('dummy/SDL_nullvideo.c', 'offscreen/SDL_offscreenvideo.c'):
        file = source_root / 'video' / driver
        content = file.read_text(encoding='utf-8')
        include = '#include "../../SDL_internal.h"'
        register = '    if (SDL_AddBasicVideoDisplay(&mode) < 0) {'
        if content.count(include) != 1 or content.count(register) != 1:
            raise ValueError(f'Expected the pinned SDL virtual desktop in {driver}.')
        content = content.replace(include, include + '\n#include "../../sdl-host-display-size.h"')
        size = '    if (XboxSdlHostedDisplaySize(&mode.w, &mode.h)) mode.refresh_rate = 60;\n'
        file.write_text(content.replace(register, size + register), encoding='utf-8')
    dummy = source_root / 'video/dummy/SDL_nullevents.c'
    source = dummy.read_text(encoding='utf-8')
    marker = '    /* do nothing. */'
    if source.count(marker) != 1:
        raise ValueError('Expected the pinned SDL dummy event pump.')
    source = source.replace('#include "SDL_nullevents_c.h"', '#include "SDL_nullevents_c.h"\n\nextern void XboxSdlPollHostInput(void);')
    dummy.write_text(source.replace(marker, '    XboxSdlPollHostInput();'), encoding='utf-8')
    for filename, hooks in HOOKS.items():
        file = source_root / filename
        source = file.read_text(encoding='utf-8')
        prefix = '../' * filename.count('/') or './'
        include = f'#include "{prefix}SDL_internal.h"'
        if source.count(include) != 1:
            raise ValueError(f'Expected one SDL internal header in {filename}.')
        define = '\n#define XBOX_SDL_MAIN_TRACE' if filename == 'SDL.c' else ''
        source = source.replace(include, include + define + f'\n#include "{prefix}sdl-startup-trace.h"')
        if filename == 'video/SDL_video.c':
            source = source.replace(include, include + '\n#include "../sdl-host-display-size.h"', 1)
        if filename == 'SDL.c':
            declaration = r'^(void\s+SDL_SetMainReady\(void\)\s*\{)'
            source, count = re.subn(declaration, r'\1\n    XboxSdlTrace("sdl_main_ready=begin\\n");'
                                    '\n    XboxSdlSetWorkingDirectory();', source, flags=re.MULTILINE)
            if count != 1:
                raise ValueError('Expected the SDL2 main entrypoint in the pinned source.')
        if filename == 'render/SDL_render.c':
            declaration = r'^(void\s+SDL_RenderPresent\(SDL_Renderer \*\s*renderer\)\s*\{)'
            source, count = re.subn(declaration, '#include "../sdl-software-frame.h"\n'
                                    '#include "sdl-host-runtime.h"\n\n'
                                    r'\1' '\n    XboxSdlPublishHostFrame(renderer);\n'
                                    '    XboxSdlCaptureSoftwareFrame(renderer);', source, flags=re.MULTILINE)
            if count != 1:
                raise ValueError('Expected the SDL renderer present entrypoint in the pinned source.')
        suffix = ''
        if filename == 'filesystem/windows/SDL_sysfilesystem.c':
            # SDL 2.26.2 also has unused Xbox filesystem stubs in this file.
            windows_end = '#endif /* SDL_FILESYSTEM_WINDOWS */'
            windows, ending, after = source.partition(windows_end)
            if not ending:
                raise ValueError('Expected an SDL filesystem implementation guarded for Windows.')
            source, suffix = windows, ending + after
            include = '#include "SDL_filesystem.h"'
            source = source.replace(include, include + '\n#include "../../sdl-local-preferences.h"', 1)
        for hook in hooks:
            source = wrap_definition(source, *hook)
        file.write_text(source + suffix, encoding='utf-8')


if __name__ == '__main__':
    add_startup_trace(Path(sys.argv[1]))
