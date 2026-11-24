"""Prepare the isolated WWA control using only the pinned native SDK and local test files."""
import hashlib
from pathlib import Path
import shutil
import sys
from xml.etree import ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from build_tools.paths import ROOT
from build_tools.stage_webview import PUBLISHER, SDK_FILES, WEB_CLASSES

root = Path(__file__).resolve().parent
template = ROOT / "ui/big-picture/AppxManifest.xml"
layout, sdk = map(Path, sys.argv[1:])
manifest = ET.parse(template)
for _, (prefix, uri) in ET.iterparse(template, events=["start-ns"]):
    ET.register_namespace(prefix, uri)
package = manifest.getroot()
ns = package.tag.rsplit("}", 1)[0] + "}"
package.find(ns + "Identity").set("Name", "XboxPortabilityProbe.WindowedWwaControl")
package.find(ns + "Identity").set("Version", "1.0.0.1")
title = "WWA Windowed Chromium Control"
package.find(ns + "Properties/" + ns + "DisplayName").text = title
app = package.find(ns + "Applications/" + ns + "Application")
app.set("StartPage", "host.html")
visual = app.find("{*}VisualElements")
visual.set("DisplayName", title)
visual.set("Description", "Isolated main CoreWindow and windowed WebView2 activation control")
capabilities = package.find(ns + "Capabilities")
package.remove(capabilities)
for name in ("Microsoft.VCLibs.140.00", "Microsoft.VCLibs.140.00.UWPDesktop"):
    ET.SubElement(package.find(ns + "Dependencies"), ns + "PackageDependency", Name=name,
                  Publisher=PUBLISHER, MinVersion="14.0.0.0")
extensions = package.find(ns + "Extensions")
server = extensions.find(ns + "Extension/" + ns + "InProcessServer")
server.find(ns + "Path").text = "WindowedControl.Native.dll"
server.find(ns + "ActivatableClass").set("ActivatableClassId", "WindowedControl.Native.Control")
extension = ET.SubElement(extensions, ns + "Extension", Category="windows.activatableClass.inProcessServer")
server = ET.SubElement(extension, ns + "InProcessServer")
ET.SubElement(server, ns + "Path").text = "Microsoft.Web.WebView2.Core.dll"
for name in WEB_CLASSES:
    ET.SubElement(server, ns + "ActivatableClass", ActivatableClassId="Microsoft.Web.WebView2.Core.CoreWebView2" + name,
                  ThreadingModel="both")
for name, digest in SDK_FILES.items():
    source = sdk / ("lib" if name.endswith(".winmd") else "runtimes/win-x64/native_uap") / name
    if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
        raise ValueError("Wrong SDK binary for " + name)
    shutil.copyfile(source, layout / name)
manifest.write(layout / "AppxManifest.xml", encoding="utf-8", xml_declaration=True)
shutil.copytree(ROOT / "ui/Assets", layout / "Assets")
for name in ("host.js", "host.html"):
    shutil.copyfile(root / name, layout / name)
