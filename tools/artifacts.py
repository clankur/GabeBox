"""Inspect PE metadata and verify SDL build artifacts for staging and deployment."""

import hashlib
import re
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
