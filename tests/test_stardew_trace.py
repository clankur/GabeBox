from pathlib import Path
import runpy
import unittest


summary = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'xbox-deploy.py'))['stardew_host_summary']


class StardewTraceTests(unittest.TestCase):
    def test_no_trace_or_unrelated_dotnet_process(self):
        self.assertIn('no-output', summary(None))
        self.assertIn('target-unconfirmed', summary(b'--- Invoked apphost: another.exe'))

    def test_host_error_preserves_code_and_redacts_paths(self):
        trace = "--- Invoked apphost\n/private/user/Stardew Valley.exe\n" \
                "Failed to load the dll from [/private/user/hostfxr.dll], HRESULT: 0x8007007E\n"
        for encoding in ('utf-8-sig', 'utf-16'):
            report = summary(trace.encode(encoding))
            self.assertIn('stardew_host_phase=apphost', report)
            self.assertIn('stardew_host_error=hostfxr.dll', report)
            self.assertIn('stardew_host_hresult=0x8007007E', report)
            self.assertNotIn('/private/user', report)

    def test_handoff_and_exit(self):
        trace = b'--- Invoked hostpolicy\nCoreCLR path = ...\nLaunch host: x app: Stardew Valley.dll\n' \
                b'Execute managed assembly exit code: 0x1\n'
        report = summary(trace)
        self.assertIn('stardew_host_phase=managed-launch', report)
        self.assertIn('stardew_host_exit=0x1', report)
