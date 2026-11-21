"""Summarize distinct native game presents in one scene from fhd-present-times.bin."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import struct

HEADER = struct.Struct('<IIQQQ')
PRESENT = struct.Struct('<QQII')
FILETIME_EPOCH = 116444736000000000


def summarize(trace, start, utc, duration):
    if len(trace) < HEADER.size:
        raise ValueError('Missing trace header.')
    magic, version, frequency, filetime, anchor = HEADER.unpack_from(trace)
    if magic != 0x31444846 or version != 1 or not frequency:
        raise ValueError('Unsupported presentation trace.')
    epoch = (filetime - FILETIME_EPOCH) / 10_000_000
    if utc is not None:
        if utc.tzinfo is None:
            raise ValueError('Provide a UTC offset for the scene timestamp.')
        start = utc.timestamp() - epoch
    if not math.isfinite(start) or not math.isfinite(duration) or start < 0 or duration < 1:
        raise ValueError('Use a nonnegative start and a duration of at least one second.')
    rows = list(PRESENT.iter_unpack(trace[HEADER.size:]))
    if not rows or any(a[0] > b[0] or a[2] + 1 != b[2] for a, b in zip(rows, rows[1:])):
        raise ValueError('Missing or inconsistent presentation records.')
    begin = anchor + round(start * frequency)
    end = begin + round(duration * frequency)
    if rows[-1][0] < end:
        raise ValueError('The recorded frames do not cover the requested window.')

    selected = [row for row in rows if begin <= row[0] < end]
    times = [row[0] for row in selected]
    modes = Counter(row[3] for row in selected)
    if modes.keys() - {1, 2}:
        raise ValueError('Unknown presentation mode.')
    # A resize or transport change starts a new sequence. Within a transport,
    # the same producer sequence must never count twice in succession.
    if any(a[3] == b[3] and a[1] == b[1] for a, b in zip(selected, selected[1:])):
        raise ValueError('The same source frame was submitted twice.')

    # A rolling count can drop when a frame leaves its one-second window.
    minimum = min(bisect_left(times, begin + frequency), len(times) - bisect_left(times, end - frequency))
    for i, tick in enumerate(times):
        if tick >= end - frequency:
            break
        minimum = min(minimum, bisect_right(times, tick + frequency, lo=i+1) - i - 1)

    intervals = sorted((b - a) * 1000 / frequency for a, b in zip(times, times[1:]))
    def percentile(fraction):
        return round(intervals[math.ceil(fraction * len(intervals)) - 1], 3) if intervals else None

    return {'start_utc': datetime.fromtimestamp(epoch + start, timezone.utc).isoformat(),
            'duration_s': duration, 'presented': len(times), 'fps': round(len(times) / duration, 3),
            'min_rolling_1s_fps': minimum, 'p50_ms': percentile(.50), 'p95_ms': percentile(.95),
            'p99_ms': percentile(.99), 'worst_ms': percentile(1),
            'intervals_over_33_33ms': sum(gap > 1000 / 30 for gap in intervals),
            'cpu_presented': modes[1], 'shared_texture_presented': modes[2]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    origin = parser.add_mutually_exclusive_group()
    origin.add_argument('--start-utc', type=lambda s: datetime.fromisoformat(s.replace('Z', '+00:00')))
    origin.add_argument('--start-offset-s', type=float, default=0)
    parser.add_argument('--duration', type=float, default=30)
    args = parser.parse_args()
    try:
        result = summarize(args.trace.read_bytes(), args.start_offset_s, args.start_utc, args.duration)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
