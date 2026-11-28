import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SmallArrayCopyTests(unittest.TestCase):
    """The Xbox small array-subresource copy workaround (runtime/win32/d3d12-small-array-copy.h)."""

    def setUp(self):
        self.header = (ROOT / 'runtime/win32/d3d12-small-array-copy.h').read_text()

    def test_alias_formats_match_block_sizes(self):
        # 16-byte blocks alias as R32G32B32A32_UINT, 8-byte blocks as R32G32_UINT, at block resolution.
        self.assertIn('alias.Format = block == 16 ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;', self.header)
        self.assertIn('alias.Width = (desc->Width + 3) / 4;', self.header)
        self.assertIn('alias.Height = (desc->Height + 3) / 4;', self.header)

    def test_copy_queue_copies_replay_before_the_titles_next_signal(self):
        execute = self.header[self.header.index('static void STDMETHODCALLTYPE XboxSacExecuteHook'):]
        execute = execute[:execute.index('\n}\n')]
        order = ['title->signal(queue, xboxSac.copyDone', 'xboxSac.queue->lpVtbl->Wait(xboxSac.queue, xboxSac.copyDone',
                 'own->execute(xboxSac.queue', 'own->signal(xboxSac.queue, xboxSac.replayDone',
                 'queue->lpVtbl->Wait(queue, xboxSac.replayDone']
        positions = [execute.index(step) for step in order]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(execute.index('->execute(queue, count, lists)'), positions[0])

    def test_play_loader_installs_it(self):
        trace = (ROOT / 'analysis/spider-man-play-loader/trace.c').read_text()
        allocs = (ROOT / 'analysis/spider-man-play-loader/allocs.h').read_text()
        self.assertLess(trace.index('#include "d3d12-small-array-copy.h"'), trace.index('#include "allocs.h"'))
        self.assertIn('XboxSacInstall((ID3D12Device *)startupDevice, (XboxSacPlacedFn)allocOriginal[29])', trace)
        self.assertIn('XboxSacNotePlaced(heap, offset, desc, (ID3D12Resource *)*out);', allocs)


if __name__ == '__main__':
    unittest.main()
