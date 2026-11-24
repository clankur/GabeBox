#!/usr/bin/env python3
"""Read Valve's pinned client archives and report native portability dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.request import urlopen
from zipfile import ZipFile

HERE = Path(__file__).resolve().parent
CDN = "https://client-update.akamai.steamstatic.com/"
PORTING_APIS = {
    "CreateProcessW", "CreateProcessA", "OpenSCManagerW", "CreateServiceW",
    "StartServiceW", "StartServiceCtrlDispatcherW", "CreateWindowExW",
    "RegisterClassExW", "CreateDesktopW", "ShellExecuteExW",
    "cef_execute_process", "cef_initialize", "cef_run_message_loop",
}


def inspect(rea, binary, member):
    proc = subprocess.run([rea, "inspect-pe-imports", str(binary), "--json"],
                          check=True, capture_output=True, text=True)
    result = json.loads(proc.stdout)["normalized_result"]
    dependencies = {}
    observations = []
    for kind in ("imports", "delay_imports"):
        modules = result[kind]
        dependencies[kind] = [m["name"] for m in modules]
        for module in modules:
            names = {item.get("name") for item in module["functions"]}
            for api in sorted(PORTING_APIS.intersection(names)):
                observations.append({"kind": kind, "module": module["name"], "function": api})
    return {"member": member, "sha256": result["artifact"]["sha256"],
            "bytes": result["artifact"]["bytes"], "architecture": result["architecture"],
            "bits": result["bits"], **dependencies, "selected_imports": observations}


def audit(rea, cache):
    release = json.loads((HERE.parent / "runtime/steam/pins/valve-release.json").read_text())
    records = []
    with tempfile.TemporaryDirectory(prefix="steam-native-audit-") as tmp:
        work = Path(tmp)
        for spec in release["archives"]:
            archive = (cache or work) / spec["file"]
            if not cache:
                with urlopen(CDN + spec["file"], timeout=90) as src, archive.open("wb") as dest:
                    shutil.copyfileobj(src, dest)
            with archive.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if archive.stat().st_size != spec["size"] or digest != spec["sha256"]:
                raise ValueError(f"Archive does not match the pinned release: {archive.name}")
            with ZipFile(archive) as zipped:
                for member in spec["members"]:
                    binary = work / Path(member).name
                    with zipped.open(member) as src, binary.open("wb") as dest:
                        shutil.copyfileobj(src, dest)
                    records.append(inspect(rea, binary, member))
    return {"valve_manifest_version": release["version"], "components": records}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, help="read the three pinned ZIPs from this directory")
    parser.add_argument("--rea", default=os.environ.get("REA_BIN", "rea"), help="path to REA CLI")
    args = parser.parse_args()
    json.dump(audit(args.rea, args.cache), sys.stdout, indent=2)
    print()
