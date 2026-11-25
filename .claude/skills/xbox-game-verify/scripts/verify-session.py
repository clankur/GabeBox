#!/usr/bin/env python3
"""Run a scripted Xbox gameplay verification session and report what was observed.

The session walks the phases in references/gameplay-plan.md. For each phase it drives
the game with tools.xbox_remote_input, samples the hosted input mailbox and the GPU
mailbox while that input is in flight, records the UTC window, and collects the run's
artifacts at the end. It then measures the settled gameplay windows and writes
verification.json, a coverage-shaped trial.json, and report.md.

Delivery is never assumed: a phase counts for controller input only when the host
published a change and the game-side adapter acknowledged it. Checks whose evidence
this build cannot produce are reported as unobserved, never as passes.

    verify-session.py --plan
    verify-session.py --version 1.0.232.60 --app-id 1145360 --image-name Hades.exe \
        --env-file ../xbox/.env --output verify-run
    verify-session.py ... --input-script wander=scripts/hades-wander.txt

Uses only the standard library, as AGENTS.md asks of core commands.
"""

from __future__ import annotations

import argparse
import datetime
import json
import shlex
import subprocess
import sys
from pathlib import Path

SETTLE_SECONDS = 8
FPS_DURATION = 30
SAMPLE_CAP = 30          # tools.xbox_deploy inspect-input --seconds maximum
TARGET_FPS = 59.5        # a settled gameplay window should average about 60 FPS
HITCH_MS = 1000 / 30
STEP_GAP = 0.6           # tools.xbox_remote_input default gap between steps

# Default input cycles. They are deliberately generic: override any phase with
# --input-script PHASE=FILE once you know the title's own verbs and menus.
CYCLES = {
    'title': (['A', 'wait:2', 'B', 'wait:1'], 5),
    'world': (['A', 'wait:3', 'A', 'wait:3'], 8),
    'wander': (['hold:LS_UP:2', 'hold:LS_RIGHT:2', 'hold:LS_DOWN:2', 'hold:LS_LEFT:2',
                'hold:RS_RIGHT:1', 'hold:RS_LEFT:1'], 13),
    'interact': (['A', 'wait:1', 'X', 'wait:1', 'Y', 'wait:1', 'B', 'wait:1',
                  'RB', 'wait:1', 'LB', 'wait:1'], 12),
    'systems': (['MENU', 'wait:2', 'DOWN', 'DOWN', 'A', 'wait:2', 'B', 'wait:1', 'B', 'wait:1'], 11),
    'keyboard': (['key:T', 'key:E', 'key:S', 'key:T', 'wait:2'], 5),
    'audio': (['A', 'wait:2', 'B', 'wait:2'], 6),
    'save': (['MENU', 'wait:2', 'A', 'wait:3', 'B', 'wait:1'], 9),
    'reload': (['A', 'wait:3', 'A', 'wait:3'], 9),
    'return': (['MENU', 'wait:2', 'B', 'wait:1', 'B', 'wait:2'], 8),
}

# name, prompt, play seconds, sampled seconds, samplers, measured window,
# operator question (asked only when a person is driving or watching).
PHASES = (
    ('launch', 'Launch the title: the observer waits for the Play request.',
     0, 0, (), False, None),
    ('title', 'Reach the title screen and press one button; watch for the UI response.',
     8, 6, ('input', 'render'), False, 'Did the title screen appear and respond to that button?'),
    ('world', 'Start or continue a game until the player actually controls a character or board.',
     14, 10, ('input', 'render'), False, 'Did you reach a state where the game is being played?'),
    ('wander', 'Traverse: two distinct areas, both sticks, rotate the camera, cross a load '
     'boundary if the game has one.',
     42, 30, ('input', 'render'), True, 'Did the character or view move as directed, throughout?'),
    ('interact', 'Use the primary verbs at least five times: open, pick up, talk, attack, build, '
     'play a card.',
     42, 30, ('input', 'render'), True, 'Did each interaction land, with no input dropped?'),
    ('systems', 'Open the inventory, map or skill screen, change one setting, close it, resume play.',
     22, 20, ('input', 'render'), False, 'Did the menus open, accept the change, and hand control back?'),
    ('keyboard', 'Where the title takes text, type into a focused field.',
     14, 12, ('input',), False, 'Did the typed characters appear in the game field?'),
    ('audio', 'Trigger a named sound: a menu confirm, a footstep, a weapon.',
     12, 10, ('input',), False, 'Was the sound audible through the TV? Name it in the note.'),
    ('save', 'Save the way the game saves. Record the slot and the state concretely.',
     12, 10, ('input', 'render'), False, 'Did the game report the save completing?'),
    ('reload', 'Leave to the menu, reload that save, and compare the state.',
     14, 10, ('input', 'render'), False, 'Did the reloaded state match what you saved?'),
    ('return', 'Exit to the library the way a player would.',
     10, 6, ('input',), False, 'Did the game exit, the app stay open, and Big Picture respond?'),
)
SMOKE = ('launch', 'world', 'wander', 'return')

