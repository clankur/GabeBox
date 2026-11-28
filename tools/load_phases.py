"""Break one hosted launch into load phases from the files a run already collects.

A run folder holds `launch.utc` (when the package was activated or Play was pressed) and, when
present, `continue.utc`, the loader trace (`trace.txt`), the game log, and the presenter's
`fhd-present-times.bin`. The presenter header ties the performance counter to UTC, which places
every loader record that carries `qpc=` on the same clock as the UTC files. No extra trace
records are needed: the phases come from records the play loader already writes.

Play is the moment the host starts the Big Picture launch: `steam_launch_run_id` (milliseconds
since 1970) in the host's `probe-report.txt` (Device Portal `LocalState\\probe-report.txt`, saved
into the run folder or given with `--probe-report`), or a `play.utc` file. An autoplay package
signs in to Steam and opens Big Picture first, about 27 s on build 1.0.63028.0, so `launch.utc`
is not Play. Without either source, Play is estimated as 1.05 s before the stage folder was
created (the one measured sample, run r720) and `play_source` says so.

Phases, in seconds from `launch.utc`:

- `stage_created`: the TempState stage folder's creation time, when a stage inventory
  (`{name: {"created": FILETIME}}`, as Device Portal lists it) is given.
- `game_log_start`: the game log's first line. The log is in console local time; the offset is
  taken as the whole quarter hour closest to the launch.
- `first_present`: the first frame the presenter showed.
- `menu`: the first `lobby-save-state=0x00000001` record (the main menu is ready), at the
  closest earlier record that carries a counter value.
- `continue`: `continue.utc`, written after the last menu input.
  Without a presenter trace, a `menu.utc` file is used and `phase_sources` says so.
- `gameplay`: the first second after `continue` that starts five seconds with at least 55
  frames each and no frame interval over 100 ms, after the world-load gaps (else `gameplay.utc`).

Compile summaries count the `pso-slow` (a title compile of at least 30 ms, with its wait for a
compile slot) and `pso-replay-compile` records in each phase, and the render thread's waits.
Gameplay compile stalls are title compiles of at least 100 ms (wait plus compile) after
`gameplay`, split into the render thread's (each one holds a frame) and worker threads' (which
stall a frame only when the frame waits for that pipeline).

`menu_to_continue` is operator time (a script's waits or a player's menu inputs), so the report
gives `play_to_gameplay_unattended_s`: Play to menu plus Continue to gameplay, compared with
`--budget`.
"""

import argparse
from bisect import bisect_left
from collections import Counter
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import struct

HEADER = struct.Struct('<IIQQQ')
PRESENT = struct.Struct('<QQII')
FILETIME_EPOCH = 116444736000000000
QPC = re.compile(r' qpc=([0-9]{1,20})')
FIELD = re.compile(r'([a-z][a-z-]*)=([0-9A-Fa-fx]+)')
GAME_LINE = re.compile(r'^([0-9]{2}):([0-9]{2}):([0-9]{2}):([0-9]{3}) ')
WARMUP = re.compile(r'File \S+ contains ([0-9]+) pipelines')


def read_utc(path):
    try:
        text = path.read_text().strip()
    except OSError:
        return None
    return datetime.fromisoformat(text.replace('Z', '+00:00')).timestamp()


class Clock:
    """Maps loader QPC values to UTC seconds through the presenter trace header."""

    def __init__(self, presents):
        magic, version, self.frequency, filetime, self.anchor = HEADER.unpack_from(presents)
        if magic != 0x31444846 or not self.frequency:
            raise ValueError('Unsupported presentation trace.')
        self.epoch = (filetime - FILETIME_EPOCH) / 10_000_000
        self.frames = [row[0] for row in PRESENT.iter_unpack(presents[HEADER.size:len(presents) - (len(presents) - HEADER.size) % PRESENT.size])]

    def utc(self, qpc):
        return self.epoch + (qpc - self.anchor) / self.frequency


def game_log_start(text, launch):
    """UTC of the game log's first line, from console local time and the launch time."""
    for line in text.splitlines():
        match = GAME_LINE.match(line)
        if not match:
            continue
        hours, minutes, seconds, millis = map(int, match.groups())
        local = hours * 3600 + minutes * 60 + seconds + millis / 1000
        day = launch - launch % 86400
        difference = (local - launch % 86400 + 43200) % 86400 - 43200
        offset = round(difference / 900) * 900
        return day + launch % 86400 + difference - offset
    return None


def records(trace):
    """Yields (line, qpc or None, last qpc seen at or before the line)."""
    last = None
    for line in trace.splitlines():
        line = line.removeprefix('steam_shadow_loader=')
        match = QPC.search(line)
        qpc = int(match[1]) if match else None
        if qpc is not None:
            last = qpc
        yield line, qpc, last


