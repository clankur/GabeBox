#!/usr/bin/env python3
"""Check an owned Steam installation against a Win32 launch source profile."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat


RESERVED_WIN32_NAMES = {"AUX", "CON", "CONIN$", "CONOUT$", "NUL", "PRN"} | {
    prefix + number for prefix in ("COM", "LPT") for number in "123456789¹²³"
}
INVALID_WIN32_CHARACTERS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def valid_win32_component(name):
    """Validate one Windows path component consistently on every build host."""
    stem = name.partition(".")[0].rstrip(" ").upper()
    return (bool(name) and not name.endswith((".", " ")) and stem not in RESERVED_WIN32_NAMES
            and INVALID_WIN32_CHARACTERS.search(name) is None)


def read_profile(profile):
    with profile.open("rb") as source:
        data = source.read(65537)
    if not data or len(data) > 65536:
        raise ValueError("Win32 source profiles must be between 1 and 65536 bytes.")
    builds = json.loads(data)
    if not isinstance(builds, dict) or not builds:
        raise ValueError("A Win32 source profile must contain at least one Steam build.")
    for build, entries in builds.items():
        if not re.fullmatch(r"[1-9][0-9]{0,19}", build) or not isinstance(entries, list) or not entries:
            raise ValueError("Expected a Steam build ID with a nonempty list of source files.")
        names = set()
        for row in entries:
            if not isinstance(row, dict) or set(row) != {"path", "size", "sha256"}:
                raise ValueError(f"Invalid file record for Steam build {build}.")
            name, size, digest = row["path"], row["size"], row["sha256"]
            if (not isinstance(name, str) or len(name) > 260
                    or any(not re.fullmatch(r"[A-Za-z0-9_. -]{1,255}", part)
                           or not valid_win32_component(part)
                           for part in name.split("/"))
                    or name.lower() in names or type(size) is not int or not 0 < size < 2**53
                    or not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest)):
                raise ValueError(f"Invalid source fingerprint for Steam build {build}: {name!r}.")
            names.add(name.lower())
    return builds


def verify_install(root, entries):
    for row in entries:
        name = row["path"]
        path = root
        parts = name.split("/")
        for index, part in enumerate(parts):
            path /= part
            info = path.lstat()
            expected_type = stat.S_ISREG if index == len(parts) - 1 else stat.S_ISDIR
            if (not expected_type(info.st_mode)
                    or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
                raise ValueError(f"Unexpected file type: {name}.")
        if info.st_size != row["size"]:
            raise ValueError(f"Size mismatch: {name}.")
        with path.open("rb") as source:
            digest = hashlib.sha256()
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != row["sha256"]:
            raise ValueError(f"SHA-256 mismatch: {name}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("installation", type=Path)
    parser.add_argument("--build", required=True, help="Steam build ID from the installation manifest")
    args = parser.parse_args()
    try:
        entries = read_profile(args.profile).get(args.build)
        if entries is None:
            raise ValueError(f"Unsupported Steam build {args.build}.")
        verify_install(args.installation, entries)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Steam build {args.build} matched {len(entries)} source fingerprints.")


if __name__ == "__main__":
    main()
