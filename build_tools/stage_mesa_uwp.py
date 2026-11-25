#!/usr/bin/env python3
"""Stage the pinned DesktopGL runtime in a local Xbox diagnostic layout."""

import argparse
import hashlib
from pathlib import Path
import re
import sys
from xml.etree import ElementTree as ET
from zipfile import BadZipFile, ZipFile


RELEASES = {
    '1502dfd1af9fb831b0ab82ea7e16b71a5b415d8f07a9090578e3a89c8acc1bd6': (
        'https://github.com/aerisarn/mesa-uwp/tree/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b',
        'a00275a53178e2645fb65be99a785c110513446a5071ff2c698ed260ad917d75', '', ('libglapi.dll',),
    ),
    '7776e83908d21ac9a0886b99505e0cfc8c9e47bd55063969ee6095d60a995e0f': (
        'https://github.com/XboxEmulationHub/mesa-uwp/tree/34732bbc207daea10bdf2fb969a4a0f7769ea1de',
        '0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615', 'bin/', (),
    ),
}
LIBRARIES = ('opengl32.dll', 'libgallium_wgl.dll', 'libEGL.dll', 'libGLESv2.dll', 'z-1.dll')
UWP_FRAMEWORK = 'Microsoft.VCLibs.140.00'
MICROSOFT = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'
DXIL_ARCHIVE_SHA256 = '9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4'
DXIL_FILES = {
    'dxil.dll': 'bin/x64/dxil.dll',
    'LICENSE.DXIL-MS.txt': 'LICENSE-MS.txt',
    'LICENSE.DXIL-LLVM.txt': 'LICENSE-LLVM.txt',
    'LICENSE.DXIL-MIT.txt': 'LICENSE-MIT.txt',
}
# Workflow builds of the same revision (.github/workflows/xbox-mesa-uwp.yml, runtime/mesa).
RUNTIME_SOURCE = 'https://github.com/XboxEmulationHub/mesa-uwp/tree/34732bbc207daea10bdf2fb969a4a0f7769ea1de'
RUNTIME_LICENSE_SHA256 = '0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615'
RUNTIME_FILES = (*LIBRARIES, 'LICENSE.mesa-uwp', 'mesa-uwp-source.txt')
RUNTIME_LIMIT = 128 * 1024 * 1024
# D3D12_SHADER_CACHE_STATS only exists in builds with runtime/mesa/patches/0003.
SHADER_CACHE_MARKER = b'D3D12_SHADER_CACHE_STATS'


def has_shader_cache(driver):
    """Whether a libgallium_wgl.dll image carries the persistent d3d12 shader cache."""
    return SHADER_CACHE_MARKER in driver


def load_mesa_runtime(directory, checksum):
    """Read a Mesa UWP workflow artifact pinned by the SHA-256 of its SHA256SUMS file."""
    if not re.fullmatch(r'[0-9a-f]{64}', checksum or ''):
        raise ValueError('Pin the Mesa UWP runtime with the SHA-256 of its SHA256SUMS file.')
    sums = (directory / 'SHA256SUMS').read_bytes()
    if hashlib.sha256(sums).hexdigest() != checksum:
        raise ValueError('The Mesa UWP runtime artifact differs from its pinned SHA256SUMS digest.')
    expected = {}
    for line in sums.decode('ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._-]+)', line)
        if not match or match[2] in expected:
            raise ValueError('Invalid Mesa UWP runtime SHA256SUMS.')
        expected[match[2]] = match[1]
    if sorted(expected) != sorted(RUNTIME_FILES):
        raise ValueError('The Mesa UWP runtime artifact must list exactly: ' + ', '.join(RUNTIME_FILES))
    files = {}
    for name, digest in expected.items():
        path = directory / name
        if path.stat().st_size > RUNTIME_LIMIT:
            raise ValueError(f'Mesa UWP runtime file {name} is too large.')
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'Mesa UWP runtime file {name} differs from SHA256SUMS.')
        if name.endswith('.dll') and (data[:2] != b'MZ' or b'PE\0\0\x64\x86' not in data[:4096]):
            raise ValueError(f'Mesa UWP runtime file {name} is not an x64 PE image.')
        files[name] = data
    if hashlib.sha256(files['LICENSE.mesa-uwp']).hexdigest() != RUNTIME_LICENSE_SHA256:
        raise ValueError('Use the Mesa UWP license from the pinned source revision.')
    if files['mesa-uwp-source.txt'].decode('ascii').splitlines()[:1] != [RUNTIME_SOURCE]:
        raise ValueError('The Mesa UWP runtime artifact was not built from the pinned source revision.')
    return files


