"""Kernel shutdown proof for locally supervised task descendants."""

import os
import subprocess
import sys
import textwrap
import unittest
from pathlib import Path


class ProcessesTest(unittest.TestCase):
    def isolated(self, source):
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(__file__).resolve().parent)
        result = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(source)],
            env=environment,
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exit_after_identity_check_cannot_redirect_cancellation(self):
        self.isolated("""
            import os, signal, subprocess
            from unittest.mock import patch
            from hstream_workflow import processes

            processes.become_subreaper()
            target = subprocess.Popen(['sleep', '60'])
            unrelated = subprocess.Popen(['sleep', '60'])
            original_send = processes.pidfd_send_signal
            target_pin = processes.pidfd_open(target.pid)
            unrelated_pin = processes.pidfd_open(unrelated.pid)
            sends = []
            saved = processes.process_identity(target.pid)
            def exit_before_send(descriptor, number):
                # Reap after the final identity read. Numeric PID ownership has
                # ended, but this descriptor must still refer to the dead task.
                original_send(target_pin, signal.SIGKILL)
                target.wait(timeout=5)
                sends.append(descriptor)
                original_send(descriptor, number)
            try:
                with patch.object(processes, 'descendants', return_value={target.pid: saved}), \\
                     patch.object(processes, 'pidfd_send_signal', side_effect=exit_before_send), \\
                     patch.object(os, 'kill', side_effect=AssertionError('Unpinned signal')):
                    processes.signal_descendants(signal.SIGTERM)
                assert len(sends) == 1
                assert unrelated.poll() is None, 'Unrelated work received cancellation'
                try:
                    os.fstat(sends[0])
                    raise AssertionError('Cancellation leaked its process descriptor')
                except OSError:
                    pass
            finally:
                for child, descriptor in ((target, target_pin), (unrelated, unrelated_pin)):
                    if child.poll() is None:
                        original_send(descriptor, signal.SIGKILL)
                    child.wait(timeout=5)
                    os.close(descriptor)
            """)

    def test_reused_identity_after_pin_is_not_signalled(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        saved = {"pid": 123, "parent": os.getpid(), "start_time": 1}
        with (
            patch.object(processes, "descendants", return_value={123: saved}),
            patch.object(processes, "pidfd_open", return_value=42) as pin,
            patch.object(
                processes, "process_identity", return_value=dict(saved, start_time=2)
            ),
            patch.object(processes, "pidfd_send_signal") as send,
            patch.object(processes.os, "close") as close,
        ):
            processes.signal_descendants(15)
        pin.assert_called_once_with(123)
        send.assert_not_called()
        close.assert_called_once_with(42)

    def test_child_forking_after_proc_listing_cannot_hide_its_detached_child(self):
        self.isolated(r"""
            import os
            import signal
            import subprocess
            import sys
            import tempfile
            import time
            from pathlib import Path
            from hstream_workflow import processes

            processes.become_subreaper()
            parent_code = '''
            import os, signal, subprocess, sys, time
            from pathlib import Path
            root = Path(sys.argv[1])
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            (root / 'ready').touch()
            while not (root / 'go').exists():
                time.sleep(.001)
            child = subprocess.Popen(['sleep', '60'], start_new_session=True)
            (root / 'child').write_text(str(child.pid))
            os._exit(0)
            '''
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                parent = subprocess.Popen([
                    sys.executable, '-c', __import__('textwrap').dedent(parent_code), temporary
                ])
                original = processes.process_identity
                reads = 0
                try:
                    deadline = time.monotonic() + 5
                    while not (root / 'ready').exists():
                        assert time.monotonic() < deadline, 'Parent did not start'
                        time.sleep(.001)

                    def identity(pid):
                        global reads
                        if pid == parent.pid:
                            reads += 1
                            if reads == 3:
                                # The SIGTERM scan and its identity recheck have
                                # completed. Fork after the following /proc list,
                                # then reap the parent before its stat is read.
                                (root / 'go').touch()
                                parent.wait(timeout=5)
                        return original(pid)

                    processes.process_identity = identity
                    stopped = processes.retire(.15)
                    child = int((root / 'child').read_text())
                    assert reads >= 3, 'The fork/listing race was not exercised'
                    assert stopped, 'Descendants were not retired'
                    assert original(child) is None, 'Shutdown was claimed with a live adopted child'
                    assert processes.reap(), 'The supervisor still owns a child'
                finally:
                    processes.process_identity = original
                    for row in processes.descendants().values():
                        try:
                            os.kill(row['pid'], signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    parent.wait(timeout=5)
                    deadline = time.monotonic() + 5
                    while not processes.reap():
                        assert time.monotonic() < deadline, 'Fixture cleanup failed'
                        time.sleep(.01)
            """)

    def test_empty_proc_scans_do_not_prove_a_live_child_stopped(self):
        self.isolated("""
            import os
            import signal
            import subprocess
            import time
            from hstream_workflow import processes

            processes.become_subreaper()
            child = subprocess.Popen(['sleep', '60'])
            try:
                # Model a scan that misses the child. No signal can establish
                # shutdown either: the kernel still reports a live child.
                processes.descendants = lambda **_kwargs: {}
                assert not processes.retire(.05), 'An empty scan was treated as shutdown proof'
                assert not processes.reap(), 'A live child was treated as reaped'
            finally:
                child.kill()
                child.wait(timeout=5)
            assert processes.reap()
            """)

    def test_reaping_retries_interrupted_kernel_waits(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        with patch.object(
            processes.os,
            "waitpid",
            side_effect=[InterruptedError(), (123, 0), ChildProcessError()],
        ):
            self.assertTrue(processes.reap())

    def test_retirement_bounds_interrupted_and_continuously_reapable_waits(self):
        self.isolated("""
            import time
            from unittest.mock import patch
            from hstream_workflow import processes
            for observation in (InterruptedError, (123, 0)):
                arguments = ({'side_effect': observation} if observation is InterruptedError
                             else {'return_value': observation})
                started = time.monotonic()
                with patch.object(processes.os, 'waitpid', **arguments), \\
                     patch.object(processes, 'signal_descendants'):
                    assert not processes.retire(.03), 'Unproved reaping became success'
                assert time.monotonic() - started < .5, 'Inner reaping exceeded both phase budgets'
            """)

    def test_zero_budget_permits_only_one_kernel_proof_check(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        for value, expected in (
            (ChildProcessError, True),
            (InterruptedError, False),
            ((0, 0), False),
            ((123, 0), False),
        ):
            arguments = (
                {"side_effect": value}
                if isinstance(value, type)
                else {"return_value": value}
            )
            with (
                self.subTest(value=value),
                patch.object(processes.os, "waitpid", **arguments) as wait,
                patch.object(processes, "signal_descendants") as send,
            ):
                self.assertEqual(processes.retire(0), expected)
                wait.assert_called_once_with(-1, os.WNOHANG)
                send.assert_not_called()

    def test_expiry_during_pin_or_identity_read_closes_without_signaling(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        saved = {"pid": 123, "parent": os.getpid(), "start_time": 1}
        for expire_at in ("pin", "identity"):
            clock = [0]

            def pin(_pid, expire=expire_at, moment=clock):
                if expire == "pin":
                    moment[0] = 2
                return 42

            def identity(_pid, moment=clock):
                moment[0] = 2
                return saved

            with (
                self.subTest(expire_at=expire_at),
                patch.object(
                    processes.time,
                    "monotonic",
                    side_effect=lambda moment=clock: moment[0],
                ),
                patch.object(processes, "descendants", return_value={123: saved}),
                patch.object(processes, "pidfd_open", side_effect=pin),
                patch.object(processes, "process_identity", side_effect=identity),
                patch.object(processes, "pidfd_send_signal") as send,
                patch.object(processes.os, "close") as close,
            ):
                processes.signal_descendants(15, deadline=1)
                send.assert_not_called()
                close.assert_called_once_with(42)

    def test_unreadable_observation_cannot_replace_kernel_proof(self):
        self.isolated("""
            import os, signal, subprocess
            from unittest.mock import patch
            from hstream_workflow import processes
            processes.become_subreaper()
            child = subprocess.Popen(['sleep', '60'])
            pin = processes.pidfd_open(child.pid)
            try:
                assert not processes.retire(0), 'Zero budget invented shutdown'
                with patch.object(processes, 'descendants', side_effect=PermissionError('unreadable')):
                    assert not processes.retire(.03), 'Live child disappeared from ownership'
                assert child.poll() is None
            finally:
                processes.pidfd_send_signal(pin, signal.SIGKILL)
                child.wait(timeout=5)
                os.close(pin)
            with patch.object(processes, 'descendants', side_effect=PermissionError('unreadable')):
                assert processes.retire(.03), 'Observation error defeated genuine ECHILD'
            assert processes.retire(0), 'Zero budget ignored genuine ECHILD'
            """)

    def test_kernel_proof_after_observation_failure_remains_authoritative(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        with (
            patch.object(
                processes.os, "waitpid", side_effect=[(0, 0), ChildProcessError()]
            ),
            patch.object(
                processes,
                "signal_descendants",
                side_effect=PermissionError("unreadable"),
            ) as send,
        ):
            self.assertTrue(processes.retire(0.03))
            send.assert_called_once()

    def test_term_ignoring_detached_child_is_killed_and_reaped(self):
        self.isolated("""
            import os, signal, subprocess, sys, tempfile, time
            from pathlib import Path
            from hstream_workflow import processes
            processes.become_subreaper()
            code = 'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print("ready",flush=True);time.sleep(60)'
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                launcher = subprocess.Popen([
                    sys.executable, '-c',
                    'import subprocess,sys;from pathlib import Path;'
                    'p=subprocess.Popen([sys.executable,"-c",sys.argv[2]],start_new_session=True,stdout=subprocess.PIPE);'
                    'assert p.stdout.readline()==b"ready\\\\n";Path(sys.argv[1]).write_text(str(p.pid))',
                    str(root/'pid'),code,
                ])
                pin = None
                try:
                    launcher.wait(timeout=5)
                    pid = int((root/'pid').read_text())
                    pin = processes.pidfd_open(pid)
                    assert processes.process_identity(pid)['parent'] == os.getpid()
                    assert processes.retire(.2), 'TERM-ignoring adopted child was not retired'
                    assert processes.process_identity(pid) is None
                finally:
                    if pin is not None:
                        try:
                            processes.pidfd_send_signal(pin,signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        os.close(pin)
                    assert processes.retire(.2), 'Fixture cleanup failed'
            """)

    def test_deadlines_bound_enumeration_ancestry_and_signal_batches(self):
        from unittest.mock import patch

        from hstream_workflow import processes

        count = 1000
        clock = [0.0]

        def now():
            clock[0] += 0.001
            return clock[0]

        # Reversed ancestry requires repeated expansion passes; no per-pass
        # deadline alone can bound the work once a very large scan is returned.
        rows = {
            pid: {"pid": pid, "parent": pid - 1, "start_time": 1}
            for pid in range(count, 0, -1)
        }
        rows[1]["parent"] = os.getpid()
        paths = [Path(str(pid)) for pid in rows]
        with (
            patch.object(processes.time, "monotonic", side_effect=now),
            patch.object(processes.Path, "iterdir", return_value=iter(paths)),
            patch.object(
                processes, "process_identity", side_effect=rows.get
            ) as identity,
        ):
            partial = processes.descendants(deadline=0.01)
            self.assertLess(identity.call_count, count)
            self.assertLess(len(partial), count)
        clock[0] = 0
        with (
            patch.object(processes.time, "monotonic", side_effect=now),
            patch.object(processes.Path, "iterdir", return_value=iter(paths)),
            patch.object(
                processes, "process_identity", side_effect=rows.get
            ) as identity,
        ):
            partial = processes.descendants(deadline=1.1)
            self.assertEqual(identity.call_count, count)
            self.assertLess(len(partial), count)
            self.assertLess(clock[0], 1.2)
        clock[0] = 0
        direct = {pid: dict(row, parent=os.getpid()) for pid, row in rows.items()}
        with (
            patch.object(processes.time, "monotonic", side_effect=now),
            patch.object(processes, "descendants", return_value=direct),
            patch.object(processes, "process_identity", side_effect=direct.get),
            patch.object(processes, "pidfd_open", return_value=42) as pin,
            patch.object(processes, "pidfd_send_signal") as send,
            patch.object(processes.os, "close") as close,
        ):
            processes.signal_descendants(15, deadline=0.01)
            self.assertLess(send.call_count, count)
            self.assertEqual(close.call_count, pin.call_count)
            self.assertLess(clock[0], 0.02)


if __name__ == "__main__":
    unittest.main()
