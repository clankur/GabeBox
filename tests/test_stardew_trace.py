import unittest

from tools.xbox_deploy import stardew_host_summary as summary
from tools.xbox_deploy import stardew_managed_summary


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

    def test_managed_output_allows_only_known_values(self):
        managed_summary = stardew_managed_summary
        self.assertEqual(managed_summary(None), 'stardew_managed=no-output')
        log = b'stardew_managed=entered\nstardew_managed_library=opengl32\n' \
              b'stardew_managed_library=/private/user/opengl32\nstardew_managed=entered\n' \
              b'stardew_managed_fault=System.TypeInitializationException\n' \
              b'stardew_managed_fault=/private/user/error\n'
        self.assertEqual(managed_summary(log), 'stardew_managed=entered\nstardew_managed_library=opengl32\n'
                         'stardew_managed_fault=System.TypeInitializationException')
