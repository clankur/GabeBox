"""Prepare the isolated native XAML mouse-capture APPX layout."""

import argparse
from pathlib import Path
import re
import shutil
from xml.etree import ElementTree as ET

from build_tools.paths import ROOT
from build_tools.redirect_pe_imports import PeImage
from probe.catalog import NATIVE_MOUSE_PROBE_NAME


MANIFEST = ROOT / 'probe/Package.appxmanifest'
DISPLAY_NAME = 'Xbox Mouse Capture'
MICROSOFT = 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US'


def prepare(layout, executable, version, *, controller_control=False):
    if not re.fullmatch(r'[0-9]{1,5}(?:\.[0-9]{1,5}){3}', version) or any(
            int(part) > 65535 for part in version.split('.')):
        raise ValueError('Use a four-component APPX version with components from 0 to 65535.')
    image = PeImage(executable.read_bytes(), read_only=True)
    flags = image.short(image.word(0x3c) + 22)
    subsystem = image.short(image.optional + 68)
    dll_flags = image.short(image.optional + 70)
    if not flags & 0x0002 or flags & 0x2000 or subsystem != 2 or not dll_flags & 0x1000:
        raise ValueError('Use an x64 Windows GUI AppContainer executable.')
    for _, (prefix, uri) in ET.iterparse(MANIFEST, events=['start-ns']):
        ET.register_namespace(prefix, uri)
    manifest = ET.parse(MANIFEST)
    root = manifest.getroot()
    ns = root.tag.rsplit('}', 1)[0] + '}'
    root.find(ns + 'Identity').attrib.update(Name=NATIVE_MOUSE_PROBE_NAME, Version=version)
    root.find(ns + 'Properties/' + ns + 'DisplayName').text = DISPLAY_NAME
    app = root.find(ns + 'Applications/' + ns + 'Application')
    app.attrib.update(Executable='MouseSource.exe', EntryPoint='MouseSource.App')
    app.find('{*}VisualElements').attrib.update(
        DisplayName=DISPLAY_NAME, Description='Physical mouse capture on the foreground XAML view')
    dependencies = root.find(ns + 'Dependencies')
    for name, minimum in (('Microsoft.VCLibs.140.00.UWPDesktop', '14.0.33321.0'),
                          ('Microsoft.VCLibs.140.00', '14.0.33519.0')):
        ET.SubElement(dependencies, ns + 'PackageDependency', Name=name, Publisher=MICROSOFT, MinVersion=minimum)
    root.remove(root.find(ns + 'Capabilities'))

    layout.mkdir()
    manifest.write(layout / 'AppxManifest.xml', encoding='utf-8', xml_declaration=True)
    shutil.copytree(ROOT / 'ui/Assets', layout / 'Assets')
    shutil.copyfile(executable, layout / 'MouseSource.exe')
    (layout / 'fhd-mouse-source.txt').touch()
    if controller_control:
        (layout / 'mouse-controller-control.txt').touch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path, help='x64 APPX executable built from native-app.cpp')
    parser.add_argument('version', help='four-component APPX version')
    parser.add_argument('layout', type=Path, help='new layout directory')
    parser.add_argument('--controller-control', action='store_true', help='enable Xbox controller pointer emulation')
    args = parser.parse_args()
    try:
        prepare(args.layout, args.executable, args.version, controller_control=args.controller_control)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Error: {error}\n')


if __name__ == '__main__':
    main()
