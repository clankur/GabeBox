"""List white flashes recorded by the presenter's opt-in frame probe (fhd-flash-probe.bin).

The probe (runtime/presenter/flash-probe.h) samples the same 16 full-width rows of each frame
in three places: the game's renderbuffer before the producer's copy (`game`), the exchange
texture the presenter draws from (`exchange`), and the presenter's own output after the draw
(`output`). For every white frame this tool says where the white first appears:

- `game`: the game's renderbuffer hashed the same as the exchange texture, so the white frame
  was rendered by the game or Mesa before the exchange.
- `exchange`: the pixels changed between the game's renderbuffer and the exchange texture.
- `presenter-draw`: the exchange texture was not white, but the presenter's output was.
- `unattributed`: no game-side record for that sequence (an older SDL build, or a lost record).

A flash seen at the console with no white record at all happened after the presenter's draw,
in composition or in the display path.
"""

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import struct

HEADER = struct.Struct('<IIQQQIIII')
RECORD = struct.Struct('<QQBBHHHHBBI')
MAGIC = 0x46444846
FILETIME_EPOCH = 116444736000000000
KINDS = {1: 'game', 2: 'exchange', 3: 'output', 4: 'capture'}
ROLES = {1: 'flash', 2: 'before', 3: 'baseline'}
FLASH, UNIFORM_WHITE, MISMATCH = 1, 2, 4
PRESENT_HEADER = struct.Struct('<IIQQQ')
PRESENT = struct.Struct('<QQII')

# Keep in step with runtime/presenter/flash-detector.h.
WHITE = 235
BASELINE, MIN_BASELINE, LOOKAHEAD = 8, 4, 3
BRIGHT_LUMA, JUMP_LUMA, RETURN_LUMA = 128 * 256, 40 * 256, 16 * 256


