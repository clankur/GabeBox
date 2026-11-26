"""Inspect PE metadata and verify workflow artifacts for staging and deployment.

Verify a downloaded workflow artifact against its SHA256SUMS file:

    python3 -m tools.artifacts verify ARTIFACT_DIR [--pin SHA256_OF_SHA256SUMS]

Unlike `shasum -c`, this accepts the CRLF line endings that older Windows workflow runs wrote.
"""

import argparse
import hashlib
from pathlib import Path
import re
import sys
from struct import unpack_from


SDL_TRIALS = {
    975370: ('Dwarf Fortress', '2.26.2', '9603fdfd2b4bd75b3574bb993e25332bae820a66f18e7ffb6f8c82cb4f6c004a'),
    413150: ('Stardew Valley', '2.30.9', '58f0a2af6653445b48e58ace6890fbe237bf76452c9743e800358d6b935ded64'),
}
SDL_LIMIT = 8 * 1024 * 1024


def pe_summary(header):
    if len(header) < 64 or header[:2] != b'MZ':
        return {'pe_header': 'unavailable'}
    pe = unpack_from('<I', header, 0x3c)[0]
    if pe < 64 or pe + 26 > len(header) or header[pe:pe + 4] != b'PE\0\0':
        return {'pe_header': 'unavailable'}

    machine, sections = unpack_from('<HH', header, pe + 4)
    optional_size = unpack_from('<H', header, pe + 20)[0]
    optional = pe + 24
    magic = unpack_from('<H', header, optional)[0]
    directory = {0x10b: 96, 0x20b: 112}.get(magic)
    section_table = optional + optional_size
    if directory is None or optional_size < directory or section_table > len(header):
        return {'pe_header': 'unavailable'}

    summary = {'pe_machine': {0x14c: 'x86', 0x8664: 'x64', 0xaa64: 'arm64'}.get(machine, hex(machine)),
               'pe_format': 'PE32' if magic == 0x10b else 'PE32+'}
    clr_entry = directory + 14 * 8
    count = unpack_from('<I', header, optional + directory - 4)[0]
    if count < 15:
        return summary
    if optional_size < clr_entry + 8:
        return {**summary, 'clr_flags': 'unavailable'}
    clr_rva, clr_size = unpack_from('<II', header, optional + clr_entry)
    if not clr_rva and not clr_size:
        return summary

    summary['clr_flags'] = 'unavailable'
    clr_header_size = 72
    if not clr_rva or clr_size < clr_header_size:
        return summary
    for section in range(min(sections, (len(header) - section_table) // 40)):
        _, address, raw_size, offset = unpack_from('<IIII', header, section_table + 40 * section + 8)
        relative = clr_rva - address
        start = offset + relative
        if 0 <= relative <= raw_size - clr_header_size and start + clr_header_size <= len(header):
            declared_size = unpack_from('<I', header, start)[0]
            if not clr_header_size <= declared_size <= min(clr_size, raw_size - relative, len(header) - start):
                break
            flags = unpack_from('<I', header, start + 16)[0]
            summary['clr_flags'] = f'0x{flags:08x}'
            summary['clr_requires_x86'] = 'yes' if flags & 2 and not flags & 0x20000 else 'no'
            break
    return summary


def load_sdl_artifact(directory, app_id):
    name, upstream, _ = SDL_TRIALS[app_id]
    artifact = directory / 'SDL2.dll'
    checksum = (directory / 'SHA256SUMS').read_text(encoding='ascii').strip()
    expected = re.fullmatch(r'([0-9a-f]{64})  SDL2\.dll', checksum)
    if artifact.stat().st_size > SDL_LIMIT or not expected:
        raise ValueError('Use a checked SDL2 workflow artifact containing SHA256SUMS.')
    candidate = artifact.read_bytes()
    if hashlib.sha256(candidate).hexdigest() != expected[1] or pe_summary(candidate).get('pe_machine') != 'x64':
        raise ValueError('The SDL2 workflow artifact has an unexpected checksum or architecture.')
    version_marker = b'\xbd\x04\xef\xfe\x00\x00\x01\x00'
    version_offset = candidate.find(version_marker)
    if version_offset < 0 or version_offset + 16 > len(candidate):
        raise ValueError('The SDL2 workflow artifact has no Windows version resource.')
    major_minor, patch_build = unpack_from('<II', candidate, version_offset + 8)
    version = f'{major_minor >> 16}.{major_minor & 0xffff}.{patch_build >> 16}'
    if version != upstream:
        raise ValueError(f'{name} requires the version-matched SDL {upstream} workflow artifact.')
    return candidate


CHECKSUM_LINE = re.compile(r'([0-9a-fA-F]{64}) [ *]([^/\\]+)')


def read_checksums(data):
    """Parse a SHA256SUMS file with LF or CRLF line endings into {file name: lowercase digest}."""
    entries = {}
    for line in data.decode('ascii').splitlines():
        if not line.strip():
            continue
        match = CHECKSUM_LINE.fullmatch(line.rstrip())
        if not match or match[2] in ('.', '..') or match[2] in entries:
            raise ValueError(f'Unexpected SHA256SUMS line: {line!r}')
        entries[match[2]] = match[1].lower()
    if not entries:
        raise ValueError('SHA256SUMS lists no files.')
    return entries


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_checksums(directory, pin=None):
    """Check every file a workflow artifact's SHA256SUMS lists; return {name: digest}."""
    data = (directory / 'SHA256SUMS').read_bytes()
    if pin is not None and hashlib.sha256(data).hexdigest() != pin.lower():
        raise ValueError('SHA256SUMS differs from the supplied pin.')
    entries = read_checksums(data)
    for name, expected in entries.items():
        if file_sha256(directory / name) != expected:
            raise ValueError(f'{name} does not match SHA256SUMS.')
    return entries


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    verify = commands.add_parser('verify', help='check the files listed in an artifact directory\'s SHA256SUMS')
    verify.add_argument('directory', type=Path)
    verify.add_argument('--pin', help='expected SHA-256 of the SHA256SUMS file itself')
    args = parser.parse_args(argv)
    if args.pin is not None and not re.fullmatch(r'[0-9a-fA-F]{64}', args.pin):
        parser.error('--pin takes a SHA-256 hex digest')
    for name in verify_checksums(args.directory, args.pin):
        print(f'{name}: OK')


if __name__ == '__main__':
    try:
        main()
    except (OSError, UnicodeDecodeError, ValueError) as error:
        sys.exit(f'Error: {error}')
