"""Filesystem containment, bounded log capture and portable content checks."""

import hashlib
import io
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from hstream_workflow.bounded_log import SEGMENT_BYTES, Capture
from hstream_workflow.files import file_digest, read_json, read_tail
from hstream_workflow.schema import WorkflowError


class FilesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.directory = self.root / "artifacts"
        self.directory.mkdir(mode=0o700)
        self.path = self.directory / "evidence"
        self.path.write_bytes(b"native evidence")

    def tearDown(self):
        self.temporary.cleanup()

    def test_content_identity_contains_no_host_device_number(self):
        result = file_digest(self.path, 100)
        self.assertEqual(
            result,
            {"size": 15, "sha256": hashlib.sha256(b"native evidence").hexdigest()},
        )

    def test_ancestor_and_final_symlinks_are_rejected(self):
        link = self.root / "alias"
        link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaises(OSError):
            file_digest(link / "evidence", 100)
        link.unlink()
        link.symlink_to(self.path)
        with self.assertRaises(OSError):
            file_digest(link, 100)

    def test_ancestor_replacement_during_hash_is_rejected(self):
        original = os.read
        moved = False

        def read(descriptor, count):
            nonlocal moved
            contents = original(descriptor, count)
            if not moved:
                moved = True
                self.directory.rename(self.root / "old-artifacts")
                self.directory.mkdir(mode=0o700)
                self.path.write_bytes(b"native evidence")
            return contents

        with patch(
            "hstream_workflow.files.os.read", side_effect=read
        ), self.assertRaises(WorkflowError):
            file_digest(self.path, 100)

    def test_growth_and_nonregular_files_fail_closed(self):
        with self.assertRaises(WorkflowError):
            file_digest(self.path, 2)
        pipe = self.directory / "pipe"
        os.mkfifo(pipe)
        with self.assertRaises(WorkflowError):
            file_digest(pipe, 100)

    def test_ambiguous_json_and_nonfinite_values_are_rejected(self):
        for value in (
            b'{"epoch":0,"epoch":1}',
            b'{"value":NaN}',
            b'{"value":Infinity}',
        ):
            with self.subTest(value=value):
                self.path.write_bytes(value)
                with self.assertRaises(WorkflowError):
                    read_json(self.path)

    def test_native_output_retains_bounded_tail_without_blocking_reader(self):
        output = b"x" * (SEGMENT_BYTES * 3) + b"final native diagnostic\n"
        capture = Capture(self.root, io.BytesIO(output))
        self.assertIsNone(capture.finish(5))
        logs = list(self.root.glob("task.log*"))
        self.assertEqual(len(logs), 2)
        self.assertLessEqual(
            sum(path.stat().st_size for path in logs), 2 * SEGMENT_BYTES
        )
        self.assertTrue(
            (self.root / "task.log").read_bytes().endswith(b"final native diagnostic\n")
        )

    def test_stopped_log_tail_is_bounded_and_rejects_symlinks_and_pipes(self):
        self.path.write_bytes(b"x" * 10000 + b"failure")
        self.assertEqual(read_tail(self.path, 7), b"failure")
        alias = self.directory / "alias"
        alias.symlink_to(self.path)
        with self.assertRaises(OSError):
            read_tail(alias, 7)
        pipe = self.directory / "pipe"
        os.mkfifo(pipe)
        with self.assertRaises(WorkflowError):
            read_tail(pipe, 7)

    def test_stopped_log_replacement_is_rejected(self):
        original = os.read

        def replace(descriptor, count):
            contents = original(descriptor, count)
            self.path.rename(self.directory / "old")
            self.path.write_bytes(b"replacement")
            return contents

        with patch(
            "hstream_workflow.files.os.read", side_effect=replace
        ), self.assertRaises(WorkflowError):
            read_tail(self.path, 7)

    def test_stopped_log_tail_handles_short_reads(self):
        self.path.write_bytes(b"x" * 10000 + b"failure")
        original = os.read
        with patch(
            "hstream_workflow.files.os.read",
            side_effect=lambda descriptor, count: original(descriptor, min(count, 2)),
        ):
            self.assertEqual(read_tail(self.path, 7), b"failure")


if __name__ == "__main__":
    unittest.main()
