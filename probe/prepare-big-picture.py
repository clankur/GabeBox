#!/usr/bin/env python3
"""Prepare the native Big Picture APPX layout from the Windows SDK template."""
import argparse
from pathlib import Path
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


def prepare_layout(template, layout, archive_cache=None):
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

        stage_client(staged / "ValveClient", archive_cache)
        staged.rename(layout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("template", type=Path, help="BigPictureTemplate-x64.appx")
    parser.add_argument("layout", type=Path, help="new local APPX layout directory")
    parser.add_argument("--archive-cache", type=Path,
                        help="read all pinned Valve ZIP filenames from this directory offline")
    args = parser.parse_args()
    prepare_layout(args.template, args.layout, args.archive_cache)
