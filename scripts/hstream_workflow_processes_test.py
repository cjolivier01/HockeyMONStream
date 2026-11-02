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
                processes.descendants = lambda: {}
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


if __name__ == "__main__":
    unittest.main()
