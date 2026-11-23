#!/usr/bin/env python3
"""Audit Steam Windows builds and measure native Xbox coverage."""

import argparse
from collections import defaultdict, deque
import hashlib
import json
from pathlib import Path, PureWindowsPath
import re
import runpy
import struct
import subprocess
import sys


PRIORITY_BINARIES = {
    1145360: ("x64/Hades.exe", "x64/EngineWin64s.dll"),
    976730: ("mcclauncher.exe", "MCC/Binaries/Win64/MCC-Win64-Shipping.exe"),
    2868840: ("SlayTheSpire2.exe",),
}
UNITY_COMPANIONS = (
    "UnityPlayer.dll",
    "GameAssembly.dll",
    "MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll",
    "MonoBleedingEdge/EmbedRuntime/mono-2.0-sgen.dll",
    "Mono/EmbedRuntime/mono.dll",
)
XINPUT_MODULES = {f"xinput{version}.dll" for version in ("1_1", "1_2", "1_3", "1_4", "9_1_0", "uap")}
CAPABILITIES = {
    "unity-player": {"unityplayer.dll"},
    "mono-runtime": {"mono-2.0-bdwgc.dll", "mono-2.0-sgen.dll", "mono.dll"},
    "il2cpp-runtime": {"gameassembly.dll"},
    "desktop-opengl": {"opengl32.dll", "glu32.dll"},
    "direct3d-9": {"d3d9.dll"},
    "direct3d-11": {"d3d11.dll"},
    "direct3d-12": {"d3d12.dll", "d3d12core.dll"},
    "dxgi": {"dxgi.dll"},
    "vulkan": {"vulkan-1.dll"},
    "desktop-windowing": {"user32.dll", "gdi32.dll", "imm32.dll"},
    "desktop-window-registration": {f"user32.dll!RegisterClass{suffix}" for suffix in ("A", "W", "ExA", "ExW")},
    "desktop-window-creation": {"user32.dll!CreateWindowExA", "user32.dll!CreateWindowExW"},
    "legacy-clr-loader": {"mscoree.dll!_CorDllMain", "mscoree.dll!_CorExeMain"},
    "kernel-transactions": {"ktmw32.dll"},
    "steamworks": {"steam_api.dll", "steam_api64.dll"},
    "media-foundation": {"mf.dll", "mfplat.dll", "mfreadwrite.dll"},
    "sdl": {"sdl2.dll", "sdl3.dll"},
    "xinput": XINPUT_MODULES,
    "directinput": {"dinput.dll", "dinput8.dll"},
    "raw-input": {"user32.dll!GetRawInputDeviceList", "user32.dll!RegisterRawInputDevices"},
}
CHECKS = ("gameplay", "controller", "audio", "save-reload", "return-to-library")
ACF_TOKEN = re.compile(r'\s+|//[^\r\n]*|"(?:[^"\\]|\\.)*"|[{}]')


def app_id(value):
    if isinstance(value, str) and re.fullmatch(r"[0-9]{1,10}", value):
        value = int(value)
    if type(value) is not int or not 1 <= value <= 0xFFFFFFFF:
        raise ValueError("Expected a positive Steam AppID.")
    return value


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def library(path):
    rows = read_json(path)
    if not isinstance(rows, list) or not rows:
        raise ValueError("Use the app's nonempty game-only Steam inventory.")
    games = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str) or not row["name"].strip():
            raise ValueError("Each inventory entry needs a Steam AppID and game name.")
        identifier = app_id(row.get("id"))
        if identifier in games:
            raise ValueError(f"Duplicate Steam AppID {identifier}.")
        games[identifier] = row["name"]
    return dict(sorted(games.items(), key=lambda pair: (pair[0] not in PRIORITY_BINARIES, pair[1].casefold(), pair[0])))


