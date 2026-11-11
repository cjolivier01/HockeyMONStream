#!/usr/bin/env python3
"""Filesystem regression tests for Plex publication and obsolete-export cleanup."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

from find_obsolete_mp4s import cleanup_targets, find_obsolete_mp4s, remove_obsolete, symlink_plex_programs


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

    def environment(self):
        """A terminal rich cannot be talked into dressing up by the ambient env."""
        env = {key: value for key, value in os.environ.items()
               if key not in ('FORCE_COLOR', 'TERM', 'COLORTERM', 'TTY_COMPATIBLE')}
        # A fixed width keeps the --clean table from folding its paths.
        return {**env, 'HOME': str(self.home), 'COLUMNS': '200', 'NO_COLOR': '1',
                # rich falls back to ASCII box characters without this.
                'PYTHONIOENCODING': 'utf-8'}

    def run_cli(self, *args, check=True, answers=''):
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        # A closed stdin keeps an unanswered prompt from hanging the suite.
        return subprocess.run([sys.executable, str(script), str(self.root), *args],
                              env=self.environment(),
                              input=answers, check=check, capture_output=True, text=True)

    def table_cells(self, output):
        """The --clean table's rows, without the box rich draws around them."""
        return [[cell.strip() for cell in line.split('│')[1:4]]
                for line in output.splitlines() if line.count('│') >= 4]

    def test_game_named_database_generations_and_legacy_recordings(self):
        legacy = self.video('game/hstream_telemetry-2.db')
        older = self.video('game/game_telemetry-3.db')
        current = self.video('game/game_telemetry-7.db')
        self.assertEqual(set(find_obsolete_mp4s(self.root)), {legacy, older})
        self.assertTrue(current.is_file())

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
        newer = self.video('sharks/program_8k_output-10.mp4')
        symlink_plex_programs(self.root, self.plex)
        self.assertEqual((self.plex / 'sharks.mp4').resolve(), newer)

    def test_relative_target_replaces_an_absolute_link(self):
        chosen = self.video('game/program_4k_output-1.mp4')
        link = self.plex / 'game.mp4'
        link.symlink_to(chosen)
        symlink_plex_programs(self.root, self.plex)
        target = link.readlink()
        self.assertFalse(target.is_absolute())
        self.assertEqual(target.parts[-3:], ('Videos', 'game', 'program_4k_output-1.mp4'))
        self.assertEqual(link.resolve(), chosen.resolve())

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

    def test_published_program_is_not_a_cleanup_candidate(self):
        chosen = self.video('game/program_4k_output-2.mp4')
        sidecar = self.video('game/program_4k_output-2.mp4.hstream-archive.json')
        self.video('game/stitched_output-3.mp4')
        ordinary = self.run_cli()
        self.assertIn(str(chosen.relative_to(self.root)), ordinary.stdout)
        published = self.run_cli('--symlink-plex', '--plex-dir', str(self.plex), '--clean', '--force')
        self.assertIn('removed 0 files', published.stderr)
        self.assertEqual((self.plex / 'game.mp4').resolve(), chosen)
        self.assertTrue(sidecar.is_file(), 'a published export must keep its origin')

    def test_cli_opt_in_default_override_and_missing_directory(self):
        chosen = self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        run = self.run_cli
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

    def test_an_obsolete_archive_takes_its_origin_sidecar(self):
        self.video('game/program_4k_output-2.mp4')
        kept = self.video('game/program_4k_output-2.mp4.hstream-archive.json')
        self.video('game/program_4k_output-1.mp4')
        self.video('game/program_4k_output-1.mp4.hstream-archive.json')
        # The video is listed first: an interrupt there leaves an orphan the
        # sweep catches next run, where the reverse strands a live export.
        self.assertEqual(self.run_cli().stdout.split(),
                         ['game/program_4k_output-1.mp4',
                          'game/program_4k_output-1.mp4.hstream-archive.json'])
        self.assertTrue(kept.is_file())

    def test_the_newest_telemetry_survives_a_video_only_run(self):
        # The later run published video without DriveGPT, so its generation
        # runs ahead of every database and there is no newer one to keep.
        self.video('game/game-program_8k_output-with-audio-1.mp4')
        self.video('game/game-program_8k_output-with-audio-2.mp4')
        kept = self.video('game/game_telemetry-1.db')
        self.run_cli('--clean', '--force')
        self.assertTrue(kept.is_file(), 'a game must not be left with no telemetry at all')
        older = self.video('game/game_telemetry-0.db')
        self.run_cli('--clean', '--force')
        self.assertTrue(kept.is_file() and not older.exists(), 'superseded telemetry still goes')

    def test_a_file_named_only_like_a_sidecar_is_ignored(self):
        self.video('game/tracking_output-1.mp4')
        stray = self.video('game/.hstream-archive.json')
        self.run_cli('--clean', '--force')
        self.assertTrue(stray.is_file())

    def test_a_sidecar_whose_export_is_already_gone_is_swept(self):
        self.video('game/program_4k_output-2.mp4')
        orphan = self.video('game/program_4k_output-1.mp4.hstream-archive.json')
        self.video('game/notes.json')
        self.assertEqual(self.run_cli().stdout.split(), [str(orphan.relative_to(self.root))])
        self.run_cli('--clean', '--force')
        self.assertFalse(orphan.exists())

    def test_orphans_are_listed_in_order_with_everything_else(self):
        self.video('game/program_4k_output-2.mp4')
        self.video('game/program_4k_output-1.mp4.hstream-archive.json')
        self.video('game/tracking_output-2.mp4')
        self.video('game/tracking_output-1.mp4')
        # The orphan sorts before the obsolete video, not after everything.
        self.assertEqual(self.run_cli().stdout.split(),
                         ['game/program_4k_output-1.mp4.hstream-archive.json',
                          'game/tracking_output-1.mp4'])

    def test_an_export_on_a_detached_volume_keeps_its_sidecar(self):
        self.video('game/program_4k_output-2.mp4')
        offline = self.root / 'game/program_4k_output-3.mp4'
        offline.symlink_to(self.home / 'detached-volume/program_4k_output-3.mp4')
        sidecar = self.video('game/program_4k_output-3.mp4.hstream-archive.json')
        self.assertEqual(self.run_cli('--clean', '--force').stderr.splitlines()[-1], 'removed 0 files')
        self.assertTrue(sidecar.is_file(), 'an export that is away is not an export that is gone')
        self.assertTrue(offline.is_symlink())

    def test_a_missing_plex_destination_cannot_sweep(self):
        chosen = self.video('game/program_4k_output-3.mp4')
        sidecar = self.video('game/program_4k_output-3.mp4.hstream-archive.json')
        self.video('game/stitched_output-9.mp4')
        unmounted = self.home / 'nas/Plex'
        failed = self.run_cli('--clean', '--symlink-plex', '--plex-dir', str(unmounted), check=False)
        self.assertEqual(failed.returncode, 2)
        self.assertIn('--plex-dir is not an existing directory', failed.stderr)
        self.assertTrue(chosen.is_file() and sidecar.is_file())

    def test_clean_and_dry_run_cannot_be_combined(self):
        failed = self.run_cli('--clean', '--dry-run', check=False)
        self.assertEqual(failed.returncode, 2)
        self.assertIn('not allowed with argument', failed.stderr)

    def test_a_dry_run_previews_the_published_sweep_without_publishing(self):
        self.video('game/stitched_output-9.mp4')
        chosen = self.video('game/program_4k_output-3.mp4')
        self.video('game/program_4k_output-3.mp4.hstream-archive.json')
        # write_job_scripts only looks at directories holding a game config.
        self.video('game/_hockeymon/config.yaml')
        dry = self.run_cli('--dry-run', '--symlink-plex', '--plex-dir', str(self.plex), '--write-jobs')
        self.assertEqual(list(self.plex.iterdir()), [], 'a dry run must not rewrite the library')
        self.assertNotIn('job script', dry.stderr, 'a dry run must not write job scripts')
        self.assertNotIn('hstream-job', dry.stderr, 'a dry run must not run hstream-job')
        # What --clean --symlink-plex would really do, not what it would do
        # without the sparing that publication performs.
        self.assertIn('would remove 0 files', dry.stderr)
        self.assertTrue(chosen.is_file())

    def test_a_game_plex_could_not_publish_cannot_be_swept(self):
        chosen = self.video('game/program_4k_output-3.mp4')
        sidecar = self.video('game/program_4k_output-3.mp4.hstream-archive.json')
        self.video('game/stitched_output-9.mp4')
        # Somebody dropped a real file where this game's link belongs, so
        # publication skips the game and spares nothing.
        (self.plex / 'game.mp4').touch()
        failed = self.run_cli('--clean', '--symlink-plex', '--plex-dir', str(self.plex), check=False)
        self.assertEqual(failed.returncode, 2)
        self.assertIn('cannot sweep games that were not published: game', failed.stderr)
        self.assertTrue(chosen.is_file() and sidecar.is_file())

    def test_a_video_that_cannot_be_removed_keeps_its_sidecar(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        sidecar = self.video('game/tracking_output-1.mp4.hstream-archive.json')
        targets = cleanup_targets(self.root, set(), False)
        self.assertEqual(targets, [(obsolete, sidecar)])
        # Another process got there first, so the unlink raises and the pair
        # must not be split: the sidecar is the surviving export's origin.
        obsolete.unlink()
        self.assertEqual(remove_obsolete(targets, self.root, False, False), 1)
        self.assertTrue(sidecar.is_file(), 'a video that did not go keeps its origin')

    def test_a_plex_published_export_keeps_its_sidecar(self):
        # The stitched export carries the newest generation, so the 4K is an
        # obsolete companion by version, yet it is the only thing Plex links.
        self.video('game/stitched_output-9.mp4')
        chosen = self.video('game/program_4k_output-3.mp4')
        sidecar = self.video('game/program_4k_output-3.mp4.hstream-archive.json')
        self.run_cli('--clean', '--force', '--symlink-plex', '--plex-dir', str(self.plex))
        self.assertEqual((self.plex / 'game.mp4').resolve(), chosen)
        self.assertTrue(chosen.is_file() and sidecar.is_file())

    def test_dry_run_reports_on_stderr_what_clean_would_delete(self):
        current = self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        sidecar = self.video('game/tracking_output-1.mp4.hstream-archive.json')
        dry = self.run_cli('--dry-run')
        # Nothing on stdout: piping a dry run into rm must remove nothing.
        self.assertEqual(dry.stdout, '')
        self.assertIn('would remove game/tracking_output-1.mp4\n', dry.stderr)
        self.assertIn('would remove game/tracking_output-1.mp4.hstream-archive.json\n', dry.stderr)
        self.assertIn('would remove 2 files\n', dry.stderr)
        self.assertTrue(obsolete.is_file() and sidecar.is_file())
        cleaned = self.run_cli('--clean', '--force')
        self.assertIn('removed game/tracking_output-1.mp4\n', cleaned.stderr)
        self.assertIn('removed 2 files\n', cleaned.stderr)
        self.assertFalse(obsolete.exists() or sidecar.exists())
        self.assertTrue(current.is_file())
        self.assertIn('removed 0 files\n', self.run_cli('--clean', '--force').stderr)

    def test_a_symlink_named_like_an_older_export_is_left_alone(self):
        current = self.video('game/tracking_output-2.mp4')
        link = self.root / 'game/tracking_output-1.mp4'
        link.symlink_to(current)
        sidecar = self.video('game/tracking_output-1.mp4.hstream-archive.json')
        listed = self.run_cli()
        self.assertIn('Skipping symlink: game/tracking_output-1.mp4', listed.stderr)
        self.assertEqual(listed.stdout, '', 'a pipe into rm must not take the link either')
        self.run_cli('--clean', '--force')
        self.assertTrue(link.is_symlink() and current.is_file())
        self.assertTrue(sidecar.is_file(), 'a spared link must keep its sidecar')

    def test_a_failed_removal_is_reported_in_the_exit_code(self):
        self.video('game/tracking_output-2.mp4')
        self.video('game/tracking_output-1.mp4')
        game = self.root / 'game'
        game.chmod(0o500)
        self.addCleanup(game.chmod, 0o700)
        failed = self.run_cli('--clean', '--force', check=False)
        self.assertEqual(failed.returncode, 1)
        self.assertIn('Could not remove game/tracking_output-1.mp4', failed.stderr)

    def test_the_clean_table_pairs_each_file_with_what_supersedes_it(self):
        self.video('game/tracking_output-2.mp4')
        self.video('game/tracking_output-1.mp4')
        self.video('game/tracking_output-1.mp4.hstream-archive.json')
        self.video('game/program_4k_output-2.mp4')
        self.video('game/program_4k_output-1.mp4.hstream-archive.json')
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr), [
            ['1', 'game/program_4k_output-1.mp4.hstream-archive.json', '(export already gone)'],
            ['2', 'game/tracking_output-1.mp4', 'game/tracking_output-2.mp4'],
            ['3', 'game/tracking_output-1.mp4.hstream-archive.json',
             '(origin of game/tracking_output-1.mp4)'],
        ])

    def test_each_file_is_tabled_against_its_own_kind_before_the_generation(self):
        self.video('game/hstream_telemetry-7.db')
        self.video('game/tracking-7.csv')
        self.video('game/tracking-2.csv')
        self.video('game/camera-2.csv')
        # Nothing carries camera.csv forward, and naming whichever file holds
        # the generation would read as the readings having moved in there.
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr), [
            ['1', 'game/camera-2.csv', '(nothing of its kind survives)'],
            ['2', 'game/tracking-2.csv', 'game/tracking-7.csv'],
        ])

    def test_a_game_name_is_not_rich_markup(self):
        # Brackets are a style tag and an unbalanced pair aborts the render;
        # colons are an emoji. All three are legal in a directory name.
        for game in ('sharks [away]', 'ducks [/] sharks', 'a:vs:b'):
            self.video(f'{game}/tracking_output-2.mp4')
            self.video(f'{game}/tracking_output-1.mp4')
        rows = self.table_cells(self.run_cli('--clean').stderr)
        self.assertEqual([cells[1] for cells in rows],
                         ['a:vs:b/tracking_output-1.mp4',
                          'ducks [/] sharks/tracking_output-1.mp4',
                          'sharks [away]/tracking_output-1.mp4'])

    def test_the_column_never_names_a_file_the_same_sweep_takes(self):
        # The 4K is retired by its own newer generation, which the companion
        # rule then retires in favour of the stitched export.
        self.video('game/program_4k_output-1.mp4')
        self.video('game/program_4k_output-3.mp4')
        self.video('game/stitched_output-9.mp4')
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr), [
            ['1', 'game/program_4k_output-1.mp4', 'game/stitched_output-9.mp4'],
            ['2', 'game/program_4k_output-3.mp4', 'game/stitched_output-9.mp4'],
        ])

    def test_a_program_export_outranks_a_stitching_intermediate(self):
        self.video('game/program_4k_output-2.mp4')
        self.video('game/program_8k_output-9.mp4')
        self.video('game/stitched_output-9.mp4')
        # Both survivors are generation 9, and the companion rule is about
        # Program exports, so the one Plex publishes is the one to name.
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr),
                         [['1', 'game/program_4k_output-2.mp4', 'game/program_8k_output-9.mp4']])

    def test_a_config_event_is_paired_across_generations_not_sequences(self):
        self.video('game/hstream_telemetry-7.db')
        self.video('game/play_tracker_source-7-1.yaml')
        self.video('game/play_tracker_source-2-1.yaml')
        # The trailing -1 is the event sequence; the generation is the 2.
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr),
                         [['1', 'game/play_tracker_source-2-1.yaml',
                           'game/play_tracker_source-7-1.yaml']])

    def test_a_link_onto_a_doomed_export_is_not_a_successor(self):
        going = self.video('game/tracking_output-2.mp4')
        link = self.root / 'game/tracking_output-3.mp4'
        link.symlink_to(going.name)
        # Discovery came through the link, so the generation it carries is the
        # file below it. Naming the link would promise a dangling one.
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr),
                         [['1', 'game/tracking_output-2.mp4', '(nothing of its kind survives)']])

    def test_a_bare_config_artifact_pairs_across_generations(self):
        for generation in (5, 7):
            (self.root / f'game/hstream_telemetry-{generation}.json').parent.mkdir(
                parents=True, exist_ok=True)
            (self.root / f'game/hstream_telemetry-{generation}.json').write_text(json.dumps(
                {'config_provenance': {'source_artifact': f'play_tracker_source-{generation}.yaml'}}))
            self.video(f'game/play_tracker_source-{generation}.yaml')
        # The -5 suffix is this one's generation, not an event sequence, so it
        # belongs with play_tracker_source-7.yaml and not with a database.
        self.assertEqual(self.table_cells(self.run_cli('--clean').stderr), [
            ['1', 'game/hstream_telemetry-5.json', 'game/hstream_telemetry-7.json'],
            ['2', 'game/play_tracker_source-5.yaml', 'game/play_tracker_source-7.yaml'],
        ])

    def test_the_4k_program_outranks_the_tracking_export_plex_passed_over(self):
        self.video('game/program-4k_output-2.mp4')
        chosen = self.video('game/program_4k_output-9.mp4')
        self.video('game/tracking_output-9.mp4')
        cleaned = self.run_cli('--clean', '--symlink-plex', '--plex-dir', str(self.plex), answers='n\n')
        # Whatever the table credits has to be what the library ends up with.
        self.assertEqual(self.table_cells(cleaned.stderr),
                         [['1', 'game/program-4k_output-2.mp4', 'game/program_4k_output-9.mp4']])
        self.assertEqual((self.plex / 'game.mp4').resolve(), chosen)

    def test_the_answer_is_about_the_sweep_and_not_about_publication(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        refused = self.run_cli('--clean', '--quiet', '--symlink-plex', '--plex-dir', str(self.plex),
                               answers='n\n')
        self.assertIn('removed 0 files\n', refused.stderr)
        self.assertTrue(obsolete.is_file())
        # Publication was asked for by its own flag and runs either way. It
        # cannot be deferred past the question: deciding in one pass and
        # applying in a second makes the first a model of the second, and the
        # two disagree on a game whose stale link an earlier game reclaims.
        self.assertEqual([entry.name for entry in self.plex.iterdir()], ['game.mp4'])

    def test_a_stale_cross_named_link_does_not_block_the_sweep(self):
        self.video('a/tracking_output-1.mp4')
        self.video('b/program_4k_output-1.mp4')
        self.video('b/stitched_output-9.mp4')
        # Left behind by a renamed game: named for b, pointing into a.
        (self.plex / 'b.mp4').symlink_to(self.root / 'a/tracking_output-1.mp4')
        listed = self.run_cli('--symlink-plex', '--plex-dir', str(self.plex))
        # b's 4K is published, so it is not a candidate and must not be listed
        # as one by a run that is publishing it in the same breath.
        self.assertEqual(listed.stdout, '')
        self.assertEqual((self.plex / 'b.mp4').resolve(), self.root / 'b/program_4k_output-1.mp4')
        self.assertEqual((self.plex / 'a.mp4').resolve(), self.root / 'a/tracking_output-1.mp4')

    def test_an_interrupt_partway_through_the_unlinks_is_not_a_traceback(self):
        for generation in range(1, 400):
            self.video(f'game/tracking_output-{generation}.mp4')
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        with subprocess.Popen(
                [sys.executable, str(script), str(self.root), '--clean', '--quiet', '--force'],
                env=self.environment(), stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as running:
            # Interrupt once the unlinks are underway, which is the only window
            # --force ever spends time in.
            removed = ''
            while removed.count('removed ') < 2:
                character = running.stderr.read(1)
                self.assertNotEqual(character, '', f'the sweep ended too soon: {removed!r}')
                removed += character
            running.send_signal(signal.SIGINT)
            self.assertIn('Stopped partway through.', removed + running.stderr.read())
            self.assertEqual(running.wait(), 130)

    def test_force_sweeps_without_rich_but_says_it_drew_no_table(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        shadow = self.home / 'no-rich-force'
        shadow.mkdir()
        (shadow / 'rich.py').write_text('raise ImportError("no rich on this box")\n')
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        # --force waived the confirmation the table informs, so it has nothing
        # left to lose by going without one.
        forced = subprocess.run(
            [sys.executable, str(script), str(self.root), '--clean', '--force'],
            env={**self.environment(), 'PYTHONPATH': str(shadow)},
            input='', check=False, capture_output=True, text=True)
        self.assertEqual(forced.returncode, 0)
        self.assertIn('Skipping the table: no rich on this box', forced.stderr)
        self.assertIn('removed 1 file\n', forced.stderr)
        self.assertFalse(obsolete.exists())

    def test_a_box_without_rich_keeps_everything_but_the_table(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        shadow = self.home / 'no-rich'
        shadow.mkdir()
        (shadow / 'rich.py').write_text('raise ImportError("no rich on this box")\n')
        env = {**self.environment(), 'PYTHONPATH': str(shadow)}

        def run(*args, **kwargs):
            script = Path(__file__).with_name('find_obsolete_mp4s.py')
            return subprocess.run([sys.executable, str(script), str(self.root), *args],
                                  env=env, input='', check=False, capture_output=True, text=True,
                                  **kwargs)

        # Only the table needs rich, so everything that draws none still runs.
        self.assertEqual(run().stdout, 'game/tracking_output-1.mp4\n')
        self.assertIn('would remove 1 file\n', run('--dry-run').stderr)
        # Asking to confirm against a table it cannot draw is the one refusal.
        refused = run('--clean')
        self.assertEqual(refused.returncode, 2)
        self.assertIn('--clean needs rich to draw its table', refused.stderr)
        self.assertTrue(obsolete.is_file())
        self.assertIn('removed 1 file\n', run('--clean', '--quiet', '--force').stderr)
        self.assertFalse(obsolete.exists())

    def test_a_closed_stdin_stops_the_questions_instead_of_crashing(self):
        for generation in range(1, 5):
            self.video(f'game/tracking_output-{generation}.mp4')
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        # 0<&- is a closed descriptor, not the empty one /dev/null gives.
        closed = subprocess.run(
            ['/bin/sh', '-c', 'exec 0<&-; exec "$@"', 'sh',
             sys.executable, str(script), str(self.root), '--clean', '--quiet'],
            check=False, capture_output=True, text=True, env=self.environment())
        self.assertEqual(closed.returncode, 0)
        self.assertNotIn('Traceback', closed.stderr)
        self.assertIn('No answer: leaving the rest in place.', closed.stderr)
        # One dead question, not one per file.
        self.assertEqual(closed.stderr.count('[y/N]'), 1)
        self.assertIn('removed 0 files\n', closed.stderr)

    def test_an_unanswered_clean_deletes_nothing(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        # stdin is closed, and EOF is not consent.
        self.assertIn('removed 0 files\n', self.run_cli('--clean').stderr)
        self.assertTrue(obsolete.is_file())

    def test_clean_deletes_only_the_files_confirmed_one_by_one(self):
        self.video('game/stitched_output-2.mp4')
        declined = self.video('game/stitched_output-1.mp4')
        self.video('game/tracking_output-2.mp4')
        approved = self.video('game/tracking_output-1.mp4')
        sidecar = self.video('game/tracking_output-1.mp4.hstream-archive.json')
        cleaned = self.run_cli('--clean', answers='n\ny\n')
        self.assertIn('Delete game/stitched_output-1.mp4? [y/N]', cleaned.stderr)
        # One answer takes the export and the origin that cannot outlive it.
        self.assertIn('Delete game/tracking_output-1.mp4 and '
                      'game/tracking_output-1.mp4.hstream-archive.json? [y/N]', cleaned.stderr)
        self.assertIn('removed 2 files\n', cleaned.stderr)
        self.assertTrue(declined.is_file())
        self.assertFalse(approved.exists() or sidecar.exists())

    def test_all_takes_one_answer_for_the_whole_sweep(self):
        kept = self.video('game/tracking_output-2.mp4')
        self.video('game/tracking_output-1.mp4')
        self.video('game/tracking_output-1.mp4.hstream-archive.json')
        refused = self.run_cli('--clean', '--all', answers='n\n')
        self.assertIn('Delete all 2 files?', refused.stderr)
        self.assertIn('removed 0 files\n', refused.stderr)
        swept = self.run_cli('--clean', '--all', answers='y\n')
        self.assertIn('removed 2 files\n', swept.stderr)
        self.assertEqual([path.name for path in (self.root / 'game').iterdir()], [kept.name])

    def test_quiet_withholds_the_table_but_not_the_question(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        cleaned = self.run_cli('--clean', '--quiet', answers='y\n')
        self.assertNotIn('Obsolete files to delete', cleaned.stderr)
        self.assertIn('Delete game/tracking_output-1.mp4? [y/N]', cleaned.stderr)
        self.assertFalse(obsolete.exists())
        self.video('game/tracking_output-3.mp4')
        forced = self.run_cli('--clean', '-q', '--force')
        self.assertNotIn('Obsolete files to delete', forced.stderr)
        self.assertIn('removed game/tracking_output-2.mp4\n', forced.stderr)

    def test_force_prints_the_table_without_asking(self):
        self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        forced = self.run_cli('--clean', '--force')
        self.assertIn('Obsolete files to delete', forced.stderr)
        self.assertNotIn('[y/N]', forced.stderr)
        self.assertFalse(obsolete.exists())

    def test_an_interrupted_confirmation_deletes_nothing(self):
        kept = self.video('game/tracking_output-2.mp4')
        obsolete = self.video('game/tracking_output-1.mp4')
        script = Path(__file__).with_name('find_obsolete_mp4s.py')
        # Nothing is ever written to stdin, so the sweep waits at the prompt.
        with subprocess.Popen(
                [sys.executable, str(script), str(self.root), '--clean', '--quiet'],
                env=self.environment(),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True) as running:
            # Read up to the question before interrupting, rather than race it.
            asked = ''
            while '[y/N]' not in asked:
                character = running.stderr.read(1)
                self.assertNotEqual(character, '', f'the sweep ended without asking: {asked!r}')
                asked += character
            running.send_signal(signal.SIGINT)
            self.assertIn('Stopped before deleting anything.', asked + running.stderr.read())
            self.assertEqual(running.wait(), 130)
        self.assertTrue(obsolete.is_file() and kept.is_file())

    def test_the_confirmation_flags_only_apply_to_clean(self):
        for flag in ('--all', '--force', '--quiet'):
            failed = self.run_cli(flag, check=False)
            self.assertEqual(failed.returncode, 2)
            self.assertIn(f'{flag} only applies to --clean', failed.stderr)
        # A dry run only reports, so it never reaches a prompt to skip either.
        self.assertEqual(self.run_cli('--dry-run', '--force', check=False).returncode, 2)
        # One answer or none: --all has nothing left to collapse under --force.
        refused = self.run_cli('--clean', '--all', '--force', check=False)
        self.assertEqual(refused.returncode, 2)
        self.assertIn('not allowed with argument', refused.stderr)

    def test_a_mistyped_root_is_an_error_not_an_empty_sweep(self):
        self.root = self.home / 'Vidoes'
        failed = self.run_cli('--clean', check=False)
        self.assertEqual(failed.returncode, 2)
        self.assertIn('not a directory', failed.stderr)


if __name__ == '__main__':
    unittest.main()
