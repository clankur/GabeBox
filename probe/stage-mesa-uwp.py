#!/usr/bin/env python3
"""Stage the pinned DesktopGL runtime in a local Xbox diagnostic layout."""

import argparse
import hashlib
from pathlib import Path
import re
import sys
from xml.etree import ElementTree as ET
from zipfile import BadZipFile, ZipFile


SOURCE = 'https://github.com/aerisarn/mesa-uwp/tree/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b'
RELEASE_SHA256 = '1502dfd1af9fb831b0ab82ea7e16b71a5b415d8f07a9090578e3a89c8acc1bd6'
LICENSE_SHA256 = 'a00275a53178e2645fb65be99a785c110513446a5071ff2c698ed260ad917d75'
LIBRARIES = ('opengl32.dll', 'libgallium_wgl.dll', 'libglapi.dll', 'libEGL.dll', 'libGLESv2.dll', 'z-1.dll')
UWP_FRAMEWORK = 'Microsoft.VCLibs.140.00'
MICROSOFT = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'


def stage_mesa(layout, archive, license_file, framework):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != RELEASE_SHA256:
        raise ValueError('Use the pinned, unchanged Mesa UWP alpha-2-resfix.zip.')
    license_data = license_file.read_bytes()
    if hashlib.sha256(license_data).hexdigest() != LICENSE_SHA256:
        raise ValueError('Use the Mesa UWP license from the pinned source revision.')
    manifest_file = layout / 'AppxManifest.xml'
    manifest = manifest_file.read_bytes()
    identity = ET.fromstring(manifest).find('{*}Identity')
    if (identity is None or identity.get('Name') != 'XboxPortabilityProbe.ChildProcess.SteamCore'
            or identity.get('ProcessorArchitecture') != 'x64'):
        raise ValueError('Use an x64 Steam Core diagnostic layout.')
    if (layout / 'steamclient64.dll').exists():
        target = (layout / 'steam-launch-app.txt').read_text(encoding='ascii').splitlines()
        if not target or target[0] not in ('413150', '646570'):
            raise ValueError('Scope this Mesa game trial to Stardew Valley or Slay the Spire.')
    if (manifest.count(b'</Dependencies>') != 1 or f'Name="{UWP_FRAMEWORK}"'.encode() in manifest
            or any((layout / name).exists() for name in (*LIBRARIES, 'LICENSE.mesa-uwp', 'mesa-uwp-source.txt'))):
        raise ValueError('Use a fresh diagnostic layout so the trial cannot overwrite an existing runtime.')

    with ZipFile(framework) as signed:
        metadata = ET.fromstring(signed.read('AppxManifest.xml'))
        identity = metadata.find('{*}Identity')
        files = {name.lower() for name in signed.namelist()}
        version = identity.get('Version', '') if identity is not None else ''
        if (identity is None or identity.get('Name') != UWP_FRAMEWORK or identity.get('Publisher') != MICROSOFT
                or identity.get('ProcessorArchitecture') != 'x64' or not re.fullmatch(r'14\.0\.\d+\.\d+', version)
                or metadata.findtext('{*}Properties/{*}Framework') != 'true'
                or not {'appxsignature.p7x', 'vcruntime140_app.dll', 'vccorlib140_app.dll', 'msvcp140_app.dll'} <= files):
            raise ValueError('Use the original signed Microsoft.VCLibs.140.00 x64 UWP framework APPX.')

    with ZipFile(archive) as mesa:
        for name in LIBRARIES:
            with (layout / name).open('xb') as output:
                output.write(mesa.read(name))
    dependency = f'  <PackageDependency Name="{UWP_FRAMEWORK}" Publisher="{MICROSOFT}" MinVersion="{version}" />\n  '
    manifest_file.write_bytes(manifest.replace(b'</Dependencies>', dependency.encode('ascii') + b'</Dependencies>'))
    (layout / 'LICENSE.mesa-uwp').write_bytes(license_data)
    (layout / 'mesa-uwp-source.txt').write_text(SOURCE + '\n', encoding='ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout', type=Path)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--license', required=True, type=Path)
    parser.add_argument('--framework', required=True, type=Path)
    args = parser.parse_args()
    stage_mesa(args.layout, args.archive, args.license, args.framework)
    print('Staged the pinned Mesa UWP runtime and framework dependency.')


if __name__ == '__main__':
    try:
        main()
    except (BadZipFile, ET.ParseError, OSError, ValueError) as error:
        sys.exit(f'Error: {error}')
