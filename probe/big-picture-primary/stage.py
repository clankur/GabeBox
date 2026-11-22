"""Select the XAML primary view only for an explicitly requested integrated package."""
import hashlib
from pathlib import Path
import re
from xml.etree import ElementTree as ET

EXE = "SteamPrimary.exe"
ENTRY = "BigPicture.Primary.App"
PAGE = "ms-appx-web:///steam-signin.html"
UAP = "{http://schemas.microsoft.com/appx/manifest/uap/windows10}"


def stage_primary(layout: Path, artifact: Path, checksum: str):
    manifest = layout / "AppxManifest.xml"
    sums = (artifact / "SHA256SUMS").read_bytes()
    if hashlib.sha256(sums).hexdigest() != (checksum or "").lower():
        raise ValueError("Primary executable checksum manifest differs from the supplied SHA-256.")
    pin = re.fullmatch(rb"([a-fA-F0-9]{64})  SteamPrimary\.exe\r?\n?", sums)
    data = (artifact / EXE).read_bytes()
    if not pin or hashlib.sha256(data).hexdigest() != pin[1].decode().lower():
        raise ValueError("Primary executable differs from its pinned Windows build.")

    raw = manifest.read_bytes()
    apps = ET.fromstring(raw).findall("./{*}Applications/{*}Application")
    if (len(apps) != 1 or apps[0].attrib != {"Id": "App", "StartPage": "steam-signin.html"}
            or apps[0].find(UAP + "ApplicationContentUriRules") is not None):
        raise ValueError("Expected the single existing Steam Core packaged web application.")
    before = b'StartPage="steam-signin.html"'
    end = b"</uap:VisualElements>"
    if raw.count(before) != 1 or raw.count(end) != 1:
        raise ValueError("Unexpected Steam Core manifest formatting.")
    raw = raw.replace(before, f'Executable="{EXE}" EntryPoint="{ENTRY}"'.encode())
    rule = (f'<uap:ApplicationContentUriRules><uap:Rule Match="{PAGE}" WindowsRuntimeAccess="all" '
            'Type="include"/></uap:ApplicationContentUriRules>')
    raw = raw.replace(end, end + rule.encode())

    html = layout / "steam-signin.html"
    page = html.read_bytes()
    marker = b'data-steam-big-picture="true"'
    charset = b'<meta charset="utf-8">'
    if page.count(marker) != 1 or page.count(charset) != 1:
        raise ValueError("Expected the integrated Steam account entry.")
    # All existing script URLs resolve within the signed package. Remote artwork and
    # the existing sign-in HTTP requests remain available without permitting remote code.
    csp = b"""<meta http-equiv="Content-Security-Policy" content="script-src 'self'; object-src 'none'; frame-src 'none'; base-uri 'none'">"""
    page = page.replace(marker, marker + b' data-steam-primary-xaml="true"')
    with (layout / EXE).open("xb") as output:
        output.write(data)
    html.write_bytes(page.replace(charset, charset + b"\n    " + csp))
    manifest.write_bytes(raw)