def fields(line):
    return {key: value for key, value in FIELD.findall(line)}


def gameplay_start(frames, frequency, start, window=5, minimum=55, longest_ms=100):
    """First present at or after start that begins `window` steady seconds after the load gaps."""
    after = frames[bisect_left(frames, start):]
    if len(after) < 2:
        return None
    longest = longest_ms * frequency / 1000
    index = next((i + 1 for i in range(len(after) - 1) if after[i + 1] - after[i] >= frequency // 4), 0)
    while index < len(after):
        begin = after[index]
        end = begin + window * frequency
        if after[-1] < end:
            return None
        last = bisect_left(after, end, index)
        steady = all(after[k + 1] - after[k] <= longest for k in range(index, last - 1)) and all(
            bisect_left(after, begin + (s + 1) * frequency, index, last) -
            bisect_left(after, begin + s * frequency, index, last) >= minimum for s in range(window))
        if steady:
            return begin
        # Restart after the next long interval, or a second later when frames are only sparse.
        gap = next((k + 1 for k in range(index, last - 1) if after[k + 1] - after[k] > longest), None)
        index = gap if gap is not None and gap > index else bisect_left(after, begin + frequency, index + 1)
    return None


def stage_created(inventory, launch):
    created = sorted((entry['created'] - FILETIME_EPOCH) / 10_000_000 for name, entry in inventory.items()
                     if name.startswith('SteamWin32Trial') and isinstance(entry, dict) and 'created' in entry)
    return next((when for when in created if when >= launch), None)


def analyze(folder, inventory=None, probe_report=None):
    folder = Path(folder)
    launch = read_utc(folder / 'launch.utc')
    resume = read_utc(folder / 'continue.utc')
    trace_path = next((folder / name for name in ('trace.txt', 'trace-live.txt') if (folder / name).exists()), None)
    trace = trace_path.read_text(errors='replace') if trace_path else ''
    clock = None
    if (folder / 'fhd-present-times.bin').exists():
        try:
            clock = Clock((folder / 'fhd-present-times.bin').read_bytes())
        except (ValueError, struct.error):
            clock = None
    result = {'run': folder.name, 'launch_utc': datetime.fromtimestamp(launch, timezone.utc).isoformat() if launch else None}

    def rel(utc):
        return round(utc - launch, 1) if utc is not None and launch is not None else None

    def at(qpc):
        return rel(clock.utc(qpc)) if clock and qpc is not None else None

    play, play_source = None, None
    report_path = probe_report or folder / 'probe-report.txt'
    if report_path.exists():
        match = re.search(r'^steam_launch_run_id=([0-9]{10,16})\r?$', report_path.read_text(errors='replace'), re.M)
        if match:
            play, play_source = int(match[1]) / 1000, 'probe-report'
    if play is None and (folder / 'play.utc').exists():
        play, play_source = read_utc(folder / 'play.utc'), 'play.utc'
    log = (folder / 'game.log').read_text(errors='replace') if (folder / 'game.log').exists() else ''
    warmup = WARMUP.findall(log)
    phases = {
        'stage_created': rel(stage_created(inventory, launch)) if inventory and launch else None,
        'game_log_start': rel(game_log_start(log, launch)) if log and launch else None,
        'first_present': at(clock.frames[0]) if clock and clock.frames else None,
        'menu': None, 'continue': rel(resume), 'gameplay': None,
    }
    menu_qpc = render_thread = None
    config = {}
    slot_summary = None
    for line, qpc, last in records(trace):
        if menu_qpc is None and 'lobby-save-state=0x00000001' in line:
            menu_qpc = last
        elif render_thread is None and line.startswith('asset live-thread-render-state=0x'):
            render_thread = '0x' + line.split('=0x', 1)[1][4:8]
        elif line.startswith('asset pso-parallel'):
            config = {key: int(value) for key, value in fields(line).items() if value.isdigit()}
        elif line.startswith('asset pso-slots '):
            slot_summary = {key: int(value) for key, value in fields(line).items() if value.isdigit()}
    phases['menu'] = at(menu_qpc)
    sources = {'menu': 'trace' if phases['menu'] is not None else None, 'gameplay': None}
    if phases['menu'] is None and (folder / 'menu.utc').exists():
        phases['menu'], sources['menu'] = rel(read_utc(folder / 'menu.utc')), 'menu.utc'
    if play is None and phases['stage_created'] is not None:
        play, play_source = launch + phases['stage_created'] - 1.05, 'estimated-from-stage'
    phases = {'play': rel(play), **phases}
    gameplay_qpc = None
    if clock and resume is not None:
        start = clock.anchor + round((resume - clock.epoch) * clock.frequency)
        gameplay_qpc = gameplay_start(clock.frames, clock.frequency, start)
        phases['gameplay'] = at(gameplay_qpc)
        sources['gameplay'] = 'presents' if phases['gameplay'] is not None else None
    if phases['gameplay'] is None and (folder / 'gameplay.utc').exists():
        phases['gameplay'], sources['gameplay'] = rel(read_utc(folder / 'gameplay.utc')), 'gameplay.utc'

    def bucket(qpc):
        if menu_qpc is None or qpc <= menu_qpc:
            return 'boot'
        if gameplay_qpc is None or qpc <= gameplay_qpc:
            return 'menu_to_gameplay'
        return 'gameplay'

    compiles = {name: Counter() for name in ('boot', 'menu_to_gameplay', 'gameplay')}
    first_qpc = last_qpc = None
    hitches = []
    for line, qpc, _ in records(trace):
        if qpc is None:
            continue
        first_qpc = qpc if first_qpc is None else min(first_qpc, qpc)
        last_qpc = qpc if last_qpc is None else max(last_qpc, qpc)
        if line.startswith('asset pso-slow '):
            f = fields(line)
            wait, compile_ms = int(f.get('wait-ms', 0)), int(f.get('compile-ms', 0))
            phase = compiles[bucket(qpc)]
            phase['title_compiles'] += 1
            phase['title_compile_ms'] += compile_ms
            phase['title_wait_ms'] += wait
            if f.get('tid') == render_thread:
                phase['render_thread_compiles'] += 1
                phase['render_thread_wait_ms'] += wait
                phase['render_thread_compile_ms'] += compile_ms
            if bucket(qpc) == 'gameplay' and wait + compile_ms >= 100:
                hitches.append((f.get('tid') == render_thread, wait + compile_ms))
        elif line.startswith('asset pso-replay-compile '):
            phase = compiles[bucket(qpc)]
            phase['replay_compiles'] += 1
            phase['replay_compile_ms'] += int(fields(line).get('ms', 0))
    intervals = {}
    ordered = [('launch', 0.0)] + [(name, value) for name, value in phases.items() if value is not None]
    for (a, x), (b, y) in zip(ordered, ordered[1:]):
        intervals[f'{a}_to_{b}'] = round(y - x, 1)
    gameplay_minutes = (clock.utc(last_qpc) - clock.utc(gameplay_qpc)) / 60 if clock and gameplay_qpc and last_qpc and last_qpc > gameplay_qpc else None
    result.update({
        'phases_s': phases,
        'intervals_s': intervals,
        'game_warmup_pipelines': int(warmup[-1]) if warmup else None,
        'pso_config': config or None,
        'pso_slots_summary': slot_summary,
        'render_thread': render_thread,
        'compiles': {name: dict(counter) for name, counter in compiles.items()},
        'gameplay_compile_stalls': {
            'render_thread': [ms for render, ms in hitches if render],
            'worker_count': sum(1 for render, _ in hitches if not render),
            'worker_worst_ms': max((ms for render, ms in hitches if not render), default=None),
            'render_thread_per_minute': round(sum(render for render, _ in hitches) / gameplay_minutes, 2)
            if gameplay_minutes else None,
            'traced_minutes': round(gameplay_minutes, 1) if gameplay_minutes else None,
        },
        'play_source': play_source,
        'phase_sources': sources,
        'play_to_menu_s': round(phases['menu'] - phases['play'], 1) if None not in (phases['menu'], phases['play']) else None,
        'continue_to_gameplay_s': round(phases['gameplay'] - phases['continue'], 1)
        if None not in (phases['gameplay'], phases['continue']) else None,
    })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('runs', nargs='+', type=Path, help='run folders')
    parser.add_argument('--stage-inventory', type=Path, help='JSON of TempState stage folders with FILETIME "created"')
    parser.add_argument('--probe-report', type=Path, help="the host's probe-report.txt for this run (else RUN/probe-report.txt)")
    parser.add_argument('--budget', type=float, default=90, help='launch-to-gameplay budget in seconds (default 90)')
    args = parser.parse_args()
    inventory = json.loads(args.stage_inventory.read_text()) if args.stage_inventory else None
    for run in args.runs:
        try:
            report = analyze(run, inventory, args.probe_report)
        except (OSError, ValueError, struct.error) as error:
            parser.error(f'{run}: {error}')
        menu, resume = report['play_to_menu_s'], report['continue_to_gameplay_s']
        # Unattended: Play to menu plus Continue to gameplay, without the menu inputs.
        report['play_to_gameplay_unattended_s'] = round(menu + resume, 1) if None not in (menu, resume) else None
        report['within_budget'] = report['play_to_gameplay_unattended_s'] is not None and \
            report['play_to_gameplay_unattended_s'] <= args.budget
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
