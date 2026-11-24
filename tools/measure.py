"""Summarize distinct native game presents in one scene from fhd-present-times.bin."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import struct

HEADER = struct.Struct('<IIQQQ')
PRESENT = struct.Struct('<QQII')
FILETIME_EPOCH = 116444736000000000
PRODUCER_STALL = re.compile(
    r'^(?:steam_shadow_loader=)?dxgi stall qpc=([0-9]{1,20}) enter=([0-9]{1,20}) lock=([0-9]{1,20}) '
    r'split=([0-9]{1,20}) present=([0-9]{1,20}) end=([0-9]{1,20}) prepace=([01])\r?$',
    re.M,
)


def reaches_gpu_60(sample):
    if not isinstance(sample, dict):
        return False
    duration = sample.get('duration_s')
    frames = sample.get('presented')
    rolling = sample.get('min_rolling_1s_fps')
    return (sample.get('hardware_adapter_verified') is True and type(duration) in (float, int)
            and math.isfinite(duration) and duration >= 30
            and type(frames) is int and frames / duration >= 59.5
            and sample.get('shared_texture_presented') == frames and sample.get('cpu_presented') == 0
            and type(rolling) is int and rolling >= 57)


def presentation_window(trace, start, utc, duration):
    if len(trace) < HEADER.size:
        raise ValueError('Missing trace header.')
    magic, version, frequency, filetime, anchor = HEADER.unpack_from(trace)
    if magic != 0x31444846 or version not in (1, 2) or not frequency:
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
    modes = Counter(row[3] for row in selected)
    if modes.keys() - {1, 2}:
        raise ValueError('Unknown presentation mode.')
    # A resize or transport change starts a new sequence. Within a transport,
    # the same producer sequence must never count twice in succession.
    if any(a[3] == b[3] and a[1] == b[1] for a, b in zip(selected, selected[1:])):
        raise ValueError('The same source frame was submitted twice.')
    return version, frequency, anchor, begin, end, epoch + start, selected, modes


def summarize(trace, start, utc, duration):
    version, frequency, _, begin, end, time_utc, selected, modes = presentation_window(trace, start, utc, duration)
    times = [row[0] for row in selected]

    # A rolling count can drop when a frame leaves its one-second window.
    minimum = min(bisect_left(times, begin + frequency), len(times) - bisect_left(times, end - frequency))
    for i, tick in enumerate(times):
        if tick >= end - frequency:
            break
        minimum = min(minimum, bisect_right(times, tick + frequency, lo=i+1) - i - 1)

    intervals = sorted((b - a) * 1000 / frequency for a, b in zip(times, times[1:]))
    def percentile(fraction):
        return round(intervals[math.ceil(fraction * len(intervals)) - 1], 3) if intervals else None

    result = {'start_utc': datetime.fromtimestamp(time_utc, timezone.utc).isoformat(),
              'duration_s': duration, 'presented': len(times), 'fps': round(len(times) / duration, 3),
              'min_rolling_1s_fps': minimum, 'p50_ms': percentile(.50), 'p95_ms': percentile(.95),
              'p99_ms': percentile(.99), 'worst_ms': percentile(1),
              'intervals_over_33_33ms': sum(gap > 1000 / 30 for gap in intervals),
              'cpu_presented': modes[1], 'shared_texture_presented': modes[2],
              'hardware_adapter_verified': version == 2 and bool(times) and modes[2] == len(times)}
    result['gpu_60fps'] = reaches_gpu_60(result)
    return result


def producer_stall_stages(log):
    for match in PRODUCER_STALL.finditer(log):
        timestamps = tuple(map(int, match.groups()[:6]))
        if timestamps[0] == 0 or timestamps[-1] > 0xFFFFFFFFFFFFFFFF or any(
                a > b for a, b in zip(timestamps, timestamps[1:])):
            raise ValueError('Inconsistent producer stall timestamps.')
        preparation = ('pacing', 'publication') if match[7] == '1' else ('publication', 'pacing')
        stages = ('between_present_calls', 'adapter_lock', *preparation, 'native_present')
        yield list(zip(stages, timestamps, timestamps[1:]))


def diagnose_stalls(trace, start, utc, duration, producer_log=None):
    _, frequency, anchor, _, _, _, frames, _ = presentation_window(trace, start, utc, duration)
    producer = list(producer_stall_stages(producer_log)) if producer_log is not None else []
    long_gaps = [(right[0] - left[0], left, right) for left, right in zip(frames, frames[1:])
                 if (right[0] - left[0]) * 30 > frequency]
    largest = sorted(long_gaps, key=lambda gap: gap[0], reverse=True)[:8]
    result = []
    for span, left, right in largest:
        begin, end = left[0], right[0]
        overlapping = []
        for stages in producer:
            phases = {name: round((min(phase_end, end) - max(phase_begin, begin)) * 1000 / frequency, 3)
                      for name, phase_begin, phase_end in stages if max(phase_begin, begin) < min(phase_end, end)}
            if phases:
                overlapping.append(phases)
        source_delta = right[1] - left[1] if left[3] == right[3] == 2 and right[1] > left[1] else None
        result.append({'start_offset_s': round((begin - anchor) / frequency, 6),
                       'gap_ms': round(span * 1000 / frequency, 3), 'gpu_source_sequence_delta': source_delta,
                       'producer_stage_overlap_ms': overlapping if producer_log is not None else None})
    return {'longest_gaps': result, 'producer_stall_records': len(producer) if producer_log is not None else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    origin = parser.add_mutually_exclusive_group()
    origin.add_argument('--start-utc', type=lambda s: datetime.fromisoformat(s.replace('Z', '+00:00')))
    origin.add_argument('--start-offset-s', type=float, default=0)
    parser.add_argument('--duration', type=float, default=30)
    parser.add_argument('--require-gpu-60', action='store_true', help='require sustained 60 fps on the verified hardware adapter')
    parser.add_argument('--diagnose-stalls', action='store_true', help='include source sequence changes at the longest presenter gaps')
    parser.add_argument('--producer-log', type=Path, help='correlate DXGI timings from the same Xbox run’s loader trace or saved report')
    args = parser.parse_args()
    try:
        trace = args.trace.read_bytes()
        result = summarize(trace, args.start_offset_s, args.start_utc, args.duration)
        if args.diagnose_stalls or args.producer_log:
            log = None
            if args.producer_log:
                raw = args.producer_log.read_bytes()
                encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
                log = raw.decode(encoding, errors='replace')
            result['stall_diagnostics'] = diagnose_stalls(trace, args.start_offset_s, args.start_utc, args.duration, log)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    if args.require_gpu_60 and not result['gpu_60fps']:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
