"""Stage the WebView2 composition host in an unpacked x64 WWA package."""

import hashlib
from pathlib import Path
import re
from xml.etree import ElementTree as ET

SDK_VERSION = "1.0.3537.50"
SDK_SHA256 = "5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa"
SDK_FILES = {
    "Microsoft.Web.WebView2.Core.dll": "84126336b10674852cfdfe03d4d097bf3bacb695b457ead2dffe0bed035c1cdf",
    "Microsoft.Web.WebView2.Core.winmd": "1057948ce96da24934d723276ecf78871369c89acc815f75cf67caa61f6f43b9",
}
HOST_FILES = ("BigPictureView.Native.dll", "BigPictureView.Native.winmd")
# Static, composable and activatable classes exported by this SDK's UAP WinMD.
WEB_CLASSES = ("ControllerWindowReference", "Controller", "Environment", "CustomSchemeRegistration",
               "EnvironmentOptions", "CompositionController")
PUBLISHER = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"


def add_webview_host(layout, host_artifact):
    layout, host_artifact = Path(layout), Path(host_artifact)
    proofs = {}
    for line in (host_artifact / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([a-fA-F0-9]{64})\s+\*?([^/\\]+)", line.strip())
        if match:
            proofs[match[2]] = match[1].lower()
    payload = {}
    for name in (*HOST_FILES, *SDK_FILES):
        data = (host_artifact / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if proofs.get(name) != digest or digest != SDK_FILES.get(name, digest):
            raise ValueError(f"Unexpected WebView2 host artifact digest for {name}")
        payload[name] = data

    manifest = layout / "AppxManifest.xml"
    for _, (prefix, uri) in ET.iterparse(manifest, events=("start-ns",)):
        if not re.fullmatch(r"ns\d+", prefix):
            ET.register_namespace(prefix, uri)
    tree = ET.parse(manifest)
    root = tree.getroot()
    ns = root.tag.rsplit("}", 1)[0] + "}"
    identity = root.find(ns + "Identity")
    if identity is None or identity.get("ProcessorArchitecture") != "x64":
        raise ValueError("WebView2 host requires an x64 package")
    deps = root.find(ns + "Dependencies")
    if deps is None:
        raise ValueError("Missing package dependencies")
    framework = "Microsoft.VCLibs.140.00.UWPDesktop"
    if not any(d.get("Name") == framework for d in deps):
        ET.SubElement(deps, ns + "PackageDependency", Name=framework, Publisher=PUBLISHER, MinVersion="14.0.0.0")
    extensions = root.find(ns + "Extensions")
    if extensions is None:
        extensions = ET.SubElement(root, ns + "Extensions")
    servers = {e.findtext(ns + "Path"): e for e in extensions.iter(ns + "InProcessServer")}
    for dll, classes in (("BigPictureView.Native.dll", ["BigPictureView.Native.Renderer"]),
                         ("Microsoft.Web.WebView2.Core.dll", ["Microsoft.Web.WebView2.Core.CoreWebView2" + x for x in WEB_CLASSES])):
        if dll in servers:
            server = servers[dll]
        else:
            extension = ET.SubElement(extensions, ns + "Extension", Category="windows.activatableClass.inProcessServer")
            server = ET.SubElement(extension, ns + "InProcessServer")
            ET.SubElement(server, ns + "Path").text = dll
        current = {item.get("ActivatableClassId") for item in server.findall(ns + "ActivatableClass")}
        for name in classes:
            if name not in current:
                ET.SubElement(server, ns + "ActivatableClass", ActivatableClassId=name, ThreadingModel="both")
    namespaces = dict(value for _, value in ET.iterparse(manifest, events=("start-ns",)))
    used = ET.tostring(root, encoding="unicode")
    for prefix in root.get("IgnorableNamespaces", "").split():
        if f"xmlns:{prefix}=" not in used:
            root.set("xmlns:" + prefix, namespaces[prefix])
    payload["big-picture-webview.js"] = Path(__file__).with_name("big-picture-webview.js").read_bytes()
    for name, data in payload.items():
        (layout / name).write_bytes(data)
    tree.write(manifest, encoding="utf-8", xml_declaration=True)