# Coverage counts a title on these five checks (tools/steam_compatibility.py CHECKS).
COVERAGE_CHECKS = ('gameplay', 'controller', 'audio', 'save-reload', 'return-to-library')
CHECKS = ('gameplay_visible', 'controller', 'keyboard', 'audio', 'fps_average',
          'save_reload', 'return_to_library')


def repo_root(start: Path) -> Path:
    for folder in (start, *start.parents):
        if (folder / 'tools' / 'xbox_deploy.py').exists():
            return folder
    raise SystemExit('Run this from the repository, or pass --repo.')


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def fill_steps(cycle: list[str], cycle_seconds: float, play_seconds: int) -> list[str]:
    """Repeat an input cycle until it covers the phase's play window."""
    span = cycle_seconds + STEP_GAP * len(cycle)
    repeats = max(1, round(play_seconds / span)) if span else 1
    return cycle * repeats


class Session:
    def __init__(self, args):
        self.args = args
        self.root = args.repo or repo_root(Path.cwd())
        self.output = Path(args.output).resolve() if args.output else None
        self.run_id = str(args.run_id) if args.run_id else None
        self.scripts = dict(entry.split('=', 1) for entry in args.input_script)
        self.commands: list[str] = []
        self.records: list[dict] = []

    # -- command plumbing -----------------------------------------------------

    def deploy(self, *arguments: str) -> list[str]:
        # --env-file is a top-level flag: it has to precede the subcommand.
        base = ['python3', '-m', 'tools.xbox_deploy']
        if self.args.env_file:
            base += ['--env-file', self.args.env_file]
        return base + list(arguments)

    def show(self, command: list[str]) -> str:
        printable = ' '.join(shlex.quote(part) for part in command)
        self.commands.append(printable)
        print(f'  $ {printable}', flush=True)
        return printable

    def run(self, command: list[str], capture: Path | None = None, stream: bool = False) -> dict:
        printable = self.show(command)
        if self.args.dry_run:
            return {'command': printable, 'status': 'dry-run'}
        finished = subprocess.run(command, cwd=self.root, text=True,
                                  stdout=subprocess.PIPE,
                                  stderr=None if stream else subprocess.PIPE)
        record = {'command': printable, 'returncode': finished.returncode}
        if finished.returncode and finished.stderr:
            # A failed check is data: keep the reason and continue the session.
            record['stderr'] = finished.stderr.strip()[-2000:]
            print(f'    (exit {finished.returncode}) {finished.stderr.strip()[-300:]}', flush=True)
        if capture is not None and (finished.stdout or '').strip():
            capture.write_text(finished.stdout, encoding='utf-8')
            record['stdout_file'] = str(capture)
        try:
            record['result'] = json.loads(finished.stdout)
        except (ValueError, TypeError):
            pass
        return record

    def start_driver(self, name: str, play_seconds: int, phase_dir: Path | None) -> tuple:
        """Launch remote input for this phase, concurrent with the samplers."""
        if not self.args.drive or name not in CYCLES or not play_seconds:
            return None, None
        command = ['python3', '-m', 'tools.xbox_remote_input']
        if self.args.env_file:
            command += ['--env-file', self.args.env_file]
        if name in self.scripts:
            command += ['--script', self.scripts[name]]
            steps: list[str] = []
        else:
            cycle, seconds = CYCLES[name]
            steps = fill_steps(list(cycle), seconds, play_seconds)
            # One screenshot at the start of the phase: it costs the game about half a
            # second, so it stays outside the measured window, which begins at +8 s.
            if phase_dir is not None and name in ('title', 'world', 'wander', 'interact', 'reload'):
                steps = [f'shot:{phase_dir}/{name}.png'] + steps
        command += steps
        printable = self.show(command + ([] if steps else []))
        if self.args.dry_run:
            return None, {'command': printable, 'status': 'dry-run', 'steps': steps}
        process = subprocess.Popen(command, cwd=self.root, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return process, {'command': printable, 'steps': steps}

    def finish_driver(self, process, record: dict | None) -> dict | None:
        if process is None or record is None:
            return record
        try:
            out, err = process.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate()
            record['status'] = 'timeout'
        record['returncode'] = process.returncode
        if process.returncode and err:
            record['stderr'] = err.strip()[-1000:]
            print(f'    (remote input exit {process.returncode}) {err.strip()[-300:]}', flush=True)
        return record

    def ask(self, question: str) -> dict:
        if self.args.non_interactive:
            return {'answer': 'unobserved', 'note': 'unattended session: nobody watched the TV'}
        reply = input(f'  {question} [y/n/skip] ').strip().lower()
        answer = {'y': 'yes', 'yes': 'yes', 'n': 'no', 'no': 'no'}.get(reply, 'unobserved')
        note = input('  Note (what happened on screen): ').strip()
        return {'answer': answer, 'note': note}

    # -- phases ---------------------------------------------------------------

    def folder(self, name: str) -> Path | None:
        return None if self.output is None else self.output / name

    def phase_launch(self, record: dict) -> None:
        command = self.deploy('observe', '--version', self.args.version,
                              '--app-id', self.args.app_id,
                              '--timeout', str(self.args.launch_timeout),
                              '--frame-seconds', '20')
        if self.args.image_name:
            command += ['--image-name', self.args.image_name]
        if self.run_id:
            command += ['--run-id', self.run_id]
        target = self.folder('phase-launch')
        if target:
            command += ['--output', str(target), '--bootstrap-frame']
        for checkpoint in self.args.require_checkpoint:
            command += ['--require-checkpoint', checkpoint]
        if self.args.require_checkpoint:
            # The CLI rejects a failure record without a required sequence.
            for checkpoint in self.args.fail_on_checkpoint:
                command += ['--fail-on-checkpoint', checkpoint]
        elif self.args.fail_on_checkpoint:
            print('  Ignoring --fail-on-checkpoint: it needs at least one --require-checkpoint.')
        step = self.run(command)
        record['steps'].append(step)
        result = step.get('result') or {}
        found = result.get('run_id') or result.get('steam_launch_run_id')
        self.run_id = self.run_id or (str(found) if found else None)
        if not self.run_id and not self.args.dry_run:
            print('  No run ID yet. Pass --run-id from `report --version` to sample this launch.')

    def phase_play(self, name: str, play: int, sample: int, samplers: tuple, record: dict) -> None:
        if not self.run_id and not self.args.dry_run:
            record['skipped'] = 'no run id for this launch'
            return
        run_id = self.run_id or 'RUN_ID'
        phase_dir = self.folder(f'phase-{name}-shots')
        if phase_dir is not None and not self.args.dry_run:
            phase_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        process, driver = self.start_driver(name, play, phase_dir)
        if 'input' in samplers:
            command = self.deploy('inspect-input', '--version', self.args.version,
                                  '--app-id', self.args.app_id, '--run-id', run_id,
                                  '--seconds', str(min(sample, SAMPLE_CAP)))
            if name in ('title', 'systems', 'return'):
                command += ['--panel-menu']
            target = self.folder(f'phase-{name}-input')
            if target:
                command += ['--output', str(target)]
            # Leave stderr attached so `sampling-host-input` reaches the operator live.
            record['steps'].append(self.run(command, stream=True))
        if 'render' in samplers:
            command = self.deploy('inspect-render', '--version', self.args.version,
                                  '--app-id', self.args.app_id, '--run-id', run_id)
            target = self.folder(f'phase-{name}-render')
            if target:
                command += ['--output', str(target)]
            record['steps'].append(self.run(command))
        driver = self.finish_driver(process, driver)
        if driver:
            record['input_driver'] = driver

    def collect(self) -> dict:
        record = {'phase': 'collect', 'started_utc': utc_now(), 'steps': []}
        run_id = self.run_id or 'RUN_ID'
        target = self.folder('collect')
        command = self.deploy('observe', '--version', self.args.version,
                              '--app-id', self.args.app_id, '--run-id', run_id, '--timeout', '90')
        if target:
            command += ['--output', str(target)]
        record['steps'].append(self.run(command))
        loader = self.folder('collect-loader')
        command = self.deploy('inspect-loader', '--version', self.args.version,
                              '--app-id', self.args.app_id, '--run-id', run_id)
        if loader:
            command += ['--output', str(loader)]
        # Only a staged Win32 trial reports a verified local copy; elsewhere this errors
        # and the exit code simply stays unobserved.
        record['steps'].append(self.run(command))
        sdl = None if self.output is None else self.output / 'sdl-startup.txt'
        record['steps'].append(self.run(
            self.deploy('report', '--version', self.args.version, '--sdl-startup'), capture=sdl))
        record['ended_utc'] = utc_now()
        return record

    def measure(self, windows: list[dict]) -> list[dict]:
        trace = None if self.output is None else self.output / 'collect' / 'fhd-present-times.bin'
        measured = []
        for window in windows:
            if trace is None:
                measured.append({'phase': window['phase'], 'state': 'unobserved',
                                 'reason': 'no --output directory, so no trace was collected'})
                continue
            if not trace.exists() and not self.args.dry_run:
                measured.append({'phase': window['phase'], 'state': 'unobserved',
                                 'reason': 'the run produced no native frame trace'})
                continue
            # No --require-gpu-60 here: the strict gate is read from the JSON instead,
            # so a window below it still returns its numbers rather than a bare exit 1.
            command = ['python3', '-m', 'tools.measure', str(trace),
                       '--start-utc', window['measure_from'],
                       '--duration', str(window['duration']), '--diagnose-stalls']
            if self.args.internal_resolution:
                command += ['--internal-resolution', self.args.internal_resolution]
            if self.args.output_resolution:
                command += ['--output-resolution', self.args.output_resolution]
            measured.append({'phase': window['phase'], 'state': 'measured', **self.run(command)})
        return measured


def phase_window(record: dict, settle: int, play: int, requested: int) -> dict:
    """A window inside the phase that was actually played, after the scene settles."""
    start = datetime.datetime.strptime(record['started_utc'], '%Y-%m-%dT%H:%M:%SZ')
    duration = max(1, min(requested, play - settle))
    return {'phase': record['phase'], 'duration': duration,
            'measure_from': (start + datetime.timedelta(seconds=settle)).strftime('%Y-%m-%dT%H:%M:%SZ')}


def input_evidence(record: dict) -> dict:
    summary = {'activity': None, 'acknowledged': False, 'unacknowledged': False,
               'connected': False, 'keyboard_ready': False, 'driven': 'input_driver' in record}
    for step in record.get('steps', ()):
        result = step.get('result') or {}
        if result.get('mode') != 'input':
            continue
        summary['activity'] = (result.get('activity') or {}).get('gamepad')
        for sample in result.get('samples', ()):
            pad = sample.get('gamepad') or {}
            summary['acknowledged'] |= pad.get('state') == 'acknowledged'
            summary['unacknowledged'] |= pad.get('state') == 'unacknowledged'
            summary['connected'] |= bool(pad.get('connected'))
            summary['keyboard_ready'] |= bool(sample.get('keyboard_ready'))
    return summary


def phase_screenshots(record: dict, output: Path | None) -> list[str]:
    """Screenshots this phase requested through remote input, if they landed."""
    if output is None:
        return []
    folder = output / f"phase-{record['phase']}-shots"
    return sorted(path.name for path in folder.glob('*.png')) if folder.is_dir() else []


def verdicts(records: list[dict], measured: list[dict], sdl_trace: str, output: Path | None) -> dict:
    by_name = {record['phase']: record for record in records}
    answers = {name: (record.get('operator') or {}).get('answer') for name, record in by_name.items()}
    out = {}

    play = [input_evidence(by_name[name]) for name in ('wander', 'interact') if name in by_name]
    if any(sample['activity'] == 'host-activity' and sample['acknowledged'] for sample in play):
        out['controller'] = ('pass', 'the host published input changes during gameplay and the '
                                     'game-side adapter acknowledged them')
    elif any(sample['activity'] == 'host-activity' and sample['unacknowledged'] for sample in play):
        out['controller'] = ('fail', 'the host published input the game never acknowledged')
    elif any(sample['activity'] == 'insufficient-samples' for sample in play):
        out['controller'] = ('unobserved', 'too few stable samples to tell delivery from idleness')
    else:
        out['controller'] = ('unobserved', 'no published input change was sampled during gameplay')

    keyboard = input_evidence(by_name['keyboard']) if 'keyboard' in by_name else None
    if keyboard and keyboard['keyboard_ready'] and answers.get('keyboard') == 'yes':
        out['keyboard'] = ('pass', 'the game enabled the key queue and the typed text appeared')
    elif answers.get('keyboard') == 'no':
        out['keyboard'] = ('fail', 'the typed text did not reach the game')
    elif keyboard and keyboard['keyboard_ready']:
        out['keyboard'] = ('unobserved', 'the game enabled the key queue, but no one confirmed the '
                                         'characters appeared')
    elif keyboard:
        out['keyboard'] = ('unobserved', 'the game never enabled the key queue')
    else:
        out['keyboard'] = ('unobserved', 'no keyboard phase evidence')

    shots = [f"{phase}/{name}" for phase in ('wander', 'interact', 'world') if phase in by_name
             for name in phase_screenshots(by_name[phase], output)]
    if shots and answers.get('wander') == 'yes':
        out['gameplay_visible'] = ('pass', f'screenshots taken during gameplay ({", ".join(shots)}) '
                                           'and the operator confirmed the scene responded')
    elif shots:
        out['gameplay_visible'] = ('unobserved', f'screenshots exist ({", ".join(shots)}) but nobody '
                                                 'confirmed they show the live scene — open them and say')
    elif answers.get('wander') == 'yes' or answers.get('interact') == 'yes':
        out['gameplay_visible'] = ('pass', 'operator observed the gameplay scene; no capture saved')
    elif answers.get('wander') == 'no':
        out['gameplay_visible'] = ('fail', 'the operator did not see the scene respond')
    else:
        out['gameplay_visible'] = ('unobserved', 'no gameplay capture and no operator confirmation')

    sampled = [entry for entry in measured if (entry.get('result') or {}).get('fps') is not None]
    if sampled:
        # Every settled gameplay window has to hold the average, so the weakest decides.
        worst = min(sampled, key=lambda entry: entry['result'].get('fps', 0))
        result = worst['result']
        detail = (f"{len(sampled)} window(s) measured; weakest was {worst['phase']} at "
                  f"{result.get('fps')} fps average "
                  f"(lowest rolling second {result.get('min_rolling_1s_fps')}, "
                  f"worst interval {result.get('worst_ms')} ms)")
        out['fps_average'] = ('pass' if result.get('fps', 0) >= TARGET_FPS else 'fail', detail)
    else:
        out['fps_average'] = ('unobserved', 'no measurable frame trace for a settled gameplay window')

    device_ready = 'sdl_audio_init=ready' in sdl_trace or 'sdl_audio_device=ready' in sdl_trace
    if answers.get('audio') == 'yes':
        out['audio'] = ('pass', 'operator confirmed a named sound' +
                        (' and SDL reported the device ready' if device_ready else ''))
    elif answers.get('audio') == 'no':
        out['audio'] = ('fail', 'the expected sound was not audible')
    elif device_ready:
        out['audio'] = ('unobserved', 'an audio device opened, but output was never confirmed; '
                                      'this project measures no audio signal')
    else:
        out['audio'] = ('unobserved', 'no audio evidence: no device checkpoint and nobody listening')

    if answers.get('save') == 'yes' and answers.get('reload') == 'yes':
        out['save_reload'] = ('pass', 'a save was written and its state matched after reload')
    elif 'no' in (answers.get('save'), answers.get('reload')):
        out['save_reload'] = ('fail', 'the save did not complete or the reloaded state did not match')
    else:
        out['save_reload'] = ('unobserved', 'save and reload were not both confirmed')

    if answers.get('return') == 'yes':
        out['return_to_library'] = ('pass', 'the game exited and Big Picture stayed responsive')
    elif answers.get('return') == 'no':
        out['return_to_library'] = ('fail', 'the exit path did not return to a responsive library')
    else:
        out['return_to_library'] = ('unobserved', 'the return to the library was not confirmed')

    return {name: {'verdict': out[name][0], 'evidence': out[name][1]} for name in CHECKS if name in out}


def pacing_issues(measured: list[dict]) -> list[dict]:
    """Hitches and transport problems worth reporting even when the average is fine."""
    issues = []
    for entry in measured:
        result = entry.get('result') or {}
        if result.get('fps') is None:
            continue
        phase = entry['phase']
        worst = result.get('worst_ms')
        long_frames = result.get('intervals_over_33_33ms') or 0
        rolling = result.get('min_rolling_1s_fps')
        if worst is not None and worst > HITCH_MS:
            issues.append({'phase': phase, 'kind': 'hitch',
                           'detail': f'longest frame interval {worst} ms '
                                     f'({long_frames} intervals over {HITCH_MS:.2f} ms)'})
        if isinstance(rolling, int) and rolling < 57:
            issues.append({'phase': phase, 'kind': 'rolling-second',
                           'detail': f'lowest rolling one-second count {rolling} frames'})
        if result.get('cpu_presented'):
            issues.append({'phase': phase, 'kind': 'cpu-upload',
                           'detail': f"{result['cpu_presented']} frames went through a CPU upload "
                                     'rather than a shared GPU texture'})
        if result.get('hardware_adapter_verified') is not True:
            issues.append({'phase': phase, 'kind': 'adapter',
                           'detail': 'the trace does not verify the hardware adapter path'})
        if result.get('gpu_60fps') is False and (result.get('fps') or 0) >= TARGET_FPS:
            issues.append({'phase': phase, 'kind': 'strict-gate',
                           'detail': 'average meets the target but gpu_60fps does not pass, so this '
                                     'window cannot count toward coverage --require-gpu-60'})
    return issues


def coverage_trial(payload: dict) -> dict:
    """A trial record in the shape tools.steam_compatibility coverage --evidence reads."""
    checks = payload['checks']
    gameplay = checks.get('gameplay_visible', {}).get('verdict') == 'pass'
    best = max((entry.get('result') or {} for entry in payload['measured']),
               key=lambda result: result.get('fps', 0), default={})
    return {
        'id': payload['app_id'],
        'build_id': payload['build_id'] or '',
        'target': payload['target'] or '',
        'evidence': payload['evidence_reference'] or '',
        'checks': {
            'gameplay': gameplay,
            'controller': checks.get('controller', {}).get('verdict') == 'pass',
            'audio': checks.get('audio', {}).get('verdict') == 'pass',
            'save-reload': checks.get('save_reload', {}).get('verdict') == 'pass',
            'return-to-library': checks.get('return_to_library', {}).get('verdict') == 'pass',
        },
        'performance_scene': 'gameplay' if best else '',
        'performance': best,
    }


def write_report(output: Path, payload: dict) -> Path:
    trial = payload['trial']
    countable = all(trial['checks'][name] for name in COVERAGE_CHECKS)
    lines = [f"# Verification report: AppID {payload['app_id']}", '',
             f"- Package version: `{payload['version']}`",
             f"- Steam AppID: `{payload['app_id']}`" + (f" · image `{payload['image_name']}`"
                                                        if payload['image_name'] else ''),
             f"- Steam build: `{payload['build_id'] or 'not recorded'}` · "
             f"target: `{payload['target'] or 'not recorded'}`",
             f"- Launch run ID: `{payload['run_id'] or 'unresolved'}`",
             f"- Session: {payload['started_utc']} to {payload['ended_utc']}"
             f"{' (input driven by tools.xbox_remote_input)' if payload['driven'] else ''}",
             '', '## Checks', '', '| Check | Verdict | Evidence |', '| --- | --- | --- |']
    for name, entry in payload['checks'].items():
        lines.append(f"| {name.replace('_', ' ')} | **{entry['verdict']}** | {entry['evidence']} |")
    lines += ['', f"Coverage: this run {'satisfies' if countable else 'does not satisfy'} the five "
                  'checks `tools.steam_compatibility coverage` counts '
                  f"({', '.join(COVERAGE_CHECKS)}). Counting also needs the strict "
                  '`--require-gpu-60` gate at native 1920x1080.', '',
              '## Measured windows', '',
              f'Target: a settled gameplay window averages about 60 FPS (>= {TARGET_FPS}), and the '
              'weakest window decides. Hitches are listed as issues, not folded into the average.', '']
    if payload['measured']:
        lines += ['| Phase | Average FPS | Lowest rolling second | Worst interval | Long frames | Transport |',
                  '| --- | ---: | ---: | ---: | ---: | --- |']
        for entry in payload['measured']:
            result = entry.get('result') or {}
            cpu = result.get('cpu_presented')
            transport = ('shared GPU texture' if cpu == 0 and result.get('shared_texture_presented')
                         else f'{cpu} CPU uploads' if cpu else entry.get('state', 'unobserved'))
            lines.append(f"| {entry['phase']} | {result.get('fps', '—')} | "
                         f"{result.get('min_rolling_1s_fps', '—')} | "
                         f"{result.get('worst_ms', '—')} ms | "
                         f"{result.get('intervals_over_33_33ms', '—')} | {transport} |")
    else:
        lines.append('No window was measured.')
    lines += ['', '## Issues', '']
    lines += ([f"- **{issue['kind']}** in `{issue['phase']}`: {issue['detail']}"
               for issue in payload['issues']] or ['- None recorded in the measured windows.'])
    lines += ['', '## Phase notes', '']
    for record in payload['phases']:
        operator = record.get('operator') or {}
        note = operator.get('note') or ''
        driver = record.get('input_driver') or {}
        sent = f" · sent {len(driver.get('steps', []))} input steps" if driver else ''
        lines.append(f"- **{record['phase']}** ({record['started_utc']} → {record.get('ended_utc', '—')})"
                     f"{sent}: {operator.get('answer', 'n/a')}{' — ' + note if note else ''}")
    unobserved = [name for name, entry in payload['checks'].items() if entry['verdict'] == 'unobserved']
    lines += ['', '## Still unobserved', '']
    lines += ([f"- {name.replace('_', ' ')}: {payload['checks'][name]['evidence']}" for name in unobserved]
              or ['- Nothing: every check reached a verdict.'])
    lines += ['', '## Commands run', '', '```sh', *payload['commands'], '```', '']
    path = output / 'report.md'
    path.write_text('\n'.join(lines), encoding='utf-8')
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description='Run a scripted Xbox gameplay verification session.')
    parser.add_argument('--version', help='installed Steam package version')
    parser.add_argument('--app-id', help='Steam AppID under test')
    parser.add_argument('--build-id', help='Steam build ID, recorded in the coverage trial')
    parser.add_argument('--target', help='console and runtime configuration, for the coverage trial')
    parser.add_argument('--evidence-reference', help='issue or run URL backing this trial')
    parser.add_argument('--image-name', help='game executable basename, for process observation')
    parser.add_argument('--run-id', help='existing steam_launch_run_id, to attach to a running game')
    parser.add_argument('--env-file', help='Device Portal .env from another workspace')
    parser.add_argument('--repo', type=Path, help='repository root (default: discovered)')
    parser.add_argument('--output', help='new directory for artifacts, verification.json and report.md')
    parser.add_argument('--phase-set', choices=('deep', 'smoke'), default='deep')
    parser.add_argument('--skip', action='append', default=[], metavar='PHASE')
    parser.add_argument('--input-script', action='append', default=[], metavar='PHASE=FILE',
                        help='drive this phase from a tools.xbox_remote_input script file')
    parser.add_argument('--no-drive', dest='drive', action='store_false',
                        help='do not send input; a person plays each phase instead')
    parser.add_argument('--require-checkpoint', action='append', default=[], metavar='TRACE')
    parser.add_argument('--fail-on-checkpoint', action='append', default=[], metavar='TRACE')
    parser.add_argument('--internal-resolution', help='observed game render resolution, WIDTHxHEIGHT')
    parser.add_argument('--output-resolution', help='observed Xbox display output, WIDTHxHEIGHT')
    parser.add_argument('--fps-duration', type=int, default=FPS_DURATION)
    parser.add_argument('--settle-seconds', type=int, default=SETTLE_SECONDS)
    parser.add_argument('--launch-timeout', type=int, default=180)
    parser.add_argument('--non-interactive', action='store_true',
                        help='nobody is watching: checks that need a human become unobserved')
    parser.add_argument('--dry-run', action='store_true', help='print the commands without running them')
    parser.add_argument('--plan', action='store_true', help='print the phase plan and exit')
    args = parser.parse_args()

    selected = [phase for phase in PHASES
                if (args.phase_set == 'deep' or phase[0] in SMOKE) and phase[0] not in args.skip]

    if args.plan:
        print(f'Phase plan ({args.phase_set}):\n')
        for name, prompt, play, sample, samplers, measurable, question in selected:
            print(f'  {name:9s} play {play:3d}s  sample {sample:3d}s  '
                  f'{", ".join(samplers) or "none":14s}{"  [frame window]" if measurable else ""}')
            print(f'            {prompt}')
            if name in CYCLES:
                print(f'            default input: {" ".join(CYCLES[name][0])}')
            if question:
                print(f'            ask: {question}')
        print('\nInput is driven with tools.xbox_remote_input unless --no-drive is given.')
        print('Override any phase with --input-script PHASE=FILE once you know the title.')
        return 0

    if not (args.version and args.app_id):
        parser.error('--version and --app-id are required unless you pass --plan')
    if not args.drive and args.non_interactive:
        parser.error('--no-drive needs a person: do not combine it with --non-interactive')

    session = Session(args)
    if session.output is not None and not args.dry_run:
        session.output.mkdir(mode=0o700, parents=True, exist_ok=True)

    started = utc_now()
    print(f'Verification session for AppID {args.app_id} on package {args.version} '
          f'({"dry run" if args.dry_run else started}).')
    print('This drives the console. Make sure no other session or player is using it.\n')

    windows = []
    for name, prompt, play, sample, samplers, measurable, question in selected:
        print(f'[{name}] {prompt}')
        record = {'phase': name, 'prompt': prompt, 'steps': []}
        if not args.drive and not args.non_interactive and name != 'launch':
            input('  Play this phase while the sampler runs. Press Enter when ready. ')
        record['started_utc'] = utc_now()
        if name == 'launch':
            session.phase_launch(record)
        else:
            session.phase_play(name, play, sample, samplers, record)
        record['ended_utc'] = utc_now()
        if question:
            record['operator'] = session.ask(question)
        if measurable:
            windows.append(phase_window(record, args.settle_seconds, play, args.fps_duration))
        session.records.append(record)
        print()

    print('[collect] Collecting run artifacts and the frame trace.')
    session.records.append(session.collect())
    measured = session.measure(windows)

    sdl_path = None if session.output is None else session.output / 'sdl-startup.txt'
    sdl_trace = sdl_path.read_text(encoding='utf-8') if sdl_path and sdl_path.exists() else ''

    payload = {
        'version': args.version, 'app_id': args.app_id, 'build_id': args.build_id,
        'target': args.target, 'evidence_reference': args.evidence_reference,
        'image_name': args.image_name, 'run_id': session.run_id, 'started_utc': started,
        'ended_utc': utc_now(), 'phase_set': args.phase_set, 'driven': args.drive,
        'interactive': not args.non_interactive, 'phases': session.records, 'measured': measured,
        'checks': verdicts(session.records, measured, sdl_trace, session.output),
        'issues': pacing_issues(measured), 'commands': session.commands,
    }
    payload['trial'] = coverage_trial(payload)
    if args.dry_run:
        print('\nDry run: no artifacts written. Commands above are what the session would run.')
        return 0

    if session.output is not None:
        (session.output / 'verification.json').write_text(json.dumps(payload, indent=2) + '\n',
                                                          encoding='utf-8')
        (session.output / 'trial.json').write_text(json.dumps([payload['trial']], indent=2) + '\n',
                                                   encoding='utf-8')
        print(f"\nWrote {write_report(session.output, payload)}, verification.json and trial.json")
    for name, entry in payload['checks'].items():
        print(f"  {name:20s} {entry['verdict']}")
    for issue in payload['issues']:
        print(f"  issue: {issue['kind']} in {issue['phase']} — {issue['detail']}")
    return 1 if any(entry['verdict'] == 'fail' for entry in payload['checks'].values()) else 0


if __name__ == '__main__':
    sys.exit(main())