def stage_mesa_runtime(layout, directory, checksum):
    """Replace the base package's prebuilt Mesa UWP runtime with a pinned workflow build."""
    files = load_mesa_runtime(directory, checksum)
    if any(not (layout / name).is_file() for name in LIBRARIES):
        raise ValueError('The base package does not carry the Mesa UWP runtime to replace.')
    for name, data in files.items():
        (layout / name).write_bytes(data)
    return has_shader_cache(files['libgallium_wgl.dll'])


def stage_mesa(layout, archive, license_file, framework, sdl_surfaceless=False, slay_mesa_jni=None,
               dxil_archive=None, indexed_control=False, desktop_gl_runtime=False):
    release = RELEASES.get(hashlib.sha256(archive.read_bytes()).hexdigest())
    if release is None:
        raise ValueError('Use a pinned, unchanged Mesa UWP release archive.')
    source, license_hash, prefix, extra_libraries = release
    libraries = LIBRARIES + extra_libraries
    license_data = license_file.read_bytes()
    if hashlib.sha256(license_data).hexdigest() != license_hash:
        raise ValueError('Use the Mesa UWP license from the pinned source revision.')
    manifest_file = layout / 'AppxManifest.xml'
    manifest = manifest_file.read_bytes()
    identity = ET.fromstring(manifest).find('{*}Identity')
    if (identity is None or identity.get('Name') != 'XboxPortabilityProbe.ChildProcess.SteamCore'
            or identity.get('ProcessorArchitecture') != 'x64'):
        raise ValueError('Use an x64 Steam Core diagnostic layout.')
    target = []
    hosted_stardew = (layout / 'steam-sdl-host-413150.txt').is_file()
    hosted_slay = (layout / 'steam-sdl-host-646570.txt').is_file()
    steam_core = (layout / 'steamclient64.dll').is_file()
    if desktop_gl_runtime and (not steam_core or not (layout / 'steam-signin.html').is_file() or dxil_archive is None):
        raise ValueError('The shared desktop OpenGL runtime requires a Steam sign-in layout and the pinned DXIL validator.')
    if steam_core:
        launch_marker = layout / 'steam-launch-app.txt'
        if launch_marker.is_file():
            target = launch_marker.read_text(encoding='ascii').splitlines()
        if not desktop_gl_runtime and ((not target and not hosted_stardew and not hosted_slay)
                                       or (target and target[0] not in ('413150', '646570'))):
            raise ValueError('Scope this Mesa game trial to Stardew Valley or Slay the Spire.')
    if indexed_control and (desktop_gl_runtime or target or hosted_stardew or hosted_slay):
        raise ValueError('Run the indexed Mesa control without Steam or game files.')
    if hosted_stardew and (not sdl_surfaceless or dxil_archive is None):
        raise ValueError('The hosted Stardew profile requires SDL surfaceless GL and the pinned DXIL validator.')
    if hosted_slay and dxil_archive is None:
        raise ValueError('The hosted Slay profile requires the pinned DXIL validator.')
    if sdl_surfaceless and ((not hosted_stardew and (target[:1] != ['413150'] or 'repeat' not in target))
                            or not (layout / 'StardewStartupDiagnostic.dll').is_file()):
        raise ValueError('Use the repeatable Stardew Valley managed diagnostic for the SDL surfaceless trial.')
    jni_name = 'mesa-jni-control.dll'
    if slay_mesa_jni is not None:
        if (target[:1] != ['646570'] or 'repeat' not in target or not all(
                (layout / filename).is_file() for filename in ('SlayConsoleDiagnostic.class', 'slay-working-directory.dll'))):
            raise ValueError('Use the repeatable Slay the Spire console diagnostic for the Mesa JNI test.')
        jni_library = (slay_mesa_jni / jni_name).read_bytes()
        expected = f'{hashlib.sha256(jni_library).hexdigest()}  {jni_name}'
        if (slay_mesa_jni / 'SHA256SUMS').read_text(encoding='ascii').strip() != expected:
            raise ValueError('Use the verified Mesa JNI control workflow artifact.')
    dxil_files = {}
    if dxil_archive is not None:
        if hashlib.sha256(dxil_archive.read_bytes()).hexdigest() != DXIL_ARCHIVE_SHA256:
            raise ValueError('Use the pinned Microsoft DirectX Shader Compiler v1.8.2505.1 archive.')
        with ZipFile(dxil_archive) as dxc:
            dxil_files = {name: dxc.read(entry) for name, entry in DXIL_FILES.items()}
    staged_names = (*libraries, 'LICENSE.mesa-uwp', 'mesa-uwp-source.txt', *dxil_files)
    if indexed_control:
        staged_names += ('mesa-indexed-control.txt',)
    if (manifest.count(b'</Dependencies>') != 1 or f'Name="{UWP_FRAMEWORK}"'.encode() in manifest
            or any((layout / name).exists() for name in staged_names)):
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
        for name in libraries:
            with (layout / name).open('xb') as output:
                output.write(mesa.read(prefix + name))
    for name, data in dxil_files.items():
        with (layout / name).open('xb') as output:
            output.write(data)
    dependency = f'  <PackageDependency Name="{UWP_FRAMEWORK}" Publisher="{MICROSOFT}" MinVersion="{version}" />\n  '
    manifest_file.write_bytes(manifest.replace(b'</Dependencies>', dependency.encode('ascii') + b'</Dependencies>'))
    (layout / 'LICENSE.mesa-uwp').write_bytes(license_data)
    (layout / 'mesa-uwp-source.txt').write_text(source + '\n', encoding='ascii')
    if sdl_surfaceless:
        (layout / 'stardew-sdl-surfaceless.txt').touch(exist_ok=False)
    if indexed_control:
        (layout / 'mesa-indexed-control.txt').touch(exist_ok=False)
    if slay_mesa_jni is not None:
        with (layout / jni_name).open('xb') as output:
            output.write(jni_library)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout', type=Path)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--license', required=True, type=Path)
    parser.add_argument('--framework', required=True, type=Path)
    parser.add_argument('--sdl-surfaceless', action='store_true', help='enable the SDL offscreen context trial')
    parser.add_argument('--slay-mesa-jni', type=Path, help='verified Mesa JNI control workflow artifact')
    parser.add_argument('--dxil-archive', type=Path, help='pinned Microsoft DXC validator archive for indexed rendering')
    parser.add_argument('--indexed-control', action='store_true', help='verify a completed indexed draw and pixel readback without Steam')
    parser.add_argument('--desktop-gl-runtime', action='store_true',
                        help='stage the shared desktop OpenGL runtime for a Steam sign-in package')
    args = parser.parse_args()
    stage_mesa(args.layout, args.archive, args.license, args.framework, args.sdl_surfaceless, args.slay_mesa_jni,
               args.dxil_archive, args.indexed_control, args.desktop_gl_runtime)
    print('Staged the pinned Mesa UWP runtime and framework dependency.')


if __name__ == '__main__':
    try:
        main()
    except (BadZipFile, ET.ParseError, OSError, ValueError) as error:
        sys.exit(f'Error: {error}')
