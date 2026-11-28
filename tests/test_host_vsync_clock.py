import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class HostVsyncClockTests(unittest.TestCase):
    """Builds and runs tests/host-vsync-clock.c against runtime/shared/host-vsync-clock.h."""

    def test_paced_30fps_lands_on_every_second_host_refresh(self):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'host-vsync-clock'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/host-vsync-clock.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_adapter_paces_multi_refresh_presents_to_the_host(self):
        frame = (ROOT / 'runtime/win32/dxgi-frame.cpp').read_text()
        commit = frame[frame.index('void DxgiFramePublisher::Commit'):frame.index('bool DxgiFramePublisher::HostVsync')]
        # The release time is taken from the done fence of the sequence just published.
        self.assertLess(commit.index('InterlockedExchange64(&gpu_->sequence[index]'),
                        commit.index('SetEventOnCompletion(slot.submitted, hostRelease.events[index])'))
        adapter = (ROOT / 'runtime/win32/dxgi.cpp').read_text()
        pacing = adapter[adapter.index('UINT NativeInterval'):adapter.index('HRESULT Capture')]
        self.assertIn('interval >= 2 && DxgiFramePublisher::HostVsync(now, host)', pacing)
        # Without a lock the nominal clock still paces, so a host that never releases cannot stall Present.
        self.assertLess(pacing.index('DxgiFramePublisher::HostVsync'), pacing.index('XboxSdlNextSwap(&clock_'))


if __name__ == '__main__':
    unittest.main()
