"""Stage the native Steam session query from the pinned Windows SDK build."""
from contextlib import contextmanager
import hashlib
import re
from xml.etree import ElementTree
from zipfile import ZipFile

from tools.artifacts import pe_summary


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


def read_package_member(package, name):
    matches = [info for info in package.infolist() if info.filename.casefold() == name.casefold()]
    if len(matches) != 1 or matches[0].filename != name or not matches[0].file_size:
        raise ValueError(f"Missing, empty or ambiguous Steam Core artifact member {name}.")
    return package.read(matches[0])


@contextmanager
def open_session_artifact(artifact, checksum):
    sums = (artifact / "SHA256SUMS").read_bytes()
    if not re.fullmatch(r"[0-9a-fA-F]{64}", checksum) or hashlib.sha256(sums).hexdigest() != checksum.lower():
        raise ValueError("Unexpected Steam Core native artifact checksums.")
    pins = [
        line.split("  ")[0] for line in sums.decode("ascii").splitlines()
        if re.fullmatch(r"[a-fA-F0-9]{64}  " + re.escape(APPX), line)
    ]
    if len(pins) != 1:
        raise ValueError("Expected one APPX checksum in the Steam Core artifact.")
    with (artifact / APPX).open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != pins[0].lower():
            raise ValueError("Unexpected Steam Core native APPX digest.")
        stream.seek(0)
        with ZipFile(stream) as package:
            verify_registration(read_package_member(package, "AppxManifest.xml"))
            read_package_member(package, "AppxSignature.p7x")
            yield package


def stage_session_component(layout, package):
    verify_registration((layout / "AppxManifest.xml").read_bytes())
    dll, metadata = (read_package_member(package, member) for member in COMPONENT)
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


def stage_stardew_startup_hook(layout, package):
    if not (layout / "steam-sdl-host-413150.txt").is_file():
        return
    name = "StardewStartupDiagnostic.dll"
    # Existing pinned Steam Core artifacts leave the verified recovery hook in place.
    if not any(member.casefold() == name.casefold() for member in package.namelist()):
        return
    hook = read_package_member(package, name)
    if pe_summary(hook).get("clr_requires_x86") != "no":
        raise ValueError("Expected a managed Stardew startup hook that supports the x64 host.")
    (layout / name).write_bytes(hook)
