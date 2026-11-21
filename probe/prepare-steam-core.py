#!/usr/bin/env python3
"""Prepare a local Steam Core probe APPX layout from the CI template."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from xml.etree import ElementTree
from zipfile import ZipFile


PROBE_ID = 'XboxPortabilityProbe.ChildProcess.SteamCore'
VALVE_FILES = ('steamclient64.dll', 'tier0_s64.dll', 'vstdlib_s64.dll')
APPX_FOOTPRINT = {'appxblockmap.xml', 'appxsignature.p7x', '[content_types].xml',
                  'appxmetadata/codeintegrity.cat'}
ANALYSIS = Path(__file__).resolve().parents[1] / 'analysis'


def system_imports(objdump, binary):
    output = subprocess.run([objdump, '--private-headers', str(binary)], check=True,
                            capture_output=True, text=True).stdout
    _, start, remainder = output.partition('The Import Tables:')
    table, end, _ = remainder.partition('Export Table:')
    if not start or not end:
        raise ValueError(f'Could not read the PE import table of {binary.name}.')
    parsed = {}
    library = None
    for line in table.splitlines():
        module = re.fullmatch(r'\s*DLL Name: ([a-zA-Z0-9_.-]+)', line)
        if module:
            library = module[1].lower()
        elif library and library not in VALVE_FILES:
            entry = re.fullmatch(r'\s+(\d+)(?:\s+(\S+))?\s*', line)
            if entry:
                parsed.setdefault(library, set()).add(entry[2] or '#' + entry[1])
    if not parsed:
        raise ValueError(f'No system imports in {binary.name}.')
    return parsed


def adapt_pinned_keyboard(binary, objdump):
    audit = json.loads((ANALYSIS / 'client-imports.json').read_text())
    pin = next(component for component in audit['components'] if component['member'] == binary.name)
    original = binary.read_bytes()
    if hashlib.sha256(original).hexdigest() != pin['sha256']:
        raise ValueError('The keyboard adaptation applies only to the audited Steam core.')
    before = system_imports(objdump, binary)
    ansi, wide = 'MapVirtualKeyExA', 'MapVirtualKeyExW'
    old_name = ansi.encode('ascii') + b'\0'
    new_name = wide.encode('ascii') + b'\0'
    if (original.count(old_name) != 1 or ansi not in before.get('user32.dll', ())
            or wide in before['user32.dll']):
        raise ValueError('Expected one audited ANSI keyboard import.')
    candidate = original.replace(old_name, new_name, 1)
    binary.write_bytes(candidate)
    before['user32.dll'].remove(ansi)
    before['user32.dll'].add(wide)
    if system_imports(objdump, binary) != before:
        raise ValueError('The derived library changed more than the expected keyboard import.')
    print(f'Derived Steam client SHA-256: {hashlib.sha256(candidate).hexdigest()}.')


def vclibs_dependency(appx):
    framework_name = 'Microsoft.VCLibs.140.00.UWPDesktop'
    required_dlls = {'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'}
    with ZipFile(appx) as framework:
        names = {name.lower() for name in framework.namelist()}
        if 'appxsignature.p7x' not in names or not required_dlls <= names:
            raise ValueError('Use a signed Visual C++ framework containing the required x64 runtime DLLs.')
        manifest = ElementTree.fromstring(framework.read('AppxManifest.xml'))
        identity = manifest.find('{*}Identity')
        if (identity is None or identity.get('Name') != framework_name
                or identity.get('ProcessorArchitecture') != 'x64'):
            raise ValueError(f'Use the {framework_name} x64 framework package.')
        publisher = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'
        version = identity.get('Version', '')
        if (identity.get('Publisher') != publisher or manifest.findtext('{*}Properties/{*}Framework') != 'true'
                or not re.fullmatch(r'14\.0\.\d+\.\d+', version)):
            raise ValueError('Unexpected Microsoft Visual C++ framework manifest.')
    return (f'<PackageDependency Name="{framework_name}" Publisher="{publisher}" '
            f'MinVersion="{version}" />').encode('ascii')


def prepare_layout(template, layout, version, valve_archive, objdump, unicode_keyboard, steam_sign_in,
                   launch_app_id=None, probe_java_runtime=False, repeat_launch=False, install_before_launch=False,
                   library_inventory=False, vclibs_framework=None, slay_startup_log=False, slay_console=False):
    version_parts = tuple(int(part) for part in version.split('.'))
    with ZipFile(template) as signed:
        manifest = signed.read('AppxManifest.xml')
        identity = ElementTree.fromstring(manifest).find('{*}Identity')
        if (identity is None or identity.get('Name') != PROBE_ID
                or identity.get('ProcessorArchitecture') != 'x64'):
            raise ValueError('The input must be the x64 Steam Core Loader Probe template.')
        base_version = identity.attrib['Version']
        if version_parts <= tuple(int(part) for part in base_version.split('.')):
            raise ValueError(f'Use a version higher than the template version {base_version}.')
        entries = [name for name in signed.namelist() if name.lower() not in APPX_FOOTPRINT]
        if any(Path(name).name.lower() in VALVE_FILES for name in entries):
            raise ValueError('The template must contain only probe code and assets.')
        dependency = vclibs_dependency(vclibs_framework) if vclibs_framework else None
        if dependency and manifest.count(b'</Dependencies>') != 1:
            raise ValueError('Use a Steam Core template with a single Dependencies section.')

        if valve_archive:
            release = json.loads((ANALYSIS / 'valve-release.json').read_text())
            pin = next(item for item in release['archives'] if VALVE_FILES[0] in item['members'])
            digest = hashlib.sha256()
            with valve_archive.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            if valve_archive.stat().st_size != pin['size'] or digest.hexdigest() != pin['sha256']:
                raise ValueError(f'Valve archive must match {pin["file"]} from the pinned manifest.')

        layout.mkdir()
        signed.extractall(layout, entries)
        # Preserve manifest namespace declarations used by IgnorableNamespaces.
        old_version = f'Version="{base_version}"'.encode()
        if manifest.count(old_version) != 1:
            raise ValueError('Expected one package version attribute in the CI template.')
        updated = manifest.replace(old_version, f'Version="{version}"'.encode())
        if steam_sign_in:
            if 'steam-signin.html' not in entries or updated.count(b'StartPage="index.html"') != 1:
                raise ValueError('Use a Steam Core template containing the sign-in page.')
            updated = updated.replace(b'StartPage="index.html"', b'StartPage="steam-signin.html"')
        if dependency:
            updated = updated.replace(b'</Dependencies>', b'  ' + dependency + b'\n  </Dependencies>')
        (layout / 'AppxManifest.xml').write_bytes(updated)
        if launch_app_id is not None:
            config = [str(launch_app_id)]
            if repeat_launch:
                config.append('repeat')
            if install_before_launch:
                config.append('install')
            (layout / 'steam-launch-app.txt').write_text('\n'.join(config), encoding='ascii')
        if probe_java_runtime:
            (layout / 'steam-java-probe.txt').touch()
        if slay_startup_log:
            shutil.copyfile(Path(__file__).with_name('slay-startup-log.xml'), layout / 'slay-startup-log.xml')
        if slay_console:
            if 'SlayConsoleDiagnostic.class'.encode('utf-16le') not in (layout / 'ProbeNative.dll').read_bytes():
                raise ValueError('Use the Windows SDK template containing the Slay console diagnostic.')
            subprocess.run(['javac', '--release', '8', '-d', str(layout),
                            str(Path(__file__).with_name('SlayConsoleDiagnostic.java'))], check=True)
        if library_inventory:
            (layout / 'steam-library-inventory.txt').touch()

    if valve_archive:
        with ZipFile(valve_archive) as archive:
            for filename in VALVE_FILES:
                with archive.open(filename) as source, (layout / filename).open('xb') as output:
                    shutil.copyfileobj(source, output)

        if unicode_keyboard:
            adapt_pinned_keyboard(layout / VALVE_FILES[0], objdump)

        imports = {}
        for filename in VALVE_FILES:
            for module, symbols in system_imports(objdump, layout / filename).items():
                imports.setdefault(module, set()).update(symbols)
        (layout / 'steam-core-imports.txt').write_text(''.join(
            f"{module} {' '.join(sorted(symbols))}\n" for module, symbols in sorted(imports.items())),
            encoding='ascii')
    print(f'Prepared version {version} in {layout} ({len(VALVE_FILES) if valve_archive else 0} Valve DLLs).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('template', type=Path)
    parser.add_argument('layout', type=Path, help='new directory outside the repository')
    parser.add_argument('--version', required=True)
    parser.add_argument('--valve-archive', type=Path, help='pinned bins_win64 ZIP, omit for the repack control')
    parser.add_argument('--objdump', default='llvm-objdump', help='path to LLVM objdump')
    parser.add_argument('--unicode-keyboard', action='store_true',
                        help='test a local Unicode import adaptation of the pinned Steam core')
    parser.add_argument('--steam-sign-in', action='store_true', help='start the interactive Steam Guard page')
    parser.add_argument('--launch-app-id', type=int, help='test one installed Steam game after sign-in')
    parser.add_argument('--repeat-launch', action='store_true',
                        help='run the launch test again each time the probe starts from a fresh process')
    parser.add_argument('--install-before-launch', action='store_true',
                        help='ask Steam to install the selected test game before Play if needed')
    parser.add_argument('--library-inventory', action='store_true',
                        help='save game names and AppIDs in the app’s local data for compatibility testing')
    parser.add_argument('--vclibs-framework', type=Path,
                        help='add a dependency on a signed Microsoft.VCLibs.140.00.UWPDesktop x64 framework APPX')
    parser.add_argument('--probe-java-runtime', action='store_true',
                        help='run the installed Slay the Spire Java version check after a startup exit')
    parser.add_argument('--slay-startup-log', action='store_true',
                        help='collect Slay the Spire startup logging during normal Steam Play')
    parser.add_argument('--slay-console', action='store_true',
                        help='capture the publisher console diagnostic after Slay the Spire exits with code 1')
    args = parser.parse_args()
    if args.steam_sign_in and not args.unicode_keyboard:
        parser.error('--steam-sign-in requires --unicode-keyboard')
    if args.unicode_keyboard and not args.valve_archive:
        parser.error('--unicode-keyboard requires --valve-archive')
    if args.launch_app_id is not None and (not args.steam_sign_in or not 0 < args.launch_app_id <= 0xFFFFFF):
        parser.error('--launch-app-id requires --steam-sign-in and a Steam AppID from 1 to 16777215')
    if args.probe_java_runtime and args.launch_app_id != 646570:
        parser.error('--probe-java-runtime requires --launch-app-id 646570')
    if args.slay_startup_log and args.launch_app_id != 646570:
        parser.error('--slay-startup-log requires --launch-app-id 646570')
    if args.slay_console and args.launch_app_id != 646570:
        parser.error('--slay-console requires --launch-app-id 646570')
    if args.repeat_launch and args.launch_app_id is None:
        parser.error('--repeat-launch requires --launch-app-id')
    if args.install_before_launch and args.launch_app_id is None:
        parser.error('--install-before-launch requires --launch-app-id')
    if args.library_inventory and not args.steam_sign_in:
        parser.error('--library-inventory requires --steam-sign-in')
    if not re.fullmatch(r'[0-9]{1,5}(?:\.[0-9]{1,5}){3}', args.version):
        parser.error('--version must have four integer components')
    if any(int(part) > 65535 for part in args.version.split('.')):
        parser.error('--version components may not exceed 65535')
    prepare_layout(args.template, args.layout, args.version, args.valve_archive, args.objdump, args.unicode_keyboard,
                   args.steam_sign_in, args.launch_app_id, args.probe_java_runtime, args.repeat_launch,
                   args.install_before_launch, args.library_inventory, args.vclibs_framework, args.slay_startup_log,
                   args.slay_console)


if __name__ == '__main__':
    main()
