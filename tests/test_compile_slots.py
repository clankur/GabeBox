import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CompileSlotPolicyTests(unittest.TestCase):
    """Builds and runs tests/compile-slots.c against runtime/shared/compile-slots.h."""

    def test_policy(self):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'compile-slots'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/compile-slots.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_play_loader_uses_the_shared_pool(self):
        source = (ROOT / 'analysis/spider-man-play-loader/trace.c').read_text()
        self.assertIn('#include "d3d12-compile-slots.h"', source)
        self.assertNotIn('CreateSemaphoreExW', source)
        self.assertIn('XboxPipelineReplaySetSlot(ReplaySlot)', source)
        build = (ROOT / 'analysis/spider-man-play-loader/build.sh').read_text()
        self.assertIn('play) mode_flags="-DPSO_SLOTS=4 -DPSO_BOOST_SLOTS=2 -DPSO_PRIORITY=1', build)

    def test_load_time_options_default_off(self):
        source = (ROOT / 'analysis/spider-man-play-loader/trace.c').read_text()
        for define, default in (('PSO_REPLAY_THREADS', 1), ('PSO_BG_RESERVE', 0), ('PSO_SKIP_WARMUP', 0)):
            self.assertIn(f'#ifndef {define}\n#define {define} {default}\n#endif', source)
        self.assertIn('XboxPipelineReplaySetThreads(PSO_REPLAY_THREADS)', source)
        self.assertIn('XboxCompileSlotsReserveBackground(PSO_BG_RESERVE)', source)
        hook = source[source.index('HANDLE WINAPI XboxTraceCreateFileW'):]
        self.assertLess(hook.index('#if PSO_SKIP_WARMUP'), hook.index('WarmupRouteOpen'))
        warmup = (ROOT / 'analysis/spider-man-play-loader/pso-warmup.h').read_text()
        # Only read-only opens are hidden; writes go to the session file, never the player's list.
        self.assertIn('if (*creation == OPEN_EXISTING && !(access & writes)) return TRUE;', warmup)
        self.assertIn('L".session"', warmup)

    def test_replay_threads_share_one_summary(self):
        replay = (ROOT / 'runtime/win32/d3d12-pipeline-replay.h').read_text()
        thread = replay[replay.index('static DWORD WINAPI XprReplayThread'):replay.index('static BOOL XprPatch')]
        self.assertLess(thread.index('InterlockedDecrement(&xprState.threadsLive) > 0'),
                        thread.index('asset pso-replay done'))
        self.assertIn('xprState.threadCount = threads < 1 ? 1 : threads > 16 ? 16 : threads;', replay)
        # A thread reserves an entry before waiting for a slot, and a rootless entry takes no slot.
        reserve = thread.index('InterlockedCompareExchange(&entry->reserved, 1, 0)')
        rooted = thread.index('if (!rooted)')
        self.assertLess(reserve, rooted)
        self.assertLess(rooted, thread.index('xprState.slot(TRUE)'))

    def test_replay_claims_an_entry_only_after_admission(self):
        replay = (ROOT / 'runtime/win32/d3d12-pipeline-replay.h').read_text()
        thread = replay[replay.index('static DWORD WINAPI XprReplayThread'):replay.index('static BOOL XprPatch')]
        enter = thread.index('xprState.slot(TRUE)')
        claim = thread.index('entry->state = XprCompiling')
        self.assertLess(enter, claim)
        self.assertEqual(thread.count('xprState.slot(FALSE)'), 2)


if __name__ == '__main__':
    unittest.main()
