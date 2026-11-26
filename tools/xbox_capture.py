"""Read-only evidence collection through Xbox Device Portal: process samples, screenshots and package files.

Every command here only reads from the console. Each is independent of the Steam AppID and of
any title: select processes by package and executable name, and files by package folder.
"""

from datetime import datetime, timezone
import hashlib
import json
from pathlib import PureWindowsPath
import re
import struct
import time
import zlib
from urllib.parse import urlencode

from .device_portal import PortalError, PortalRetry, PortalUnavailable, utc_text
from .measure import FILETIME_EPOCH


PROCESSES = '/api/resourcemanager/processes'
SCREENSHOT = '/ext/screenshot?download=true&hdr=false'
SCREENSHOT_LIMIT = 32 * 1024 * 1024
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
# Windows Error Reporting starts for a crash in any process, including games launched by Steam.
CRASH_IMAGES = ('WerFault.exe',)
MIB = 1024 * 1024
PACKAGE_FOLDER = re.compile(r'\\(?:LocalState|TempState|LocalCache|RoamingState|AC)'
                            r'(?:\\[^\\/:*?"<>|\x00-\x1f]{1,255})*')
FILE_NAME = re.compile(r'[^\\/:*?"<>|\x00-\x1f]{1,255}')
FILE_LIMIT = 256 * 1024 * 1024


def package_folder(folder):
    parts = folder.split('\\')[1:]
    if not PACKAGE_FOLDER.fullmatch(folder) or any(part in ('.', '..') or part != part.strip(' .') for part in parts):
        raise ValueError('Use a package folder such as \\LocalState or \\TempState\\SteamWin32Trial (12).')
    return folder


def file_name(name):
    if not FILE_NAME.fullmatch(name) or name in ('.', '..') or name != name.strip(' .'):
        raise ValueError('Use a single file name without a folder.')
    return name


def filetime_utc(value):
    if type(value) is not int or value <= FILETIME_EPOCH:
        return None
    return utc_text((value - FILETIME_EPOCH) / 10_000_000)


def utc_filetime(text):
    moment = datetime.fromisoformat(text.replace('Z', '+00:00'))
    if moment.tzinfo is None:
        raise ValueError('Give --since-utc with a UTC offset, for example 2026-11-25T19:08:00Z.')
    return FILETIME_EPOCH + round(moment.astimezone(timezone.utc).timestamp() * 10_000_000)


def selected_processes(processes, package_full_name, images):
    wanted = {image.casefold() for image in images}
    rows = []
    for item in processes:
        image = PureWindowsPath(str(item.get('ImageName', ''))).name
        if not (package_full_name and item.get('PackageFullName') == package_full_name) and image.casefold() not in wanted:
            continue
        pid = item.get('ProcessId')
        if type(pid) is not int:
            continue

        def mib(key):
            value = item.get(key)
            return round(value / MIB, 1) if type(value) is int and value >= 0 else None

        cpu = item.get('CPUUsage')
        rows.append({'image': image, 'pid': pid,
                     'cpu_percent': round(cpu, 1) if type(cpu) in (int, float) else None,
                     'private_working_set_mib': mib('PrivateWorkingSet'), 'total_commit_mib': mib('TotalCommit'),
                     'packaged': bool(package_full_name and item.get('PackageFullName') == package_full_name)})
    return sorted(rows, key=lambda row: (row['image'].casefold(), row['pid']))


