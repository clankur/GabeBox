import runpy
import unittest
from pathlib import Path


summary = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'xbox-deploy.py'))['slay_console_summary']


class SlayConsoleTests(unittest.TestCase):
    def test_prelogger_failure_reports_type_and_preserves_private_output(self):
        raw = (b'slay_console=helper-started\nslay_console_jar=readable\nError: Unable to access jarfile /private/file.jar\n'
               b'java.lang.UnsatisfiedLinkError: path to private files\nslay_console=exit-1\n')
        result = summary(raw)
        self.assertIn('slay_console_jar=readable', result)
        self.assertIn('slay_console_observed=jar-unavailable', result)
        self.assertIn('slay_logged_exception=java.lang.UnsatisfiedLinkError', result)
        self.assertIn('slay_console=exit-1', result)
        self.assertNotIn('private', result)

    def test_absent_and_unmarked_log(self):
        self.assertEqual(summary(None), 'slay_console=no-output')
        self.assertEqual(summary(b'unrelated'), 'slay_console=unrecognized-output')