def installed_manifest(path, identifier):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular Steam manifest in steamapps.")
    with path.open("rb") as source:
        raw = source.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise ValueError("Steam manifest exceeds 1 MiB.")
    text = raw.decode("utf-8-sig")
    tokens = []
    offset = 0
    while offset < len(text):
        match = ACF_TOKEN.match(text, offset)
        if not match:
            raise ValueError("Unsupported Steam manifest syntax.")
        token = match[0]
        if token[0] in '"{}':
            tokens.append(token)
        offset = match.end()
    if len(tokens) < 3 or tokens[:2] != ['"AppState"', '{']:
        raise ValueError("Expected a Steam AppState manifest.")

    fields = {}
    keys = {"appid", "buildid", "installdir", "stateflags"}
    index = 2
    while index < len(tokens) and tokens[index][0] == '"':
        if index + 1 == len(tokens):
            raise ValueError("Missing Steam manifest field value.")
        key, value = tokens[index][1:-1].casefold(), tokens[index + 1]
        index += 2
        if value == '{':
            if key in keys:
                raise ValueError("Expected a scalar Steam manifest field.")
            depth = 1
            while index < len(tokens) and depth:
                depth += (tokens[index] == '{') - (tokens[index] == '}')
                index += 1
        elif value[0] != '"':
            raise ValueError("Missing Steam manifest field value.")
        elif key in keys:
            if key in fields:
                raise ValueError("Duplicate Steam manifest field.")
            fields[key] = value[1:-1]

    if (tokens[index:] != ['}'] or fields.keys() != keys or app_id(fields["appid"]) != identifier
            or not re.fullmatch(r"(?:0|[1-9][0-9]*)", fields["buildid"])
            or (fields["stateflags"] == "4" and fields["buildid"] == "0")):
        raise ValueError("Incomplete or mismatched Steam manifest.")
    directory = re.sub(r'\\(["\\])', r'\1', fields["installdir"])
    if not directory or directory in {".", ".."} or any(c in directory for c in '/\\:'):
        raise ValueError("Expected an install directory name inside steamapps/common.")
    return fields["buildid"], directory, fields["stateflags"]


def binary_paths(values):
    if (not isinstance(values, (list, tuple)) or not values
            or any(not isinstance(value, str) or not value.strip() for value in values)):
        raise ValueError("Supply a nonempty list of relative binary paths.")
    paths = []
    for value in values:
        path = PureWindowsPath(value)
        if path.anchor or ".." in path.parts or any(":" in part for part in path.parts) or not path.name:
            raise ValueError("Use paths relative to the installed game directory.")
        paths.append(path.as_posix())
    return list(dict.fromkeys(paths))


def unity_binaries(root, candidates):
    launchers = []
    for candidate in candidates:
        launcher = root / candidate
        player = launcher.with_name("UnityPlayer.dll")
        data = launcher.with_name(launcher.stem + "_Data")
        if (player.is_file() and data.is_dir() and root in player.resolve().parents
                and root in data.resolve().parents):
            launchers.append(launcher)
    if len(launchers) != 1:
        return []
    launcher = launchers[0]
    companions = [launcher.parent / name for name in UNITY_COMPANIONS]
    return [launcher.relative_to(root).as_posix()] + [
        path.relative_to(root).as_posix()
        for path in companions if path.is_file() and root in path.resolve().parents
    ]


