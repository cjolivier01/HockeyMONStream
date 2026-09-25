import os
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from benchmark_player_analytics import inspect_log, process_ids, stop
from profile_player_analytics import analyze, run_profile


class HarnessTest(unittest.TestCase):
    def test_cleanup_after_wrapper_already_exited(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / 'worker.pid'
            child = ('import os,signal,time; from pathlib import Path; '
                     'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                     f'Path({str(pid_file)!r}).write_text(str(os.getpid())); time.sleep(60)')
            parent = ('import subprocess,sys,time; from pathlib import Path; '
                      f'subprocess.Popen([sys.executable,"-c",{child!r}]); '
                      f'p=Path({str(pid_file)!r});\n'
                      'while not p.exists(): time.sleep(.01)')
            process = subprocess.Popen([sys.executable, '-c', parent], start_new_session=True)
            try:
                process.wait(timeout=5)
                stop(process)
                pid = int(pid_file.read_text())
                for _ in range(20):
                    try:
                        state = Path(f'/proc/{pid}/stat').read_text().rpartition(') ')[2].split()[0]
                    except FileNotFoundError:
                        break
                    if state == 'Z':
                        break
                    time.sleep(.05)
                else:
                    self.fail('Worker survived cleanup of its already-exited wrapper')
            finally:
                try:
                    os.killpg(process.pid, 9)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)

    def test_process_tree_without_kernel_children_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for pid, parent in [(100, 1), (101, 100), (102, 101), (103, 1)]:
                (root / str(pid)).mkdir()
                (root / str(pid) / 'stat').write_text(f'{pid} (worker (name)) S {parent} 0 0\n')
            (root / '104').mkdir()  # Raced/exited process with missing stat.
            self.assertEqual(process_ids(100, root), {100, 101, 102})

    def test_profiler_timeout_stops_worker_after_wrapper_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / 'worker.pid'
            child = ('import os,signal,time; from pathlib import Path; '
                     'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                     f'Path({str(pid_file)!r}).write_text(str(os.getpid())); time.sleep(60)')
            parent = ('import subprocess,sys,time; '
                      f'subprocess.Popen([sys.executable,"-c",{child!r}]); time.sleep(60)')
            with (Path(directory) / 'log').open('w') as log:
                with self.assertRaises(subprocess.TimeoutExpired):
                    run_profile([sys.executable, '-c', parent], directory, dict(os.environ), log, 1)
            self.assertTrue(pid_file.exists())
            pid = int(pid_file.read_text())
            for _ in range(20):
                try:
                    state = Path(f'/proc/{pid}/stat').read_text().rpartition(') ')[2].split()[0]
                except FileNotFoundError:
                    break
                if state == 'Z':
                    break
                time.sleep(.05)
            else:
                self.fail('Profiler worker survived its process-group timeout')

    def test_success_and_overlay_suppression(self):
        case = {'expect_analytics': True, 'expect_overlay': True,
                'minimum_analytics_counters': {'pose-enqueues': 1},
                'maximum_overlay_counters': {'suppressed': 0},
                'required_log_patterns': ['App run successful'], 'detector_engine': '/tmp/pinned.engine'}
        log = ('deserialized trt engine from :/tmp/pinned.engine\nApp run successful\n'
               'HSTREAM_PLAYER_ANALYTICS frames=10 pose-enqueues=2\n'
               'HSTREAM_PLAYER_OVERLAY renders=10 launches=10 upload-bytes=1024 suppressed=0\n')
        self.assertEqual(inspect_log(log, case)[3], [])
        self.assertTrue(inspect_log(log.replace('suppressed=0', 'suppressed=1'), case)[3])
        self.assertTrue(inspect_log(log.replace('pinned.engine', 'different.engine'), case)[3])
        self.assertTrue(inspect_log(log.replace('launches=10', 'launches=0'), case)[3])
        self.assertTrue(inspect_log(log.replace('pose-enqueues=2', 'pose-enqueues=0'), case)[3])
        self.assertEqual(inspect_log(log.replace('launches=10', 'launches=8'), case)[3], [])

    def test_expected_disabled_and_diagnostic_not_counter(self):
        case = {'expect_analytics': False, 'expect_overlay': False}
        self.assertEqual(inspect_log('', case)[3], [])
        self.assertTrue(inspect_log('HSTREAM_PLAYER_ANALYTICS frames=1\n', case)[3])
        self.assertEqual(inspect_log('HSTREAM_PLAYER_OVERLAY status=suppressed reason=1\n', case)[1], [])

    def test_compact_transfer_attribution_and_oversize_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / 'sample.sqlite'
            connection = sqlite3.connect(database)
            connection.executescript('''
                CREATE TABLE StringIds(id INTEGER,value TEXT);
                CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL(start INTEGER,end INTEGER,deviceId INTEGER,
                    contextId INTEGER,streamId INTEGER,demangledName INTEGER);
                CREATE TABLE ENUM_CUDA_MEMCPY_OPER(id INTEGER,label TEXT);
                CREATE TABLE CUPTI_ACTIVITY_KIND_MEMCPY(start INTEGER,end INTEGER,deviceId INTEGER,
                    contextId INTEGER,streamId INTEGER,bytes INTEGER,copyKind INTEGER,correlationId INTEGER);
                INSERT INTO ENUM_CUDA_MEMCPY_OPER VALUES(2,'Device-to-Host');
                INSERT INTO StringIds VALUES(1,'hm::player_analytics::DecodeKernel()');
                INSERT INTO StringIds VALUES(2,'hm::player_analytics::JerseyDecode()');
                INSERT INTO StringIds VALUES(3,'hm::player_analytics::ActionReduce()');
                INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES(10,20,0,1,9,1),(30,40,0,1,9,2),(50,60,0,1,9,3);
                INSERT INTO CUPTI_ACTIVITY_KIND_MEMCPY VALUES(21,22,0,1,9,1632,2,1),
                    (41,42,0,1,9,64,2,2),(61,62,0,1,9,8,2,3),(12,16,0,1,99,5000000,2,4);
            ''')
            connection.commit()
            report = analyze(database)
            self.assertEqual(report['failures'], [])
            self.assertEqual(report['compact_d2h_counts'], {'pose': 1, 'jersey': 1, 'action': 1})
            self.assertEqual(report['compact_d2h_max_bytes'], 1632)
            self.assertTrue(analyze(database, expect_overlay=True)['failures'])
            connection.execute("INSERT INTO StringIds VALUES(4,'hm::draw_display::analytics::detail::Draw<false>()')")
            connection.execute('INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES(70,80,0,1,10,4)')
            connection.commit()
            self.assertEqual(analyze(database, expect_overlay=True)['failures'], [])
            connection.execute('INSERT INTO CUPTI_ACTIVITY_KIND_MEMCPY VALUES(81,82,0,1,10,8192,2,5)')
            connection.commit()
            self.assertTrue(any('Program overlay stream' in message
                                for message in analyze(database, expect_overlay=True)['failures']))
            connection.execute('DELETE FROM CUPTI_ACTIVITY_KIND_MEMCPY WHERE correlationId=5')
            connection.execute('UPDATE CUPTI_ACTIVITY_KIND_MEMCPY SET bytes=132710400 WHERE correlationId=1')
            connection.commit()
            self.assertTrue(any('outside compact' in message for message in analyze(database)['failures']))
            connection.execute('DELETE FROM CUPTI_ACTIVITY_KIND_MEMCPY WHERE correlationId=3')
            connection.commit()
            self.assertTrue(any('action' in message for message in analyze(database)['failures']))
            connection.close()


if __name__ == '__main__':
    unittest.main()
