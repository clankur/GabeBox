"""Validate and stage the native FHD presenter."""

import hashlib
import re
from xml.etree import ElementTree


FHD_COMPONENT = ('Fhd.Native.dll', 'Fhd.Native.winmd')


def presenter_payload(artifact, checksum):
    """Verify both members against a pinned CI checksum manifest before staging."""
    if not re.fullmatch(r'[0-9a-fA-F]{64}', checksum or ''):
        raise ValueError('Provide the SHA-256 of the presenter artifact SHA256SUMS file.')
    sums = (artifact / 'SHA256SUMS').read_bytes()
    if hashlib.sha256(sums).hexdigest() != checksum.lower():
        raise ValueError('Presenter checksum manifest differs from the supplied SHA-256.')
    expected = {}
    for line in sums.decode('ascii').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{64})  (Fhd\.Native\.(?:dll|winmd))', line)
        if not match or match[2] in expected:
            raise ValueError('Unexpected presenter checksum entry.')
        expected[match[2]] = match[1].lower()
    if expected.keys() != set(FHD_COMPONENT):
        raise ValueError('Presenter checksum manifest must cover the DLL and WinMD.')
    payload = {name: (artifact / name).read_bytes() for name in FHD_COMPONENT}
    if any(hashlib.sha256(data).hexdigest() != expected[name] for name, data in payload.items()):
        raise ValueError('Presenter component does not match its checksum manifest.')
    return payload


def presenter_manifest(manifest):
    root = ElementTree.fromstring(manifest)
    servers = root.findall('{*}Extensions/{*}Extension/{*}InProcessServer')
    for server in servers:
        for klass in server.findall('{*}ActivatableClass'):
            if klass.get('ActivatableClassId') == 'Fhd.Native.RenderHost':
                if server.findtext('{*}Path') != FHD_COMPONENT[0] or klass.get('ThreadingModel') != 'both':
                    raise ValueError('The template has a conflicting native presenter registration.')
                return manifest
    # Insert beside the existing package-level ProbeNative registration, without rewriting
    # the namespace declarations referenced by IgnorableNamespaces.
    server_pattern = rb'<Extension\b[^>]*>[^<]*<InProcessServer>.*?</InProcessServer>\s*</Extension>'
    registrations = [m for m in re.finditer(server_pattern, manifest, re.DOTALL)
                     if b'<Path>ProbeNative.dll</Path>' in m[0]]
    if len(registrations) != 1:
        raise ValueError('Expected exactly one native probe server in this SDK template.')
    end = registrations[0].end()
    extra = (b'\n    <Extension Category="windows.activatableClass.inProcessServer">'
             b'<InProcessServer><Path>Fhd.Native.dll</Path><ActivatableClass '
             b'ActivatableClassId="Fhd.Native.RenderHost" ThreadingModel="both" />'
             b'</InProcessServer></Extension>')
    return manifest[:end] + extra + manifest[end:]


def stage_native_presenter(layout, artifact, checksum):
    payload = presenter_payload(artifact, checksum)
    path = layout / 'AppxManifest.xml'
    manifest = presenter_manifest(path.read_bytes())
    for name, data in payload.items():
        (layout / name).write_bytes(data)
    path.write_bytes(manifest)