def scan_library(games, libraries, entries, inspect, import_results=None):
    locations = defaultdict(list)
    for steamapps in dict.fromkeys(path.resolve(strict=True) for path in libraries):
        if not steamapps.is_dir():
            raise ValueError("Supply a Steam library's steamapps directory.")
        for manifest in steamapps.glob("appmanifest_*.acf"):
            match = re.fullmatch(r"appmanifest_([1-9][0-9]{0,9})\.acf", manifest.name)
            if match and int(match[1]) in games:
                locations[int(match[1])].append(manifest)

    rows, audits = [], []
    for identifier, name in games.items():
        row = {"id": identifier, "name": name}
        rows.append(row)
        matches = locations[identifier]
        if len(matches) != 1:
            row["status"] = "multiple-installs" if matches else "no-manifest"
            continue
        manifest = matches[0]
        try:
            build, directory, state = installed_manifest(manifest, identifier)
            if build != "0":
                row["build_id"] = build
            if state != "4":
                row["status"] = "incomplete-install"
                continue
            common = (manifest.parent / "common").resolve(strict=True)
            root = (common / directory).resolve(strict=True)
            if common.parent != manifest.parent or root.parent != common or not root.is_dir():
                raise ValueError("The game directory must be inside steamapps/common.")
            binaries = entries.get(identifier) or PRIORITY_BINARIES.get(identifier)
            if not binaries:
                candidates = sorted(path.relative_to(root).as_posix() for path in root.rglob("*")
                                    if path.suffix.casefold() == ".exe" and path.is_file()
                                    and root in path.resolve().parents)
                binaries = unity_binaries(root, candidates) or (candidates if len(candidates) == 1 else [])
                if not binaries:
                    row.update(status="entrypoints-needed", candidates=candidates)
                    continue
            audit = audit_install(identifier, build, root, binaries, inspect, import_results)
            current = installed_manifest(manifest, identifier)
            if current != (build, directory, state):
                if current[0] == "0":
                    row.pop("build_id", None)
                else:
                    row["build_id"] = current[0]
                raise ValueError("Steam changed the installed build during the scan.")
            audits.append(audit)
            row["status"] = "audited"
        except (ValueError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            row.update(status="scan-error", error=error.__class__.__name__)
            if isinstance(error, ValueError):
                row["detail"] = str(error)
            elif isinstance(error, FileNotFoundError) and error.filename:
                row["detail"] = f"File not found: {Path(error.filename).name}."
    result = {"games": rows, "audits": audits}
    if import_results is not None:
        result["observed_blocker_groups"] = observed_blocker_groups(audits)
    return result


def xbox_import_results(paths):
    missing_modules, missing_symbols = set(), set()
    for path in paths:
        report = path.read_text(encoding="utf-8-sig")
        missing_modules.update(module.lower() for module in re.findall(
            r"^([\w.-]+\.dll)_system_load_error=0x[0-9a-f]+$", report, re.M | re.I))
        missing_symbols.update(f"{module.lower()}!{symbol}" for module, symbol in re.findall(
            r"^missing_import=([\w.-]+\.dll)!(\S+)$", report, re.M | re.I))
    return missing_modules, missing_symbols


def observed_blocker_groups(audits):
    groups = defaultdict(lambda: {"load_time": [], "delay_load": []})
    for audit in audits:
        title_build = {"id": audit["id"], "build_id": audit["build_id"]}
        for key in audit["observed_missing_system_imports"]:
            groups[key]["load_time"].append(title_build)
        for key in audit["observed_missing_delay_imports"]:
            groups[key]["delay_load"].append(title_build)
    return [{"import": key, **group} for key, group in sorted(
        groups.items(), key=lambda item: (-len(item[1]["load_time"]), -len(item[1]["delay_load"]), item[0]))]


def under_root(root, path):
    candidate = path.resolve(strict=True)
    if root not in candidate.parents or not candidate.is_file():
        raise ValueError(f"Candidate must be a file in the supplied game directory: {path.name}.")
    return candidate


def rea_imports(rea, binary, fallback):
    try:
        raw = subprocess.run([rea, "inspect-pe-imports", str(binary), "--json"], check=True,
                             timeout=45, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        try:
            failure = json.loads(error.stdout)
        except (TypeError, ValueError):
            raise error from None
        if (isinstance(failure, dict) and failure.get("code") == "resource_constraint"
                and (failure.get("details") or {}).get("resource") == "file-size"):
            return fallback(binary)
        raise
    result = json.loads(raw)["normalized_result"]
    sources = []
    for table in ("imports", "delay_imports"):
        modules = defaultdict(set)
        for module in result[table]:
            modules[module["name"].lower()].update(
                symbol["name"] if symbol["name"] is not None else f'#{symbol["ordinal"]}'
                for symbol in module["functions"])
        sources.append(dict(modules))
    return result["architecture"], result["artifact"]["sha256"], *sources


def pe_headers(objdump, binary):
    output = subprocess.run([objdump, "--private-headers", str(binary)], check=True, timeout=30,
                            capture_output=True, text=True).stdout
    machine = re.search(r"^.*file format (coff-[\w-]+)$", output, re.M)
    if not machine or not re.search(r"^Magic\s+(?:010b|020b)\b", output, re.M | re.I):
        raise ValueError(f"Could not inspect the Windows PE image {binary.name}.")
    architecture = {"coff-x86-64": "amd64", "coff-i386": "i386", "coff-arm64": "arm64"}
    return architecture.get(machine[1], machine[1]), output


def objdump_inspector(objdump):
    parse_import_table = runpy.run_path(str(Path(__file__).with_name("prepare-steam-core.py")))["parse_import_table"]

    def inspect(binary):
        architecture, output = pe_headers(objdump, binary)
        directory = re.search(r"^Entry 1\s+([0-9a-f]+)\s+[0-9a-f]+\s+Import Directory", output, re.M | re.I)
        imports = parse_import_table(output)
        if not imports and (not directory or int(directory[1], 16)):
            raise ValueError(f"Could not inspect the PE import table of {binary.name}.")
        delay = re.search(r"^Entry d\s+([0-9a-f]+)\s+[0-9a-f]+\s+Delay Import Directory", output, re.M | re.I)
        with binary.open("rb") as source:
            digest = hashlib.sha256()
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        # llvm-objdump identifies a delay-import directory. REA reads its edges.
        delay_imports = {} if delay and not int(delay[1], 16) else None
        return architecture, digest.hexdigest(), imports or {}, delay_imports

    return inspect


def pe_inspector(objdump, rea):
    llvm = objdump_inspector(objdump)
    if rea:
        return lambda binary: rea_imports(rea, binary, llvm)
    return llvm


def report_imports(imports):
    return {module: sorted(symbols) for module, symbols in sorted(imports.items())}


def pe_certificate_table(binary):
    """Check whether the PE declares a certificate table, without verifying its signature."""
    with binary.open("rb") as source:
        dos = source.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            return None
        source.seek(struct.unpack_from("<I", dos, 60)[0])
        header = source.read(24)
        if len(header) != 24 or header[:4] != b"PE\0\0":
            return None
        optional = source.read(struct.unpack_from("<H", header, 20)[0])
    if len(optional) < 2:
        return None
    directories = {0x10B: 96, 0x20B: 112}.get(struct.unpack_from("<H", optional)[0])
    if directories is None or len(optional) < directories:
        return None
    if struct.unpack_from("<I", optional, directories - 4)[0] <= 4:
        return False
    offset = directories + 4 * 8
    return any(struct.unpack_from("<II", optional, offset)) if len(optional) >= offset + 8 else None


def audit_install(identifier, build, root, binaries, inspect, import_results):
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Supply the game install directory.")
    queue = deque(under_root(root, root / binary) for binary in binaries)
    modules, seen, directories = {}, set(), {}
    blockers, delay_blockers, external = set(), set(), set()
    while queue:
        binary = queue.popleft()
        if binary in seen:
            continue
        seen.add(binary)
        machine, digest, imports, delay_imports = inspect(binary)
        # The packaged Xbox import control runs as an AMD64 process.
        missing_modules, missing_symbols = import_results if import_results and machine == "amd64" else ((), ())
        relative = binary.relative_to(root).as_posix()
        bundled = {}
        if binary.parent not in directories:
            index = defaultdict(list)
            for path in binary.parent.iterdir():
                if path.suffix.casefold() == ".dll" and path.is_file():
                    index[path.name.casefold()].append(path)
            directories[binary.parent] = index
        for table, unresolved in ((imports, blockers), (delay_imports or {}, delay_blockers)):
            for module, symbols in table.items():
                candidates = directories[binary.parent].get(module, ())
                if len(candidates) > 1:
                    raise ValueError(f"Ambiguous bundled DLL name {module} alongside {relative}.")
                if candidates:
                    dependency = under_root(root, candidates[0])
                    bundled[module] = dependency.relative_to(root).as_posix()
                    queue.append(dependency)
                    continue
                external.add(module)
                if module in missing_modules:
                    unresolved.add(module)
                unresolved.update(f"{module}!{symbol}" for symbol in symbols
                                  if f"{module}!{symbol}" in missing_symbols)
        modules[relative] = {
            "architecture": machine, "sha256": digest, "bundled": bundled,
            "authenticode_table": pe_certificate_table(binary),
            "imports": report_imports(imports),
            "delay_imports": None if delay_imports is None else report_imports(delay_imports),
            "uses_dynamic_loader": any({"LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW",
                                        "GetProcAddress", "LdrLoadDll"} & symbols for symbols in imports.values()),
        }
    imported = set().union(*(details["imports"].keys() | (details["delay_imports"] or {}).keys()
                             for details in modules.values()))
    imported.update(Path(relative).name.casefold() for relative in modules)
    imported.update(f"{module}!{symbol}"
                    for details in modules.values() for table in (details["imports"], details["delay_imports"] or {})
                    for module, symbols in table.items() for symbol in symbols)
    return {
        "id": identifier, "build_id": build, "modules": modules, "external_imports": sorted(external),
        "capabilities": [name for name, imports in CAPABILITIES.items() if imports & imported],
        "observed_missing_system_imports": sorted(blockers),
        "observed_missing_delay_imports": sorted(delay_blockers),
    }


def runtime_exports(output):
    table = output.partition("Export Table:")[2]
    names = set()
    for ordinal, address, name in re.findall(r"^[ \t]*(\d+)[ \t]+(0x[0-9a-f]+)(?:[ \t]+(\S+))?",
                                             table, re.M | re.I):
        if int(address, 16):
            names.add(f"#{ordinal}")
            if name:
                names.add(name)
    return names


def compare_runtime(modules, paths, objdump):
    candidates = {}
    for path in paths:
        module = path.name.lower()
        if path.suffix.lower() != ".dll" or module in candidates:
            raise ValueError(f"Supply one runtime DLL per module name: {path.name}.")
        architecture, headers = pe_headers(objdump, path)
        exports = runtime_exports(headers)
        required, architectures = set(), set()
        for details in modules.values():
            imports = set(details["imports"].get(module, ()))
            imports.update((details["delay_imports"] or {}).get(module, ()))
            if imports:
                required.update(imports)
                architectures.add(details["architecture"])
        candidates[module] = {
            "architecture": architecture,
            "importer_architectures": sorted(architectures),
            "required_export_count": len(required),
            "missing_exports": sorted(required - exports),
        }
    return candidates


def plan_bindings(audit, binary, registry, runtime_aliases=None, host_xinput=False, signed_source_sha256=None):
    image = audit["modules"][binary]
    if image["architecture"] != "amd64":
        raise ValueError("Win32 import bindings currently support AMD64 images.")
    if image.get("authenticode_table") is True:
        if signed_source_sha256 is None:
            raise ValueError(f"{binary} declares an Authenticode table. Pin the original SHA-256 for a local recipe.")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", signed_source_sha256) or signed_source_sha256.lower() != image["sha256"]:
            raise ValueError(f"{binary} does not match the audited signed-source SHA-256.")
    elif signed_source_sha256 is not None:
        raise ValueError(f"{binary} has no audited Authenticode table to pin.")
    missing = set(audit["observed_missing_system_imports"])
    selected = {}
    for library, symbols in image["imports"].items():
        if library in image.get("bundled", {}):
            continue
        if library in missing and runtime_aliases and library in runtime_aliases:
            target, architecture, exports = runtime_aliases[library]
            absent = set(symbols) - exports
            if architecture != image["architecture"] or absent:
                detail = ", ".join(sorted(absent)) if absent else architecture
                raise ValueError(f"{target} cannot replace {library} for {binary}: {detail}.")
            bindings = dict.fromkeys(symbols, target)
        else:
            available = registry.get(library, {})
            bindings = {symbol: available[symbol] for symbol in symbols
                        if symbol in available and f"{library}!{symbol}" in missing}
        if host_xinput and library in XINPUT_MODULES:
            for symbol in ("XInputGetState", "XInputGetCapabilities"):
                if symbol in symbols:
                    bindings[symbol] = "XboxWin32Input.dll"
        if bindings:
            selected[library] = bindings
    return selected


def inspect_runtime_aliases(aliases, objdump):
    candidates = {}
    for alias in aliases:
        source, separator, filename = alias.partition("=")
        source = source.lower()
        path = Path(filename)
        if (not separator or not re.fullmatch(r"[\w.-]+\.dll", source, re.ASCII)
                or not re.fullmatch(r"[\w.-]+\.dll", path.name, re.I | re.ASCII)
                or source == path.name.lower() or source in candidates):
            raise ValueError("Use one SOURCE.dll=PATH_TO_CANDIDATE.dll per missing DLL.")
        architecture, headers = pe_headers(objdump, path)
        candidates[source] = (path.name, architecture, runtime_exports(headers))
    return candidates


def coverage(games, target, audits, evidence, require_gpu_60=False, installed_builds=None):
    gpu_gate = (runpy.run_path(str(Path(__file__).with_name("fhd") / "measure.py"))["reaches_gpu_60"]
                if require_gpu_60 else None)
    by_game = defaultdict(list)
    by_capability = defaultdict(set)
    verified = set()
    for audit in audits:
        identifier = app_id(audit["id"])
        if identifier in games:
            by_game[identifier].append(audit)
            for capability in audit["capabilities"]:
                by_capability[capability].add(identifier)
    for trial in evidence:
        identifier = app_id(trial["id"])
        if identifier not in games or trial["target"] != target:
            continue
        if (not trial.get("build_id") or not trial.get("evidence")
                or any(trial["checks"].get(check) is not True for check in CHECKS)):
            continue
        if gpu_gate and (trial.get("performance_scene") != "gameplay" or not gpu_gate(trial.get("performance"))):
            continue
        known_builds = (installed_builds or {}).get(identifier) or {audit["build_id"] for audit in by_game[identifier]}
        if not known_builds or known_builds == {trial["build_id"]}:
            verified.add(identifier)
    return {
        "target": target,
        "gpu_60_gameplay_required": require_gpu_60,
        "library_count": len(games),
        "goal_count": (4 * len(games) + 4) // 5,
        "audited_count": len(by_game),
        "local_verified_count": len(verified),
        "capability_groups": {name: sorted(ids) for name, ids in sorted(by_capability.items())},
        "games": [{"id": identifier, "name": name, "audited": identifier in by_game,
                   "local_verified": identifier in verified}
                  for identifier, name in games.items()],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    scan = commands.add_parser("scan", help="read a PE executable and its app-local import graph")
    scan.add_argument("--app-id", type=app_id, required=True)
    scan.add_argument("--build-id", required=True, help="buildid from the Steam manifest")
    scan.add_argument("--install-dir", type=Path, required=True)
    scan.add_argument("--binary", action="append", metavar="RELATIVE_PATH",
                      help="executable or dynamically loaded DLL, repeat as needed")
    scan.add_argument("--objdump", default="llvm-objdump")
    scan.add_argument("--rea", metavar="PATH", help="use REA for delay imports, with LLVM fallback for oversized files")
    scan.add_argument("--xbox-import-report", type=Path, action="append", default=[],
                      help="AMD64 Xbox import control report, repeat for the same runtime")
    scan.add_argument("--runtime-dll", action="append", type=Path, default=[],
                      help="compare required exports with a candidate DLL via llvm-objdump, repeat as needed")
    batch = commands.add_parser("scan-library", help="audit completed installs from a Steam game inventory")
    batch.add_argument("inventory", type=Path)
    batch.add_argument("--steamapps", type=Path, action="append", required=True,
                       help="Steam library's steamapps directory, repeat for multiple libraries")
    batch.add_argument("--entrypoints", type=Path,
                       help="JSON object mapping Steam AppIDs to relative executable or engine paths")
    batch.add_argument("--objdump", default="llvm-objdump")
    batch.add_argument("--rea", metavar="PATH", help="use REA for delay imports, with LLVM fallback for oversized files")
    batch.add_argument("--xbox-import-report", type=Path, action="append", default=[],
                       help="match AMD64 imports against console observations and rank shared blockers")
    bindings = commands.add_parser("bindings", help="select known bindings for a binary's observed missing imports")
    bindings.add_argument("audit", type=Path)
    bindings.add_argument("binary", metavar="RELATIVE_PATH")
    bindings.add_argument("--registry", type=Path, default=Path(__file__).with_name("win32-runtime") / "bindings.json")
    bindings.add_argument("--runtime-alias", action="append", default=[], metavar="SOURCE.dll=PATH",
                          help="trial candidate for an observed missing DLL, subject to a complete import match")
    bindings.add_argument("--host-xinput", action="store_true",
                          help="use the Xbox host for XInput controller state and capabilities in a hosted game")
    bindings.add_argument("--signed-source-sha256", metavar="SHA256",
                          help="allow bindings for a hash-pinned signed source in a local staging recipe")
    bindings.add_argument("--objdump", default="llvm-objdump")
    report = commands.add_parser("coverage", help="count trials against an exported Steam inventory")
    report.add_argument("inventory", type=Path)
    report.add_argument("--target", required=True, help="identifier for the exact console and runtime configuration")
    report.add_argument("--audit", type=Path, action="append", default=[])
    report.add_argument("--library-audit", type=Path, action="append", default=[],
                        help="include results from scan-library, repeat as needed")
    report.add_argument("--evidence", type=Path, help="JSON array of observed game trials")
    report.add_argument("--require-gpu-60", action="store_true", help="require a hardware 60 fps scene measurement for each counted title")
    args = parser.parse_args()
    try:
        if args.command == "scan":
            defaults = PRIORITY_BINARIES.get(args.app_id, ())
            binaries = args.binary or defaults
            if not binaries or not re.fullmatch(r"[0-9]+", args.build_id):
                raise ValueError("Supply the numeric Steam build ID and the relative game binary path(s).")
            result = audit_install(args.app_id, args.build_id, args.install_dir, binary_paths(binaries),
                                   pe_inspector(args.objdump, args.rea), xbox_import_results(args.xbox_import_report))
            if args.runtime_dll:
                result["runtime_candidates"] = compare_runtime(result["modules"], args.runtime_dll, args.objdump)
        elif args.command == "scan-library":
            inputs = read_json(args.entrypoints) if args.entrypoints else {}
            if not isinstance(inputs, dict):
                raise ValueError("Supply a JSON object mapping Steam AppIDs to lists of binary paths.")
            entries = {}
            for identifier, values in inputs.items():
                identifier = app_id(identifier)
                if identifier in entries:
                    raise ValueError(f"Duplicate entrypoints for Steam AppID {identifier}.")
                entries[identifier] = binary_paths(values)
            observations = xbox_import_results(args.xbox_import_report) if args.xbox_import_report else None
            result = scan_library(library(args.inventory), args.steamapps, entries,
                                  pe_inspector(args.objdump, args.rea), observations)
        elif args.command == "bindings":
            aliases = inspect_runtime_aliases(args.runtime_alias, args.objdump)
            result = plan_bindings(read_json(args.audit), args.binary, read_json(args.registry), aliases,
                                   args.host_xinput, args.signed_source_sha256)
        else:
            audits = [read_json(path) for path in args.audit]
            installed_builds = defaultdict(set)
            for path in args.library_audit:
                batch_result = read_json(path)
                audits.extend(batch_result["audits"])
                for game in batch_result["games"]:
                    if "build_id" in game:
                        installed_builds[app_id(game["id"])].add(game["build_id"])
            result = coverage(library(args.inventory), args.target, audits,
                              read_json(args.evidence) if args.evidence else [], args.require_gpu_60, installed_builds)
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip()
        if not detail:
            try:
                detail = json.loads(error.stdout).get("message", "")
            except (ValueError, AttributeError):
                pass
        parser.exit(1, f"Error: PE inspection failed ({detail or f'exit {error.returncode}'}).\n")
    except (ValueError, OSError, KeyError, TypeError, AttributeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Error: {error}\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
