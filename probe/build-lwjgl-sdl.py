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
    parser.add_argument('--gdx-api', type=Path)
    parser.add_argument('--gdx-controllers-api', type=Path)
    args = parser.parse_args()
    for file, digest in zip((args.sources, args.api, args.asm), PINS):
        if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            parser.error(f'{file.name} is not the pinned LWJGL artifact.')
    providers = Path(__file__).with_name('lwjgl')
    gdx = (args.gdx_api, args.gdx_controllers_api)
    if any(gdx) and not all(gdx):
        parser.error('Supply both libGDX 1.9.5 API JARs for the controller adapter.')
    if all(gdx):
        for file, digest in zip(gdx, (
            'dee9df299c0aa8f7872851433fe383103bf476df4d22461693bdcf3fbdb68c1b',
            'd4d248b6be5a87e958a8b0395f3fa100bfa147ec538ed7ce3829dd70b5d00bb1',
        )):
            if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                parser.error(f'{file.name} is not the pinned libGDX API.')
    with TemporaryDirectory() as scratch, ZipFile(args.sources) as sources, ZipFile(args.api) as api:
        root = Path(scratch)
        inputs = [providers / name for name in ('XboxSdlDisplay.java', 'XboxSdlContext.java', 'XboxSdlGamepad.java')]
        classpath = [args.api]
        if all(gdx):
            inputs.append(providers / 'DesktopControllerManager.java')
            classpath.extend(gdx)
        subprocess.run(['javac', '--release', '8', '-cp', os.pathsep.join(map(str, classpath)), '-d', str(root),
                        *map(str, inputs)], check=True)
        subprocess.run(['javac', '-cp', str(args.asm), '-d', str(root), str(providers / 'BuildXboxFactories.java')],
                       check=True)
        for source in ('Display', 'ContextGL', 'GLContext'):
            entry = f'org/lwjgl/opengl/{source}.class'
            original = root / 'original.class'
            original.write_bytes(api.read(entry))
            subprocess.run(['java', '-cp', os.pathsep.join((str(root), str(args.asm))), 'BuildXboxFactories',
                            str(original), str(root / entry)], check=True)
        replacements = {str(path.relative_to(root)): path for package in ('org', 'com')
                        for path in (root / package).rglob('*.class')}
        license_body = sources.read('org/lwjgl/opengl/Display.java').decode('utf-8').split('package org.lwjgl.opengl;', 1)[0]
        with ZipFile(args.output, 'x', ZIP_DEFLATED) as output:
            for entry in api.infolist():
                if entry.filename not in replacements:
                    output.writestr(entry, api.read(entry))
            for name, path in sorted(replacements.items()):
                output.write(path, name)
            output.writestr('LICENSE.lwjgl', license_body)


if __name__ == '__main__':
    main()
