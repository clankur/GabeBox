#!/usr/bin/env python3
"""Test selected Steam imports against the x64 Windows SDK at link time only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZipFile

HERE = Path(__file__).resolve().parent
SDK_VERSION = "10.0.26100.8249"
SDK_SHA256 = "dba411916afdd47c99fecdf7d5558df7679d291dc338746590ae9adf5fd0511b"
SYSTEM_LIBS = {"kernel32.dll", "user32.dll", "advapi32.dll", "shell32.dll"}


def link_probe(linker, objdump, library, function, directory):
    output = directory / f"{function}-{library.stem}.dll"
    cmd = [linker, "/dll", "/noentry", "/appcontainer", "/machine:x64", "/nodefaultlib",
           f"/include:__imp_{function}", f"/out:{output}", str(library)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode:
        if "undefined symbol:" in proc.stderr and function in proc.stderr:
            return None
        raise RuntimeError(proc.stderr or proc.stdout)
    dump = subprocess.check_output([objdump, "-p", str(output)], text=True)
    current_dll = None
    for line in dump.splitlines():
        if "DLL Name:" in line:
            current_dll = line.partition("DLL Name:")[2].strip()
        elif re.search(r"\s" + re.escape(function) + r"$", line.strip()):
            if current_dll and "APPCONTAINER" in dump:
                return current_dll
    raise ValueError(f"Linked file did not contain expected import or AppContainer flag: {function}")


def run(sdk, linker, objdump):
    with sdk.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SDK_SHA256:
            raise ValueError(f"Expected official microsoft.windows.sdk.cpp.x64 {SDK_VERSION}")
    uses = {}
    for component in json.loads((HERE / "client-imports.json").read_text())["components"]:
        for item in component["selected_imports"]:
            module = item["module"].lower()
            if module in SYSTEM_LIBS:
                uses.setdefault((module, item["function"]), set()).add(component["member"])
    rows = []
    with tempfile.TemporaryDirectory() as tmp, ZipFile(sdk) as archive:
        directory = Path(tmp)
        members = {name.lower(): name for name in archive.namelist()}
        libraries = {}
        for name in ["windowsapp", *sorted({Path(module).stem for module, _ in uses})]:
            lib = directory / f"{name}.lib"
            lib.write_bytes(archive.read(members[f"c/um/x64/{name}.lib"]))
            libraries[name] = lib
        for (module, function), components in sorted(uses.items()):
            desktop = link_probe(linker, objdump, libraries[Path(module).stem], function, directory)
            if desktop is None:
                raise ValueError(f"Desktop control failed for {module}!{function}")
            uwp = link_probe(linker, objdump, libraries["windowsapp"], function, directory)
            rows.append({"function": function, "components": sorted(components),
                         "desktop_import_dll": desktop, "windowsapp_import_dll": uwp})
    return {"sdk_x64": SDK_VERSION, "sdk_sha256": SDK_SHA256, "results": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path, help="Pinned Windows SDK CPP x64 NuGet package")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()
    print(json.dumps(run(args.sdk, args.linker, args.objdump), indent=2))
