"""Build an isolated, account-free XAML activation layout from a pinned Windows executable."""
import argparse
from pathlib import Path
import shutil
from xml.etree import ElementTree as ET


from build_tools.paths import ROOT
from build_tools.stage_primary import stage_primary


CONTROL = ROOT / "probe/big-picture-primary"
MANIFEST = ROOT / "ui/big-picture/AppxManifest.xml"


def prepare(layout, artifact, checksum, version):
    manifest = ET.parse(MANIFEST)
    ns = manifest.getroot().tag.rsplit("}", 1)[0] + "}"
    for _, (prefix, uri) in ET.iterparse(MANIFEST, events=["start-ns"]):
        ET.register_namespace(prefix, uri)
    identity = manifest.find(ns + "Identity")
    identity.set("Name", "XboxPortabilityProbe.SteamPrimaryControl")
    identity.set("Version", version)
    manifest.find(ns + "Properties/" + ns + "DisplayName").text = "Steam Primary Activation Control"
    app = manifest.find(ns + "Applications/" + ns + "Application")
    app.set("StartPage", "steam-signin.html")
    visual = app.find("{*}VisualElements")
    visual.set("DisplayName", "Steam Primary Activation Control")
    visual.set("Description", "Isolated UWP XAML startup and local Windows Runtime projection test")
    package = manifest.getroot()
    for node in (package.find(ns + "Capabilities"), package.find(ns + "Extensions")):
        if node is not None:
            package.remove(node)
    ET.SubElement(package.find(ns + "Dependencies"), ns + "PackageDependency", {
        "Name": "Microsoft.VCLibs.140.00", "Publisher": "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US",
        "MinVersion": "14.0.0.0"})
    layout.mkdir()
    manifest.write(layout / "AppxManifest.xml", encoding="utf-8", xml_declaration=True)
    shutil.copytree(ROOT / "ui/Assets", layout / "Assets")
    (layout / "steam-signin.html").write_text(
        '<!doctype html><html lang="en" data-steam-big-picture="true"><meta charset="utf-8">'
        '<title>Steam primary activation control</title><p id="status">Loading local projection…</p>'
        '<script src="activation-check.js"></script></html>', encoding="utf-8")
    shutil.copy2(CONTROL / "activation-check.js", layout / "activation-check.js")
    stage_primary(layout, artifact, checksum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("layout", type=Path)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("checksum")
    parser.add_argument("--version", default="1.0.0.2")
    args = parser.parse_args()
    prepare(args.layout, args.artifact, args.checksum, args.version)


if __name__ == "__main__":
    main()