def sample_processes(portal, package_full_name, images, seconds, interval, output, *,
                     clock=time.monotonic, wall_clock=time.time, pause=time.sleep, progress=None):
    """Write one JSON line per sample and return a summary of every process seen.

    The package's own processes and the named executables are sampled. A process that starts or
    exits between samples produces an event line. A sample that Device Portal cannot answer
    after bounded retries is written as unavailable and sampling continues.
    """
    start = clock()
    end = start + seconds
    retry = PortalRetry(clock=clock, wall_clock=wall_clock, pause=pause, deadline=end, progress=progress)
    seen = {}
    previous = None
    summary = {'samples': 0, 'unavailable_samples': 0, 'peak_package_commit_mib': None}
    with output.open('x', encoding='utf-8') as lines:
        def write(row):
            lines.write(json.dumps(row) + '\n')
            lines.flush()

        index = 0
        while True:
            moment = utc_text(wall_clock())
            try:
                _, raw = retry.request(portal, 'GET', PROCESSES, read_limit=4 * MIB)
                rows = selected_processes(json.loads(raw)['Processes'], package_full_name, images)
            except PortalUnavailable:
                summary['unavailable_samples'] += 1
                write({'utc': moment, 'unavailable': True})
            else:
                summary['samples'] += 1
                current = {(row['image'], row['pid']) for row in rows}
                if previous is not None:
                    for image, pid in sorted(current - previous):
                        write({'utc': moment, 'event': 'started', 'image': image, 'pid': pid})
                    for image, pid in sorted(previous - current):
                        write({'utc': moment, 'event': 'exited', 'image': image, 'pid': pid})
                previous = current
                commit = round(sum(row['total_commit_mib'] or 0 for row in rows if row['packaged']), 1)
                write({'utc': moment, 'package_commit_mib': commit, 'processes': rows})
                if summary['peak_package_commit_mib'] is None or commit > summary['peak_package_commit_mib']:
                    summary['peak_package_commit_mib'] = commit
                for row in rows:
                    entry = seen.setdefault((row['image'], row['pid']), {
                        'image': row['image'], 'pid': row['pid'], 'first_utc': moment, 'samples': 0,
                        'peak_total_commit_mib': None, 'peak_private_working_set_mib': None, 'max_cpu_percent': None})
                    entry['last_utc'] = moment
                    entry['samples'] += 1
                    for key, value in (('peak_total_commit_mib', row['total_commit_mib']),
                                       ('peak_private_working_set_mib', row['private_working_set_mib']),
                                       ('max_cpu_percent', row['cpu_percent'])):
                        if value is not None and (entry[key] is None or value > entry[key]):
                            entry[key] = value
            index += 1
            next_sample = start + index * interval
            if next_sample > end:
                break
            pause(max(0, next_sample - clock()))
    summary['processes'] = sorted(seen.values(), key=lambda entry: (entry['image'].casefold(), entry['pid']))
    portal_summary = retry.summary()
    if portal_summary:
        summary['portal'] = portal_summary
    return summary


def png_size(data):
    if len(data) < 24 or not data.startswith(PNG_SIGNATURE) or data[12:16] != b'IHDR':
        return None
    return struct.unpack('>II', data[16:24])


