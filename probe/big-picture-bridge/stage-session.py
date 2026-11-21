"""Stage the native Steam session query from the pinned Windows SDK build."""
import hashlib
from pathlib import Path
import re
from xml.etree import ElementTree
from zipfile import ZipFile


APPX = "PackagedProcessProbe-x64.appx"
COMPONENT = ("ProbeNative.dll", "XboxProbe.Native.winmd")


def verify_registration(manifest):
    root = ElementTree.fromstring(manifest)
    ident = root.find("{*}Identity")
    if ident is None or (ident.get("Name"), ident.get("Publisher"), ident.get("ProcessorArchitecture")) != (
        "XboxPortabilityProbe.ChildProcess.SteamCore", "CN=Xbox Portability Probe", "x64"
    ):
        raise ValueError("Expected x64 Steam Core identity.")
    servers = root.findall("./{*}Extensions/{*}Extension[@Category='windows.activatableClass.inProcessServer']/{*}InProcessServer")
    for server in servers:
        if server.findtext("{*}Path") == COMPONENT[0] and server.find(
            "{*}ActivatableClass[@ActivatableClassId='XboxProbe.Native.ProbeRunner']"
        ) is not None:
            return
    raise ValueError("Missing the Steam Core native class registration.")


def stage_session_component(layout: Path, artifact: Path, checksum: str):
    sums = (artifact / "SHA256SUMS").read_bytes()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", checksum) or hashlib.sha256(sums).hexdigest() != checksum.lower():
        raise ValueError("Unexpected Steam Core native artifact checksums.")
    pins = [
        line.split("  ")[0] for line in sums.decode("ascii").splitlines()
        if re.fullmatch(r"[a-fA-F0-9]{64}  " + re.escape(APPX), line)
    ]
    if len(pins) != 1:
        raise ValueError("Expected one APPX checksum in the Steam Core artifact.")
    verify_registration((layout / "AppxManifest.xml").read_bytes())
    with (artifact / APPX).open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != pins[0].lower():
            raise ValueError("Unexpected Steam Core native APPX digest.")
        stream.seek(0)
        with ZipFile(stream) as package:
            for member in (*COMPONENT, "AppxManifest.xml", "AppxSignature.p7x"):
                if package.namelist().count(member) != 1:
                    raise ValueError(f"Missing or ambiguous native artifact member {member}.")
            verify_registration(package.read("AppxManifest.xml"))
            dll, metadata = (package.read(member) for member in COMPONENT)
    if not dll.startswith(b"MZ") or not metadata.startswith(b"MZ"):
        raise ValueError("Invalid Steam Core native component.")
    for name in ("SteamSessionSteamId", "RunSteamSignIn", "ResumeSteamSignIn", "LaunchSteamGame", "LaunchSteamSdlGame"):
        if name.encode("ascii") + b"\x00" not in metadata:
            raise ValueError(f"Missing required Steam Core member {name}.")
    for member in COMPONENT:
        if not (layout / member).is_file():
            raise ValueError(f"Missing original Steam Core component {member}.")
    for member, data in zip(COMPONENT, (dll, metadata)):
        (layout / member).write_bytes(data)