def white_jump(cells):
    return max(8, cells // 64)


def white_return(cells):
    return max(4, cells // 128)


def upper_median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0


def classify(before, frame, after):
    """Flags for one frame given up to BASELINE earlier and LOOKAHEAD later frames of its source."""
    flags = UNIFORM_WHITE if frame['cells'] and frame['min'] >= WHITE else 0
    if len(before) < MIN_BASELINE:
        return flags
    luma = upper_median(f['luma'] for f in before)
    whites = upper_median(f['white'] for f in before)
    bright = frame['luma'] >= BRIGHT_LUMA and frame['luma'] >= luma + JUMP_LUMA
    if not bright and frame['white'] < whites + white_jump(frame['cells']):
        return flags
    if any(a['luma'] <= luma + RETURN_LUMA and a['white'] <= whites + white_return(a['cells']) for a in after):
        flags |= FLASH
    return flags


def detect(frames):
    """Recompute the online detector's flash flags for one source's frames in record order."""
    return [classify(frames[max(0, i - BASELINE):i], frame, frames[i + 1:i + 1 + LOOKAHEAD])
            for i, frame in enumerate(frames)]


def parse(raw):
    if len(raw) < HEADER.size or (len(raw) - HEADER.size) % RECORD.size:
        raise ValueError('Not a whole flash probe file.')
    magic, version, frequency, filetime, anchor, rows, cells, white, _ = HEADER.unpack_from(raw)
    if magic != MAGIC or version != 1 or not frequency:
        raise ValueError('Unsupported flash probe file.')
    header = {'frequency': frequency, 'filetime': filetime, 'counter': anchor, 'rows': rows, 'cells': cells,
              'white': white}
    records = []
    for counter, sequence, kind, slot, flags, luma, whites, total, low, high, digest in RECORD.iter_unpack(raw[HEADER.size:]):
        if kind not in KINDS:
            raise ValueError(f'Unknown flash probe record kind {kind}.')
        records.append({'counter': counter, 'sequence': sequence, 'kind': KINDS[kind], 'slot': slot, 'flags': flags,
                        'luma': luma, 'white': whites, 'cells': total, 'min': low, 'max': high, 'hash': digest})
    return header, records


def utc_seconds(header, counter):
    signed = counter - header['counter']
    return (header['filetime'] - FILETIME_EPOCH) / 10_000_000 + signed / header['frequency']


def iso(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


def stamp(header):
    return datetime.fromtimestamp(utc_seconds(header, header['counter']), timezone.utc).strftime('%Y%m%dT%H%M%S')


def flag_names(flags):
    return [name for bit, name in ((FLASH, 'flash'), (UNIFORM_WHITE, 'uniform_white'), (MISMATCH, 'hash_mismatch'))
            if flags & bit]


def present_index(trace):
    """Map each shared-texture source sequence to its first present (index, QPC) in fhd-present-times.bin."""
    if len(trace) < PRESENT_HEADER.size or (len(trace) - PRESENT_HEADER.size) % PRESENT.size:
        raise ValueError('Not a whole presentation trace.')
    magic, version, frequency, filetime, anchor = PRESENT_HEADER.unpack_from(trace)
    if magic != 0x31444846 or version not in (1, 2) or not frequency:
        raise ValueError('Unsupported presentation trace.')
    first = {}
    for counter, sequence, presented, mode in PRESENT.iter_unpack(trace[PRESENT_HEADER.size:]):
        if mode == 2:
            first.setdefault(sequence, (presented, counter))
    epoch = (filetime - FILETIME_EPOCH) / 10_000_000
    return {sequence: {'present_index': index, 'present_utc': iso(epoch + (counter - anchor) / frequency)}
            for sequence, (index, counter) in first.items()}


def frame_view(header, record, recomputed=None):
    view = {'utc': iso(utc_seconds(header, record['counter'])), 'sequence': record['sequence'], 'slot': record['slot'],
            'luma': round(record['luma'] / 256, 2),
            'white_fraction': round(record['white'] / record['cells'], 4) if record['cells'] else 0,
            'min_cell_luma': record['min'], 'max_cell_luma': record['max'], 'hash': f"{record['hash']:08x}",
            'flags': flag_names(record['flags'])}
    if recomputed is not None and (recomputed & (FLASH | UNIFORM_WHITE)) != (record['flags'] & (FLASH | UNIFORM_WHITE)):
        view['offline_flags'] = flag_names(recomputed)
    return view


def analyze(raw, present_trace=None, limit=64):
    header, records = parse(raw)
    by_kind = {kind: [r for r in records if r['kind'] == kind] for kind in ('game', 'exchange', 'output')}
    offline = {kind: detect(frames) for kind, frames in by_kind.items()}
    lookup = {kind: {r['sequence']: (r, offline[kind][i]) for i, r in enumerate(frames)}
              for kind, frames in by_kind.items()}
    presents = present_index(present_trace) if present_trace is not None else {}

    def white(record):
        return bool(record['flags'] & (FLASH | UNIFORM_WHITE))

    sources = {}
    for kind, frames in by_kind.items():
        white_frames = [r for r in frames if white(r)]
        runs = sum(1 for i, r in enumerate(frames) if white(r) and (i == 0 or not white(frames[i - 1])))
        sources[kind] = {'frames': len(frames), 'white_frames': len(white_frames), 'white_runs': runs,
                         'flash_frames': sum(bool(r['flags'] & FLASH) for r in frames),
                         'uniform_white_frames': sum(bool(r['flags'] & UNIFORM_WHITE) for r in frames),
                         'offline_disagreements': sum(
                             (f & (FLASH | UNIFORM_WHITE)) != (r['flags'] & (FLASH | UNIFORM_WHITE))
                             for r, f in zip(frames, offline[kind]))}

    flashes = []
    seen = set()
    for kind in ('exchange', 'output'):
        for i, record in enumerate(by_kind[kind]):
            if not white(record) or record['sequence'] in seen:
                continue
            seen.add(record['sequence'])
            sequence = record['sequence']
            entry = {'sequence': sequence, 'utc': iso(utc_seconds(header, record['counter'])),
                     'offset_s': round((record['counter'] - header['counter']) / header['frequency'], 6)}
            views = {}
            for source in ('game', 'exchange', 'output'):
                found = lookup[source].get(sequence)
                views[source] = frame_view(header, *found) if found else None
            entry.update(views)
            game, exchange = lookup['game'].get(sequence), lookup['exchange'].get(sequence)
            match = game[0]['hash'] == exchange[0]['hash'] if game and exchange else None
            entry['game_exchange_hash_match'] = match
            if exchange and not white(exchange[0]):
                origin = 'presenter-draw'
            elif match is True:
                origin = 'game'
            elif match is False:
                origin = 'exchange'
            else:
                origin = 'unattributed'
            entry['origin'] = origin
            entry.update(presents.get(sequence, {}))
            flashes.append(entry)
    flashes.sort(key=lambda entry: entry['sequence'])

    game_only = [frame_view(header, r, f) for r, f in zip(by_kind['game'], offline['game'])
                 if white(r) and r['sequence'] not in seen]
    pairs = [(sequence, r['hash'], lookup['game'][sequence][0]['hash'])
             for sequence, (r, _) in lookup['exchange'].items() if sequence in lookup['game']]
    mismatched = sorted(sequence for sequence, a, b in pairs if a != b)
    captures = []
    for record in records:
        if record['kind'] != 'capture':
            continue
        role = ROLES.get(record['flags'], 'unknown')
        captures.append({'utc': iso(utc_seconds(header, record['counter'])), 'event': record['slot'], 'role': role,
                         'sequence': record['sequence'],
                         'file': f"fhd-flash-{stamp(header)}-{record['slot']}-{role}-{record['sequence']}.bmp"})

    origins = {}
    for entry in flashes:
        origins[entry['origin']] = origins.get(entry['origin'], 0) + 1
    exchange_ticks = [r['counter'] for r in by_kind['exchange']]
    duration = ((exchange_ticks[-1] - exchange_ticks[0]) / header['frequency']) if len(exchange_ticks) > 1 else 0
    return {'start_utc': iso(utc_seconds(header, header['counter'])),
            'rows': header['rows'], 'cells_per_row': header['cells'], 'white_cell_luma': header['white'],
            'exchange_seconds': round(duration, 3), 'sources': sources,
            'hash_pairs': len(pairs), 'hash_mismatches': len(mismatched), 'mismatched_sequences': mismatched[:limit],
            'white_frames': len(flashes), 'origins': origins, 'flashes': flashes[:limit], 'listed': min(len(flashes), limit),
            'game_only_white_frames': game_only[:limit], 'captures': captures,
            'sampled_fps': round(len(exchange_ticks) / duration, 3) if duration and math.isfinite(duration) else None,
            'first_white_utc': flashes[0]['utc'] if flashes else None,
            'gaps_over_100ms': sum(1 for a, b in zip(exchange_ticks, exchange_ticks[1:])
                                   if (b - a) * 10 > header['frequency']),
            'present_matched': sum(1 for entry in flashes if 'present_index' in entry) if presents else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('probe', type=Path, help='fhd-flash-probe.bin copied from the package LocalState')
    parser.add_argument('--present-trace', type=Path, help="the same run's fhd-present-times.bin, to add present indexes")
    parser.add_argument('--limit', type=int, default=64, help='white frames and mismatches to list (default 64)')
    parser.add_argument('--require-none', action='store_true', help='exit 1 when any white frame was recorded')
    args = parser.parse_args()
    if args.limit < 1:
        parser.error('--limit takes a positive number')
    try:
        raw = args.probe.read_bytes()
        trace = args.present_trace.read_bytes() if args.present_trace else None
        result = analyze(raw, trace, args.limit)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    if args.require_none and result['white_frames']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
