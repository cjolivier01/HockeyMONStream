"""Local launch rejection never substitutes for uncertain child shutdown."""

import errno
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch
from uuid import uuid4

from hstream_workflow import local
from hstream_workflow.files import read_json, write_json
from hstream_workflow.schema import WorkflowError


class LocalTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hstream-local-launch-")
        self.root = Path(self.temporary.name)
        self.intent = {"backend": "local", "attempt_id": str(uuid4())}
        write_json(self.root / "submission.json", self.intent)
        self.command = [sys.executable, "-c", "import time; time.sleep(60)"]

    def tearDown(self):
        self.temporary.cleanup()

    def isolated(self, source):
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(__file__).resolve().parent)
        return subprocess.run(
            [sys.executable, "-c", textwrap.dedent(source), str(self.root)],
            env=environment,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def reject(self):
        with patch.object(
            subprocess.Popen,
            "_execute_child",
            side_effect=BlockingIOError(
                errno.EAGAIN, "Resource temporarily unavailable"
            ),
        ), self.assertRaises(BlockingIOError):
            local.submit(self.root, self.command)
        return read_json(self.root / "launch-rejected.json")

    def test_actual_process_limit_rejection_has_exact_terminal_proof(self):
        result = self.isolated("""
            import errno
            import resource
            import sys
            from pathlib import Path
            from hstream_workflow import local

            root = Path(sys.argv[1])
            limits = resource.getrlimit(resource.RLIMIT_NPROC)
            try:
                resource.setrlimit(resource.RLIMIT_NPROC, (0, limits[1]))
                try:
                    launched = local.submit(root, [sys.executable, '-c', 'pass'])
                except OSError as exc:
                    assert exc.errno == errno.EAGAIN, str(exc)
                else:
                    # Root or a process with CAP_SYS_RESOURCE can bypass the
                    # limit. Retire the harmless child before skipping.
                    local._children[launched['pid']].wait(timeout=5)
                    sys.exit(77)
            finally:
                resource.setrlimit(resource.RLIMIT_NPROC, limits)
            """)
        if result.returncode == 77:
            self.skipTest("This process can bypass RLIMIT_NPROC")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        proof = read_json(self.root / "launch-rejected.json")
        self.assertEqual(proof["attempt_id"], self.intent["attempt_id"])
        self.assertEqual(proof["errno"], errno.EAGAIN)
        observation = local.lookup(self.root)
        self.assertEqual(observation["state"], "REJECTED")
        self.assertTrue(observation["terminal"])
        self.assertTrue(observation["stopped"])
        self.assertTrue(observation["cleanup_complete"])
        self.assertEqual(observation["attempt_id"], self.intent["attempt_id"])

    def test_queued_cancellation_preserves_a_definite_launch_rejection(self):
        write_json(self.root / "cancel.json", {"attempt_id": self.intent["attempt_id"]})
        self.reject()
        self.assertEqual(local.lookup(self.root)["state"], "REJECTED")

    def test_cancellation_alone_does_not_prove_a_live_supervisor_stopped(self):
        write_json(self.root / "cancel.json", {"attempt_id": self.intent["attempt_id"]})
        launched = local.submit(self.root, self.command)
        child = local._children[launched["pid"]]
        try:
            self.assertEqual(local.lookup(self.root)["state"], "RUNNING")
            self.assertFalse((self.root / "launch-rejected.json").exists())
        finally:
            child.kill()
            child.wait(timeout=5)
            local.collect()
        self.assertEqual(local.lookup(self.root)["state"], "UNKNOWN")

    def test_crash_before_rejection_publication_remains_uncertain(self):
        result = self.isolated("""
            import errno
            import os
            import subprocess
            import sys
            from pathlib import Path
            from unittest.mock import patch
            from hstream_workflow import local

            root = Path(sys.argv[1])
            original = local.write_json
            def publish(path, *args, **kwargs):
                if path.name == 'launch-rejected.json':
                    os._exit(73)
                return original(path, *args, **kwargs)
            local.write_json = publish
            with patch.object(subprocess.Popen, '_execute_child',
                              side_effect=BlockingIOError(errno.EAGAIN, 'fork rejected')):
                local.submit(root, [sys.executable, '-c', 'pass'])
            """)
        self.assertEqual(result.returncode, 73, result.stdout + result.stderr)
        self.assertTrue((self.root / "dispatch.json").exists())
        self.assertFalse((self.root / "launch-rejected.json").exists())
        write_json(self.root / "cancel.json", {"attempt_id": self.intent["attempt_id"]})
        observation = local.lookup(self.root)
        self.assertEqual(observation["state"], "UNKNOWN")
        self.assertFalse(observation["terminal"])

    def test_constructor_error_after_child_acceptance_remains_uncertain(self):
        original = subprocess.Popen._execute_child
        created = []

        def accepted(child, *args, **kwargs):
            original(child, *args, **kwargs)
            created.append(child)
            raise OSError(errno.EIO, "Acceptance response could not be read")

        try:
            with patch.object(
                subprocess.Popen, "_execute_child", accepted
            ), self.assertRaises(OSError):
                local.submit(self.root, self.command)
            self.assertEqual(len(created), 1)
            self.assertIsNone(created[0].poll())
            self.assertFalse((self.root / "launch-rejected.json").exists())
            self.assertEqual(local.lookup(self.root)["state"], "UNKNOWN")
        finally:
            for child in created:
                child.kill()
                child.wait(timeout=5)

    def test_foreign_or_conflicting_rejection_proof_is_rejected(self):
        proof = self.reject()
        write_json(
            self.root / "launch-rejected.json", {**proof, "attempt_id": str(uuid4())}
        )
        with self.assertRaisesRegex(WorkflowError, "rejection identity"):
            local.lookup(self.root)
        write_json(self.root / "launch-rejected.json", proof)
        for name in ("launch.json", "supervisor.json"):
            with self.subTest(name=name):
                write_json(self.root / name, {"attempt_id": self.intent["attempt_id"]})
                with self.assertRaisesRegex(WorkflowError, "conflicts"):
                    local.lookup(self.root)
                (self.root / name).unlink()


if __name__ == "__main__":
    unittest.main()
