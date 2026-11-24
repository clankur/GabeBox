#!/usr/bin/env python3
"""Build a windowed display fallback from the exact upstream LWJGL 2.9.2 source."""

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
DISPLAY_CLASS = 'org/lwjgl/opengl/WindowsDisplay.class'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', type=Path)
    parser.add_argument('api', type=Path)
    parser.add_argument('asm', type=Path, help='original asm-9.7.1.jar from Maven Central')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    for file, digest in zip((args.sources, args.api, args.asm), PINS):
        if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            parser.error(f'{file.name} does not match the pinned upstream artifact')
    with ZipFile(args.sources) as sources:
        code = sources.read('org/lwjgl/opengl/WindowsDisplay.java').decode('utf-8')
    with TemporaryDirectory() as scratch:
        root = Path(scratch)
        original, patched = root / 'original.class', root / 'WindowsDisplay.class'
        with ZipFile(args.api) as archive:
            original.write_bytes(archive.read(DISPLAY_CLASS))
        builder = Path(__file__).with_name('BuildLwjglDisplayFallback.java')
        subprocess.run(['javac', '--release', '8', '-cp', str(args.asm), '-d', str(root), str(builder)], check=True)
        subprocess.run(['java', '-cp', os.pathsep.join((str(root), str(args.asm))), 'BuildLwjglDisplayFallback',
                        str(original), str(patched)], check=True)
        # Keep LWJGL's sealed packages together under the rebuild's single code source.
        with ZipFile(args.api) as archive, ZipFile(args.output, 'x', ZIP_DEFLATED) as output:
            for entry in archive.infolist():
                data = patched.read_bytes() if entry.filename == DISPLAY_CLASS else archive.read(entry)
                output.writestr(entry, data)
            output.writestr('LICENSE.lwjgl', code[:code.index('package org.lwjgl.opengl;')])


if __name__ == '__main__':
    main()
