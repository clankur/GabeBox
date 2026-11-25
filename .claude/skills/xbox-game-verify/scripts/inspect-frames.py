#!/usr/bin/env python3
"""Screen gameplay captures for rendering artifacts and missing content.

Reads the PNG and BMP captures a verification session collected and reports, per
frame, the statistics that catch this layer's characteristic failures: a black or
flat frame, a scene with no detail, magenta missing-texture blocks, garbage noise,
and frames that never change while the game was being driven. With --reference it
also compares each capture against a known-good set for the same title and says
which reference it resembles and how closely.

This is a screen, not a judgement. It narrows hundreds of frames to the few worth
opening, and a frame it passes can still be wrong in ways only someone who knows
the game will see — a missing NPC, the wrong skybox, a UI layer that vanished.
Always look at the captures it points at, and at a sample of the ones it clears.

    inspect-frames.py verify-run --json frames.json
    inspect-frames.py verify-run/phase-wander-shots --reference analysis/hades-reference

Uses only the standard library, as AGENTS.md asks of core commands.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

MAX_SAMPLES = 240_000      # cap the pixels examined per frame
SIGNATURE = 16             # coarse signature grid, for reference comparison

THRESHOLDS = {
    'black_fraction': 0.98,       # the frame is black
    'flat_fraction': 0.90,        # one colour covers the frame
    'white_fraction': 0.90,       # blown out
    'detail_low': 1.5,            # neighbour delta: nothing is drawn
    'detail_noise': 70.0,         # neighbour delta: garbage, not a scene
    'magenta_fraction': 0.02,     # classic missing-texture marker
    'static_delta': 0.75,         # consecutive frames that never changed
    'reference_distance': 28.0,   # unlike every known-good reference
}


# -- image reading ---------------------------------------------------------------

def read_png(data: bytes) -> tuple:
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, header = 8, bytearray(), None
    while pos + 8 <= len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body[:13])
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
        pos += 12 + length
    if header is None:
        raise ValueError('PNG without IHDR')
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError(f'unsupported PNG: depth {depth}, colour type {colour}, interlace {interlace}')
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(stride * height)
    previous = bytearray(stride)
    pos = 0
    for row in range(height):
        filter_type = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upleft = previous[i - channels] if i >= channels else 0
                up = previous[i]
                estimate = left + up - upleft
                da, db, dc = abs(estimate - left), abs(estimate - up), abs(estimate - upleft)
                nearest = left if da <= db and da <= dc else up if db <= dc else upleft
                line[i] = (line[i] + nearest) & 0xFF
        elif filter_type:
            raise ValueError(f'unknown PNG filter {filter_type}')
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return width, height, channels, bytes(out)


def read_bmp(data: bytes) -> tuple:
    if data[:2] != b'BM':
        raise ValueError('not a BMP')
    offset, = struct.unpack('<I', data[10:14])
    width, height, planes, bits = struct.unpack('<iiHH', data[18:30])
    if planes != 1 or bits not in (24, 32):
        raise ValueError(f'unsupported BMP: {bits} bits, {planes} planes')
    channels = bits // 8
    flip = height > 0
    height = abs(height)
    stride = (width * channels + 3) & ~3
    out = bytearray(width * height * 3)
    for row in range(height):
        source = offset + (height - 1 - row if flip else row) * stride
        line = data[source:source + width * channels]
        for column in range(width):
            blue, green, red = line[column * channels:column * channels + 3]
            target = (row * width + column) * 3
            out[target:target + 3] = bytes((red, green, blue))
    return width, height, 3, bytes(out)


def load(path: Path) -> tuple:
    data = path.read_bytes()
    return read_png(data) if data[:8] == b'\x89PNG\r\n\x1a\n' else read_bmp(data)


# -- measurement -----------------------------------------------------------------

def measure(path: Path) -> dict:
    width, height, channels, pixels = load(path)
    step = max(1, int(((width * height) / MAX_SAMPLES) ** 0.5))
    rows = range(0, height, step)
    columns = range(0, width, step)
    counts: dict[tuple, int] = {}
    black = white = magenta = 0
    total = 0
    sums = [0, 0, 0]
    squares = 0
    detail = 0
    comparisons = 0
    signature = [[0, 0] for _ in range(SIGNATURE * SIGNATURE)]
    previous_row: list[tuple] | None = None
    for row in rows:
        line: list[tuple] = []
        for column in columns:
            index = (row * width + column) * channels
            red, green, blue = pixels[index], pixels[index + 1], pixels[index + 2]
            line.append((red, green, blue))
            total += 1
            sums[0] += red
            sums[1] += green
            sums[2] += blue
            luma = (red * 299 + green * 587 + blue * 114) // 1000
            squares += luma * luma
            if red < 8 and green < 8 and blue < 8:
                black += 1
            if red > 247 and green > 247 and blue > 247:
                white += 1
            if red > 200 and blue > 200 and green < 60:
                magenta += 1
            key = (red >> 3, green >> 3, blue >> 3)
            counts[key] = counts.get(key, 0) + 1
            cell = (row * SIGNATURE // height) * SIGNATURE + (column * SIGNATURE // width)
            signature[cell][0] += luma
            signature[cell][1] += 1
        for index in range(1, len(line)):
            detail += sum(abs(a - b) for a, b in zip(line[index], line[index - 1])) / 3
            comparisons += 1
        if previous_row is not None:
            for a, b in zip(line, previous_row):
                detail += sum(abs(x - y) for x, y in zip(a, b)) / 3
                comparisons += 1
        previous_row = line
    modal = max(counts.values()) if counts else 0
    mean_luma = (sums[0] * 299 + sums[1] * 587 + sums[2] * 114) / (1000 * total) if total else 0
    variance = squares / total - mean_luma ** 2 if total else 0
    return {
        'file': path.name,
        'path': str(path),
        'width': width,
        'height': height,
        'sampled_pixels': total,
        'mean_rgb': [round(value / total, 1) for value in sums] if total else [0, 0, 0],
        'luma_stddev': round(max(variance, 0) ** 0.5, 2),
        'black_fraction': round(black / total, 4) if total else 0,
        'white_fraction': round(white / total, 4) if total else 0,
        'magenta_fraction': round(magenta / total, 4) if total else 0,
        'flat_fraction': round(modal / total, 4) if total else 0,
        'distinct_colours': len(counts),
        'detail': round(detail / comparisons, 2) if comparisons else 0,
        'signature': [round(cell[0] / cell[1], 1) if cell[1] else 0.0 for cell in signature],
    }


def flags(frame: dict) -> list[dict]:
    found = []
    if frame['black_fraction'] >= THRESHOLDS['black_fraction']:
        found.append({'flag': 'black-frame',
                      'detail': f"{frame['black_fraction']:.1%} of sampled pixels are black"})
    elif frame['flat_fraction'] >= THRESHOLDS['flat_fraction']:
        found.append({'flag': 'flat-frame',
                      'detail': f"one colour covers {frame['flat_fraction']:.1%} of the frame"})
    if frame['white_fraction'] >= THRESHOLDS['white_fraction']:
        found.append({'flag': 'blown-out',
                      'detail': f"{frame['white_fraction']:.1%} of sampled pixels are white"})
    if frame['magenta_fraction'] >= THRESHOLDS['magenta_fraction']:
        found.append({'flag': 'magenta-blocks',
                      'detail': f"{frame['magenta_fraction']:.1%} magenta, the usual missing-texture fill"})
    if frame['detail'] < THRESHOLDS['detail_low'] and frame['black_fraction'] < THRESHOLDS['black_fraction']:
        found.append({'flag': 'low-detail',
                      'detail': f"neighbour delta {frame['detail']}: little or nothing is drawn"})
    if frame['detail'] > THRESHOLDS['detail_noise']:
        found.append({'flag': 'noise',
                      'detail': f"neighbour delta {frame['detail']}: high-frequency garbage, not a scene"})
    return found


def signature_distance(left: list, right: list) -> float:
    return sum(abs(a - b) for a, b in zip(left, right)) / len(left)


def compare_frames(frames: list[dict], motion_phases: tuple) -> list[dict]:
    """Consecutive captures that never changed while the game was being driven.

    Only phases where movement was actually driven can fail this: a title screen or a
    paused menu is legitimately still, and flagging it would train the reader to ignore
    the flag that matters.
    """
    issues = []
    for previous, frame in zip(frames, frames[1:]):
        if previous.get('phase') != frame.get('phase'):
            continue
        if frame.get('phase') not in motion_phases:
            continue
        distance = signature_distance(previous['signature'], frame['signature'])
        frame['delta_from_previous'] = round(distance, 2)
        if distance < THRESHOLDS['static_delta']:
            issues.append({'flag': 'static-scene', 'file': frame['file'],
                           'detail': f"identical to {previous['file']} (delta {distance:.2f}) "
                                     'although input was being sent'})
    return issues


def compare_reference(frames: list[dict], references: list[dict]) -> list[dict]:
    issues = []
    for frame in frames:
        nearest = min(references, key=lambda item: signature_distance(frame['signature'], item['signature']))
        distance = signature_distance(frame['signature'], nearest['signature'])
        frame['nearest_reference'] = nearest['file']
        frame['reference_distance'] = round(distance, 2)
        if distance > THRESHOLDS['reference_distance']:
            issues.append({'flag': 'unlike-reference', 'file': frame['file'],
                           'detail': f"nearest known-good frame {nearest['file']} is {distance:.1f} away; "
                                     'open both and say what differs'})
    return issues


def collect(target: Path) -> list[Path]:
    if target.is_file():
        return [target]
    return sorted(path for path in target.rglob('*')
                  if path.suffix.lower() in ('.png', '.bmp') and path.is_file())


def normalise(name: str) -> str:
    return name[len('phase-'):].removesuffix('-shots') if name.startswith('phase-') else name


def phase_of(path: Path, root: Path) -> str:
    for part in path.relative_to(root).parts if path != root else ():
        if part.startswith('phase-'):
            return normalise(part)
    return normalise(path.parent.name)


def main() -> int:
    parser = argparse.ArgumentParser(description='Screen gameplay captures for rendering artifacts.')
    parser.add_argument('target', type=Path, help='a capture, or a session directory to walk')
    parser.add_argument('--reference', type=Path, help='directory of known-good captures for this title')
    parser.add_argument('--json', type=Path, help='write the full measurements here')
    parser.add_argument('--motion-phase', action='append', default=None, metavar='NAME',
                        help='phase whose captures must change between frames '
                             '(default: wander, interact)')
    parser.add_argument('--fail-on-flag', action='store_true', help='exit 1 when any frame is flagged')
    parser.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                        help=f"adjust a threshold: {', '.join(sorted(THRESHOLDS))}")
    args = parser.parse_args()
    args.motion_phase = args.motion_phase or ['wander', 'interact']

    for entry in args.set:
        name, _, value = entry.partition('=')
        if name not in THRESHOLDS:
            parser.error(f'unknown threshold {name!r}; choose from {", ".join(sorted(THRESHOLDS))}')
        try:
            THRESHOLDS[name] = float(value)
        except ValueError:
            parser.error(f'threshold {name} needs a number, not {value!r}')

    paths = collect(args.target)
    if not paths:
        print(f'No PNG or BMP captures under {args.target}.', file=sys.stderr)
        return 2

    frames, unreadable = [], []
    for path in paths:
        try:
            frame = measure(path)
        except (ValueError, zlib.error, struct.error, IndexError) as error:
            unreadable.append({'file': path.name, 'error': str(error)})
            continue
        frame['phase'] = phase_of(path, args.target if args.target.is_dir() else args.target.parent)
        frame['flags'] = flags(frame)
        frames.append(frame)

    issues = [dict(item, file=frame['file'], phase=frame['phase'])
              for frame in frames for item in frame['flags']]
    issues += compare_frames(frames, tuple(args.motion_phase))
    if args.reference:
        references = []
        for path in collect(args.reference):
            try:
                references.append(measure(path))
            except (ValueError, zlib.error, struct.error, IndexError) as error:
                unreadable.append({'file': path.name, 'error': str(error)})
        if references:
            issues += compare_reference(frames, references)
        else:
            print(f'No readable references under {args.reference}.', file=sys.stderr)

    report = {'captures': len(frames), 'flagged': len({item['file'] for item in issues}),
              'issues': issues, 'unreadable': unreadable, 'frames': frames}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    print(f'{len(frames)} capture(s) screened, {report["flagged"]} flagged.')
    for frame in frames:
        marks = ', '.join(item['flag'] for item in frame['flags']) or 'clean'
        extra = ''
        if 'reference_distance' in frame:
            extra = f" · nearest {frame['nearest_reference']} ({frame['reference_distance']})"
        print(f"  {frame['phase']:10s} {frame['file']:28s} {frame['width']}x{frame['height']} "
              f"detail {frame['detail']:6.2f}  colours {frame['distinct_colours']:6d}  {marks}{extra}")
    for item in issues:
        print(f"  ! {item['flag']}: {item['file']} — {item['detail']}")
    for item in unreadable:
        print(f"  ? unreadable: {item['file']} — {item['error']}")
    if frames:
        print('\nOpen the flagged captures, and a sample of the clean ones: this screen cannot '
              'see a missing NPC, the wrong skybox, or a UI layer that failed to draw.')
    return 1 if args.fail_on_flag and issues else 0


if __name__ == '__main__':
    sys.exit(main())
