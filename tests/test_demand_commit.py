import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COPIES = ('runtime/win32/demand-commit.h', 'analysis/spider-man-play-loader/demand-commit.h')


def function(text, signature):
    body = text[text.index(signature):]
    return body[:body.index('\n}\n')]


class DemandCommitTests(unittest.TestCase):
    """Demand commit (runtime/win32/demand-commit.h) and the play loader's copy of it."""

    def test_copies_differ_only_in_the_play_loader_fault_counter(self):
        runtime, play = ((ROOT / path).read_text() for path in COPIES)
        extra = {'extern volatile LONG perfFaults;', '    if (committed) InterlockedIncrement(&perfFaults);'}
        self.assertEqual([line for line in play.splitlines() if line not in extra], runtime.splitlines())

    def test_fault_handler_retries_only_resolved_faults(self):
        for path in COPIES:
            with self.subTest(path=path):
                fault = function((ROOT / path).read_text(), 'static LONG WINAPI XdcFault(')
                self.assertIn('if (access == 8) return EXCEPTION_CONTINUE_SEARCH;', fault)
                self.assertIn('XdcCommit(range, address, 1) && XdcAllows(address, access)', fault)

    def test_recommit_keeps_live_ranges(self):
        for path in COPIES:
            with self.subTest(path=path):
                take = function((ROOT / path).read_text(), 'static LPVOID XboxDemandCommitTake(')
                self.assertNotIn('XdcDropLocked(', take)
                self.assertIn('if (live->allocation != allocation)', take)
                self.assertIn('if (live->base <= from && to <= live->end)', take)
                self.assertIn('range->allocation = allocation;', take)


if __name__ == '__main__':
    unittest.main()
