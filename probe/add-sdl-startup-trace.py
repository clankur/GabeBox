"""Add opt-in startup checkpoints to a pinned SDL2 source tree."""

import re
import shutil
import sys
from pathlib import Path


HOOKS = {
    'video/SDL_video.c': (
        ('int', 'SDL_VideoInit', 'driver_name', 'video_init', 'value == 0'),
        ('SDL_Window *', 'SDL_CreateWindow', 'title, x, y, w, h, flags', 'window', 'value != NULL'),
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
    wrapper = (
        f'static {return_type} {implementation}({parameters});\n\n'
        f'{return_type}\n{function}({parameters})\n{{\n'
        f'    XboxSdlTrace("sdl_{stage}=begin\\n");\n'
        f'    {return_type} value = {implementation}({arguments});\n'
        f'    XboxSdlTrace({worked} ? "sdl_{stage}=ready\\n" : "sdl_{stage}=error\\n");\n'
        '    return value;\n}\n\n'
        f'static {return_type}\n{implementation}({parameters})\n{{'
    )
    return source[:match.start()] + wrapper + source[match.end():]


def add_startup_trace(tree):
    source_root = tree / 'src'
    shutil.copyfile(Path(__file__).with_name('sdl-startup-trace.h'), source_root / 'sdl-startup-trace.h')
    for filename, hooks in HOOKS.items():
        file = source_root / filename
        source = file.read_text(encoding='utf-8')
        include = '#include "../SDL_internal.h"'
        if source.count(include) != 1:
            raise ValueError(f'Expected one SDL internal header in {filename}.')
        source = source.replace(include, include + '\n#include "../sdl-startup-trace.h"')
        for hook in hooks:
            source = wrap_definition(source, *hook)
        file.write_text(source, encoding='utf-8')


if __name__ == '__main__':
    add_startup_trace(Path(sys.argv[1]))
