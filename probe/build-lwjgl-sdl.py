#!/usr/bin/env python3
"""Build the opt-in SDL display and context providers in the sealed LWJGL 2.9.2 JAR."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile


PINS = (
    '89541c3c6e1cfadf1c0ddc3e9d291fdd8112ddad506c9eca2636223b728818b4',
    '5be32082256ff3b7aa4a4a740fb9960375d83cfdaa8b79b62c28df1d7f1a3e31',
    '8cadd43ac5eb6d09de05faecca38b917a040bb9139c7edeb4cc81c740b713281',
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path)
    parser.add_argument('api', type=Path)
    parser.add_argument('asm', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--render-size', choices=('1280x720', '960x540', '854x480', '640x360'), default='854x480',
                        help='Slay virtual window size in the signed adapter (default: %(default)s)')
    parser.add_argument('--gdx-api', type=Path)
    parser.add_argument('--gdx-controllers-api', type=Path)
    args = parser.parse_args()
    for file, digest in zip((args.sources, args.api, args.asm), PINS):
        if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            parser.error(f'{file.name} is not the pinned LWJGL artifact.')
    providers = Path(__file__).with_name('lwjgl')
    gdx = (args.gdx_api, args.gdx_controllers_api)
    if any(gdx) and not all(gdx):
        parser.error('Supply both libGDX 1.9.5 API JARs for the controller and scene texture adapters.')
    if all(gdx):
        for file, digest in zip(gdx, (
            'dee9df299c0aa8f7872851433fe383103bf476df4d22461693bdcf3fbdb68c1b',
            'd4d248b6be5a87e958a8b0395f3fa100bfa147ec538ed7ce3829dd70b5d00bb1',
        )):
            if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                parser.error(f'{file.name} is not the pinned libGDX API.')
    with TemporaryDirectory() as scratch, ZipFile(args.sources) as sources, ZipFile(args.api) as api:
        root = Path(scratch)
        inputs = [providers / name for name in ('XboxSdlDisplay.java', 'XboxSdlContext.java', 'XboxSdlGamepad.java', 'XboxRenderProfile.java')]
        mode_source = sources.read('org/lwjgl/opengl/DisplayMode.java').decode('utf-8')
        original_window = 'this(width, height, 0, 0, false);'
        if mode_source.count(original_window) != 1:
            raise ValueError('Unexpected LWJGL windowed display constructor')
        mode = root / 'DisplayMode.java'
        mode.write_text(mode_source.replace(original_window, 'this(XboxRenderProfile.windowWidth(width), '
                                            'XboxRenderProfile.windowHeight(height), 0, 0, false);'))
        inputs.append(mode)
        classpath = [args.api]
        if all(gdx):
            inputs.extend(providers / name for name in ('DesktopControllerManager.java', 'XboxSceneTexture.java', 'DisplayConfig.java'))
            classpath.extend(gdx)
        subprocess.run(['javac', '--release', '8', '-cp', os.pathsep.join(map(str, classpath)), '-d', str(root),
                        *map(str, inputs)], check=True)
        subprocess.run(['javac', '-cp', str(args.asm), '-d', str(root), str(providers / 'BuildXboxFactories.java')],
                       check=True)
        originals = {f'org/lwjgl/opengl/{name}.class': api.read(f'org/lwjgl/opengl/{name}.class')
                     for name in ('Display', 'ContextGL', 'GLContext')}
        if all(gdx):
            with ZipFile(args.gdx_api) as gdx_jar:
                factory = 'com/badlogic/gdx/graphics/TextureData$Factory.class'
                originals[factory] = gdx_jar.read(factory)
        for entry, bytecode in originals.items():
            original = root / 'original.class'
            original.write_bytes(bytecode)
            subprocess.run(['java', '-cp', os.pathsep.join((str(root), str(args.asm))), 'BuildXboxFactories',
                            str(original), str(root / entry)], check=True)
        replacements = {path.relative_to(root).as_posix(): path for package in ('org', 'com')
                        for path in (root / package).rglob('*.class')}
        license_body = sources.read('org/lwjgl/opengl/Display.java').decode('utf-8').split('package org.lwjgl.opengl;', 1)[0]
        with ZipFile(args.output, 'x', ZIP_DEFLATED) as output:
            for entry in api.infolist():
                if entry.filename not in replacements:
                    contents = api.read(entry)
                    if all(gdx) and entry.filename == 'META-INF/MANIFEST.MF':
                        # This package also contains classes loaded from the publisher's game JAR.
                        contents = contents.rstrip(b'\r\n') + (b'\r\n\r\nName: com/badlogic/gdx/graphics/\r\nSealed: false'
                                                                b'\r\n\r\nName: com/megacrit/cardcrawl/core/\r\nSealed: false\r\n\r\n')
                    output.writestr(entry, contents)
            for name, path in sorted(replacements.items()):
                output.write(path, name)
            output.writestr('LICENSE.lwjgl', license_body)
            output.writestr('xbox-slay-render-size.txt', args.render_size + '\n')
            if all(gdx):
                output.write(providers / 'LICENSE.libgdx', 'LICENSE.libgdx')


if __name__ == '__main__':
    main()
