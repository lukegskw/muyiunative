"""Ordered local disc sets: BOM/comments/relative Unicode paths, explicit order, missing files, duplicate paths, nesting and size limits. No game files."""
import shutil, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import disc_set


class DiscSet(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp()).resolve(); self.paths = [self.root / f'disc {i} é.cue' for i in range(1, 5)]
        for p in self.paths: p.write_text('synthetic layout checked later by the disc reader')
        self.playlist = self.root / 'set.m3u'

    def tearDown(self): shutil.rmtree(self.root)

    def test_ordered_playlist_and_explicit_list(self):
        self.playlist.write_text('\ufeff#EXTM3U\n\n' + '\n'.join(p.name for p in reversed(self.paths)), encoding='utf-8')
        self.assertEqual(disc_set.sources(self.playlist), list(reversed(self.paths)))
        self.assertEqual(disc_set.sources(self.paths[0], self.paths[1:]), self.paths)
        self.assertEqual(disc_set.sources(self.paths[0]), self.paths[:1])

    def test_invalid_sets_are_rejected_before_compilation(self):
        for text in ('', '# comments only', self.paths[0].name + '\n./' + self.paths[0].name, 'absent.bin', 'set.m3u', 'https://example.invalid/game.cue',
                     '\n'.join(self.paths[0].name for _ in range(disc_set.MAX_DISCS + 1))):
            with self.subTest(text=text):
                self.playlist.write_text(text)
                with self.assertRaises(ValueError): disc_set.sources(self.playlist)
        self.playlist.write_text(self.paths[0].name)
        with self.assertRaises(ValueError): disc_set.sources(self.playlist, self.paths[1:])
