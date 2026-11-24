"""Validate retained Xbox release, image and collector receipts."""

import hashlib
import json
import struct
import xml.etree.ElementTree as ET
import zipfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(path):
    return json.loads(path.read_bytes())


def capture_time(path):
    return int(path.stem.rsplit("-", 1)[1]) / 1_000_000_000


def check_png(receipt, digest):
    data = receipt.with_suffix(".png").read_bytes()
    require(data[:16] == b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" and
            struct.unpack(">II", data[16:24]) == (1920, 1080), f"Expected full-HD PNG paired with {receipt.name}")
    require(hashlib.sha256(data).hexdigest() == digest, f"PNG digest mismatch for {receipt.name}")


def check_release(appx, digest, launch):
    with appx.open("rb") as source:
        computed = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            computed.update(chunk)
    require(computed.hexdigest() == digest.lower(), "APPX differs from the supplied release pin")
    with zipfile.ZipFile(appx) as files:
        identity = ET.fromstring(files.read("AppxManifest.xml")).find("{*}Identity")
        require(files.read("AppxSignature.p7x").startswith(b"PKCX"), "Missing APPX signature record")
    require(identity is not None and identity.get("Name") == "XboxPortabilityProbe.ChildProcess.SteamCore",
            "Expected the production Steam Core identity")
    require(launch["version"] == identity.get("Version") and
            launch["package"] == f'{identity.get("Name")}_{identity.get("Version")}_{identity.get("ProcessorArchitecture")}__47az75d01t3xc',
            "Installed package identity/version differs from pinned APPX")
