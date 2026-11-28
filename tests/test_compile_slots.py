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

    def test_replay_claims_an_entry_only_after_admission(self):
        replay = (ROOT / 'runtime/win32/d3d12-pipeline-replay.h').read_text()
        thread = replay[replay.index('static DWORD WINAPI XprReplayThread'):replay.index('static BOOL XprPatch')]
        enter = thread.index('xprState.slot(TRUE)')
        claim = thread.index('entry->state = XprCompiling')
        self.assertLess(enter, claim)
        self.assertEqual(thread.count('xprState.slot(FALSE)'), 2)


if __name__ == '__main__':
    unittest.main()
