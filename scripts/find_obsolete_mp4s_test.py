#!/usr/bin/env python3
"""Filesystem regression tests for optional Plex publication."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from find_obsolete_mp4s import symlink_plex_programs


class PlexLinksTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.root = self.home / 'Videos'
        self.root.mkdir()
        self.plex = self.home / 'Plex'
        self.plex.mkdir()

    def video(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def test_latest_generation_then_4k_and_shallow_scan(self):
        self.video('sharks/tracking_output-2.mp4')
        chosen = self.video('sharks/program_4k_output-2.mp4')
        self.video('sharks/stitched_output-99.mp4')
        self.video('sharks/tracking_output-100.mkv')
        self.video('season/chicago/tracking_output-5.mp4')
        self.video('tracking_output-1.mp4')
        symlink_plex_programs(self.root, self.plex)
        self.assertEqual(list(self.plex.iterdir()), [self.plex / 'sharks.mp4'])
        self.assertEqual((self.plex / 'sharks.mp4').resolve(), chosen)
        newer = self.video('sharks/tracking_output-10.mp4')
        symlink_plex_programs(self.root, self.plex)
        self.assertEqual((self.plex / 'sharks.mp4').resolve(), newer)

    def test_remove_every_matching_link_including_dangling_and_relative(self):
        chosen = self.video('game/game-program-4k_output-with-audio-3.mp4')
        (self.plex / 'arbitrary-name.avi').symlink_to(chosen)
        (self.plex / 'dangling').symlink_to('../Videos/game/removed.mp4')
        (self.plex / 'nested').symlink_to('../Videos/game/nested/removed.mp4')
        (self.plex / 'directory').symlink_to(chosen.parent)
        (self.plex / 'unrelated').symlink_to('../Videos/game-other/old.mp4')
        (self.plex / 'regular').write_text('keep')
        symlink_plex_programs(self.root, self.plex)
        self.assertEqual({p.name for p in self.plex.iterdir()}, {'game.mp4', 'unrelated', 'regular'})
        self.assertEqual((self.plex / 'game.mp4').resolve(), chosen)

    def test_collision_preserves_files_and_old_links(self):
        chosen = self.video('game/tracking_output.mp4')
        old = self.plex / 'old'
        old.symlink_to(chosen)
        collision = self.plex / 'game.mp4'
        for kind in ('regular', 'unrelated_link', 'directory'):
            with self.subTest(kind=kind):
                if kind == 'regular':
                    collision.write_text('keep')
                elif kind == 'unrelated_link':
                    collision.symlink_to('/missing/unrelated.mp4')
                else:
                    collision.mkdir()
                symlink_plex_programs(self.root, self.plex)
                self.assertTrue(old.is_symlink())
                if kind == 'regular':
                    self.assertEqual(collision.read_text(), 'keep')
                elif kind == 'unrelated_link':
                    self.assertEqual(collision.readlink(), Path('/missing/unrelated.mp4'))
                if kind == 'directory':
                    collision.rmdir()
                else:
                    collision.unlink()

    def test_cli_opt_in_default_override_and_missing_directory(self):
        chosen = self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        def run(*args):
            return subprocess.run([sys.executable, str(script), str(self.root), *args],
                                  env={**os.environ, 'HOME': str(self.home)},
                                  check=True, capture_output=True, text=True)
        self.assertEqual(run().stdout, str(obsolete.relative_to(self.root)) + '\n')
        self.assertEqual(list(self.plex.iterdir()), [])
        self.assertEqual(run('--symlink-plex').stdout, str(obsolete.relative_to(self.root)) + '\n')
        self.assertEqual((self.plex / 'game.mp4').resolve(), chosen)
        other = self.home / 'Other'
        other.mkdir()
        run('--symlink-plex', '--plex-dir', '~/Other')
        self.assertEqual((other / 'game.mp4').resolve(), chosen)
        missing = self.home / 'missing'
        self.assertIn('Skipping Plex links', run('--symlink-plex', '--plex-dir', str(missing)).stderr)
        self.assertFalse(missing.exists())
        missing.touch()
        self.assertIn('Skipping Plex links', run('--symlink-plex', '--plex-dir', str(missing)).stderr)


if __name__ == '__main__':
    unittest.main()
