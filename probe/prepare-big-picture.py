#!/usr/bin/env python3
"""Prepare the native Big Picture APPX layout from the Windows SDK template."""
import argparse
from pathlib import Path
import re
import runpy
import shutil
from tempfile import TemporaryDirectory
from xml.etree import ElementTree
from zipfile import ZipFile


STAGER = Path(__file__).resolve().parents[1] / "analysis" / "stage-pinned-client.py"
stage_client = runpy.run_path(str(STAGER))["stage_client"]
HOST_FILES = {"AppxManifest.xml", "big-picture.html", "big-picture.js",
              "BigPicture.Native.dll", "BigPicture.Native.winmd", "Assets/Logo.png",
              "Assets/SmallLogo.png", "Assets/Splash.png", "Assets/StoreLogo.png"}


def prepare_layout(template, layout, archive_cache=None, webview_host=None, version=None):
    if layout.exists():
        raise FileExistsError(f"Use a new layout directory: {layout}")
    with TemporaryDirectory(dir=layout.parent) as temp:
        staged = Path(temp) / "layout"
        staged.mkdir()
        with ZipFile(template) as package:
            manifest = ElementTree.fromstring(package.read("AppxManifest.xml"))
            identity = manifest.find("{*}Identity")
            apps = manifest.findall("{*}Applications/{*}Application")
            if (identity is None or identity.get("Name") != "XboxPortabilityProbe.BigPicture"
                    or identity.get("ProcessorArchitecture") != "x64"
                    or identity.get("Publisher") != "CN=Xbox Portability Probe"
                    or len(apps) != 1 or apps[0].get("Id") != "App"
                    or apps[0].get("StartPage") != "big-picture.html"
                    or "Executable" in apps[0].attrib or "EntryPoint" in apps[0].attrib):
                raise ValueError("Use the x64 Big Picture native launcher SDK template.")
            servers = manifest.findall("{*}Extensions/{*}Extension/{*}InProcessServer")
            if not any(server.findtext("{*}Path") == "BigPicture.Native.dll"
                       and server.find("{*}ActivatableClass[@ActivatableClassId='BigPicture.Native.Launcher']") is not None
                       for server in servers):
                raise ValueError("The template must register the native Big Picture launcher.")
            if manifest.find("{*}Capabilities/{*}Capability[@Name='internetClient']") is None:
                raise ValueError("The template must declare the internetClient capability.")
            names = {name.replace("\\", "/") for name in package.namelist()}
            if not HOST_FILES.union({"AppxSignature.p7x"}) <= names:
                raise ValueError("The signed Big Picture SDK template is incomplete.")
            # Rebuild the package footprint at signing time. Include only the SDK
            # launcher, its manifest, resources and tile art from the template.
            for member in package.infolist():
                name = member.filename.replace("\\", "/")
                if name not in HOST_FILES and name != "resources.pri" and not name.startswith("Assets/"):
                    continue
                if ".." in name.split("/"):
                    raise ValueError(f"Invalid template asset path: {name}")
                if member.is_dir():
                    continue
                output = staged / name
                output.parent.mkdir(parents=True, exist_ok=True)
                with package.open(member) as source, output.open("xb") as target:
                    shutil.copyfileobj(source, target)

        if webview_host is not None:
            version = version or "1.0.0.1"
            if not re.fullmatch(r"1\.0\.0\.[1-9][0-9]{0,4}", version) or int(version.split(".")[-1]) > 65535:
                raise ValueError("Use a WebView test version between 1.0.0.1 and 1.0.0.65535.")
            host = Path(__file__).with_name("big-picture-webview")
            for name in ("smoke.html", "smoke.js"):
                shutil.copyfile(host / name, staged / name)
            manifest_path = staged / "AppxManifest.xml"
            original = manifest_path.read_bytes()
            if original.count(b' Version="1.0.0.0"') != 1:
                raise ValueError("Expected the original 1.0.0.0 Big Picture SDK template.")
            manifest_path.write_bytes(original.replace(b' Version="1.0.0.0"', f' Version="{version}"'.encode())
                                      .replace(b'StartPage="big-picture.html"', b'StartPage="smoke.html"'))
            runpy.run_path(str(host / "stage.py"))["add_webview_host"](staged, webview_host)
        elif version is not None:
            raise ValueError("--version applies to the WebView test package.")
        stage_client(staged / "ValveClient", archive_cache, ui_only=webview_host is not None)
        staged.rename(layout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("template", type=Path, help="BigPictureTemplate-x64.appx")
    parser.add_argument("layout", type=Path, help="new local APPX layout directory")
    parser.add_argument("--archive-cache", type=Path,
                        help="read all pinned Valve ZIP filenames from this directory offline")
    parser.add_argument("--webview-host", type=Path,
                        help="build a separate device test of the original Valve document with this x64 host")
    parser.add_argument("--version", help="four-part version for a WebView test package upgrade")
    args = parser.parse_args()
    prepare_layout(args.template, args.layout, args.archive_cache, args.webview_host, args.version)