# Bytes per pixel for 8- and 16-bit greyscale, RGB, palette, greyscale-alpha and RGBA PNGs.
PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def png_uniform_color(data):
    """Return the single colour of a PNG whose pixels are all equal, as hex, or None.

    The filtered rows of a uniform image are fixed by its first pixel, so each row is compared
    with that encoding instead of being unfiltered. Interlaced, sub-byte or malformed images
    return None.
    """
    size = png_size(data)
    if size is None or len(data) < 33:
        return None
    width, height = size
    depth, color, _, _, interlace = data[24:29]
    if color not in PNG_CHANNELS or depth not in (8, 16) or interlace or not width or not height:
        return None
    bpp = PNG_CHANNELS[color] * depth // 8
    stride = width * bpp
    expected_size = height * (stride + 1)
    compressed = bytearray()
    palette = b''
    offset = 8
    while offset + 12 <= len(data):
        length, kind = struct.unpack('>I4s', data[offset:offset + 8])
        if kind == b'IDAT':
            compressed += data[offset + 8:offset + 8 + length]
        elif kind == b'PLTE':
            palette = data[offset + 8:offset + 8 + length]
        elif kind == b'IEND':
            break
        offset += 12 + length
    try:
        inflater = zlib.decompressobj()
        raw = inflater.decompress(bytes(compressed), expected_size + 1)
    except zlib.error:
        return None
    if len(raw) != expected_size:
        return None
    # Row 0 sees a zero row above it, so every filter stores its first pixel unchanged.
    pixel = raw[1:1 + bpp]
    rest = bytes(stride - bpp)
    half = bytes(value - value // 2 for value in pixel)
    # Expected bytes per filter type (None, Sub, Up, Average, Paeth) for the first and later rows.
    first_rows = {0: pixel * width, 1: pixel + rest, 2: pixel * width, 3: pixel + half * (width - 1), 4: pixel + rest}
    later_rows = {0: pixel * width, 1: pixel + rest, 2: bytes(stride), 3: half + rest, 4: bytes(stride)}
    for row in range(height):
        start = row * (stride + 1)
        rows = first_rows if row == 0 else later_rows
        if rows.get(raw[start]) != raw[start + 1:start + 1 + stride]:
            return None
    if color == 3:
        pixel = palette[3 * pixel[0]:3 * pixel[0] + 3]
        if len(pixel) != 3:
            return None
    return '#' + pixel.hex()


def capture_screenshots(portal, output, delay, interval, count, *,
                        clock=time.monotonic, wall_clock=time.time, pause=time.sleep, progress=None):
    """Save Device Portal screenshots on a schedule and index each one with its capture time.

    Each capture briefly stalls presentation on the console, so the index records the UTC
    start and end of every capture for excluding it from frame-rate windows.
    """
    start = clock()
    retry = PortalRetry(clock=clock, wall_clock=wall_clock, pause=pause, progress=progress)
    output.mkdir(mode=0o700)
    entries = []
    with (output / 'screenshots.jsonl').open('x', encoding='utf-8') as index:
        for number in range(1, count + 1):
            offset = delay + (number - 1) * interval
            pause(max(0, start + offset - clock()))
            entry = {'offset_s': offset, 'start_utc': utc_text(wall_clock())}
            began = clock()
            try:
                _, data = retry.request(portal, 'GET', SCREENSHOT, read_limit=SCREENSHOT_LIMIT + 1)
            except PortalUnavailable:
                entry['error'] = 'portal-unavailable'
            except PortalError as error:
                entry['error'] = f'http-{error.status}'
            else:
                size = png_size(data)
                if size is None or len(data) > SCREENSHOT_LIMIT:
                    entry['error'] = 'not-a-png'
                else:
                    name = f'shot-{number:03d}.png'
                    with (output / name).open('xb') as file:
                        file.write(data)
                    entry.update(file=name, width=size[0], height=size[1], bytes=len(data),
                                 sha256=hashlib.sha256(data).hexdigest())
                    uniform = png_uniform_color(data)
                    if uniform:
                        # Device Portal returns a uniform frame, often white, for surfaces it cannot read,
                        # such as the Big Picture overlay. Keep it, but mark it as not showing content.
                        entry['uniform_color'] = uniform
            entry['end_utc'] = utc_text(wall_clock())
            entry['capture_ms'] = round((clock() - began) * 1000)
            index.write(json.dumps(entry) + '\n')
            index.flush()
            entries.append(entry)
            if progress:
                note = f' uniform {entry["uniform_color"]}' if 'uniform_color' in entry else ''
                progress(f'screenshot {number}/{count} {entry.get("file", entry.get("error"))}{note}')
    result = {'screenshots': entries, 'saved': sum('file' in entry for entry in entries)}
    uniform = sum('uniform_color' in entry for entry in entries)
    if uniform:
        result['uniform'] = uniform
    portal_summary = retry.summary()
    if portal_summary:
        result['portal'] = portal_summary
    return result


def folder_query(package, folder, **extra):
    return urlencode({'knownfolderid': 'LocalAppData', 'packagefullname': package['PackageFullName'],
                      'path': folder, **extra})


def list_files(portal, package, folder, *, recursive=False, since=None, depth=6):
    """List a package folder, optionally recursively, keeping files created or changed since a FILETIME."""
    found = []

    def walk(path, level):
        _, raw = portal.request('GET', '/api/filesystem/apps/files?' + folder_query(package, path), read_limit=8 * MIB)
        for item in json.loads(raw).get('Items', []):
            name = item.get('Name')
            if not isinstance(name, str) or not FILE_NAME.fullmatch(name):
                continue
            child = path + '\\' + name
            directory = item.get('Type') == 16
            created, modified = item.get('DateCreated'), item.get('DateModified')
            if directory and recursive and level < depth:
                walk(child, level + 1)
            if since is not None and not any(type(value) is int and value >= since for value in (created, modified)):
                continue
            entry = {'path': child, 'type': 'directory' if directory else 'file',
                     'created_utc': filetime_utc(created), 'modified_utc': filetime_utc(modified)}
            if not directory and type(item.get('FileSize')) is int:
                entry['bytes'] = item['FileSize']
            found.append(entry)

    walk(package_folder(folder), 0)
    return sorted(found, key=lambda entry: entry['path'].casefold())


def get_file(portal, package, folder, name, destination, limit=FILE_LIMIT):
    """Stream one package file to a new local file and return its size and SHA-256."""
    query = folder_query(package, package_folder(folder), filename=file_name(name))
    digest = hashlib.sha256()
    total = 0
    created = False

    def receive(response):
        nonlocal total, created
        with destination.open('xb') as file:
            created = True
            while chunk := response.read(MIB):
                total += len(chunk)
                if total > limit:
                    raise ValueError(f'{name} is larger than {limit} bytes.')
                digest.update(chunk)
                file.write(chunk)

    try:
        portal.request('GET', '/api/filesystem/apps/file?' + query, receive=receive)
    except BaseException:
        if created:
            destination.unlink(missing_ok=True)  # Never leave a partial copy that looks complete.
        raise
    return {'file': str(destination), 'bytes': total, 'sha256': digest.hexdigest()}
