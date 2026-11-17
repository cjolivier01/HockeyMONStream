"""Busy allocations remain distinct from successful GPU execution."""

import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from hstream_workflow.files import read_json
from hstream_workflow.setup_checks import defer_busy, is_deferred


class BusyCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.test = {"registry": self.temp.name, "workflow": "qualification"}
        self.status = {
            "owner_released": False,
            "epoch": 0,
            "nodes": [
                {
                    "kind": "experiment-probe",
                    "state": "queued",
                    "attempts": [
                        {
                            "id": "attempt",
                            "backend": "slurm",
                            "details": {
                                "state": "PENDING",
                                "reason": "Resources",
                                "scheduler_id": "7",
                                "cluster": "local",
                            },
                        }
                    ],
                }
            ],
        }

    def test_busy_intent_is_bound_and_only_never_started_retirement_is_deferred(self):
        controller = Mock()
        defer_busy(controller, self.test, self.status, enabled=True)
        controller.cancel.assert_called_once_with("qualification", 0)
        self.assertEqual(
            read_json(Path(self.temp.name) / "deferred-gpu.json")["attempt"], "attempt"
        )
        stopped = copy.deepcopy(self.status)
        stopped["owner_released"] = True
        details = stopped["nodes"][0]["attempts"][0]["details"]
        details.update(
            state="CANCELLED",
            host="",
            never_allocated=True,
            terminal=True,
            stopped=True,
            cleanup_complete=True,
        )
        self.assertTrue(is_deferred(self.test, stopped))
        for field, value in (
            ("host", "mini"),
            ("never_allocated", False),
            ("state", "FAILED"),
            ("scheduler_id", "8"),
            ("stopped", False),
            ("cleanup_complete", False),
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(stopped)
                changed["nodes"][0]["attempts"][0]["details"][field] = value
                self.assertFalse(is_deferred(self.test, changed))
        stopped["owner_released"] = False
        self.assertFalse(is_deferred(self.test, stopped))

    def test_slurm_accounting_never_assigned_sentinel_allows_deferred_cleanup(self):
        import os, pwd
        from types import SimpleNamespace
        from hstream_workflow import slurm
        from hstream_workflow.files import write_json

        root = Path(self.temp.name)
        user = pwd.getpwuid(os.getuid()).pw_name
        write_json(
            root / "submission.json",
            {
                "attempt_id": "attempt",
                "name": "test-job",
                "cluster": "local",
                "user": user,
                "submitted_after": "2026-01-01",
            },
        )
        write_json(
            root / "launch.json",
            {"attempt_id": "attempt", "scheduler_id": "7", "cluster": "local"},
        )
        # These are the real squeue %R and sacct values from a blocked check.
        with patch.object(
            slurm,
            "run_command",
            side_effect=[
                SimpleNamespace(stdout=f"7|test-job|PENDING|(Resources)||{user}|attempt\n"),
                SimpleNamespace(stdout=f"7|test-job|PENDING|0:0|None assigned|{user}|attempt|local\n"),
            ],
        ):
            queued = slurm.lookup(root, 30)
        self.assertEqual(queued["reason"], "(Resources)")
        self.status["nodes"][0]["attempts"][0]["details"] = queued
        controller = Mock()
        defer_busy(controller, self.test, self.status, enabled=True)
        controller.cancel.assert_called_once_with("qualification", 0)
        with patch.object(
            slurm,
            "run_command",
            side_effect=[
                SimpleNamespace(stdout=""),
                SimpleNamespace(
                    stdout=f"7|test-job|CANCELLED|0:0|None assigned|{user}|attempt|local\n"
                ),
            ],
        ):
            observation = slurm.lookup(root, 30)
        self.assertEqual(observation["host"], "")
        self.assertTrue(observation["never_allocated"])
        stopped = copy.deepcopy(self.status)
        stopped["owner_released"] = True
        stopped["nodes"][0]["attempts"][0]["details"] = dict(
            observation, stopped=True, cleanup_complete=True
        )
        self.assertTrue(is_deferred(self.test, stopped))
        for unknown in ("Unknown", "N/A"):
            with patch.object(
                slurm,
                "run_command",
                side_effect=[
                    SimpleNamespace(stdout=""),
                    SimpleNamespace(
                        stdout=f"7|test-job|CANCELLED|0:0|{unknown}|{user}|attempt|local\n"
                    ),
                ],
            ):
                observation = slurm.lookup(root, 30)
            stopped["nodes"][0]["attempts"][0]["details"] = dict(
                observation, stopped=True, cleanup_complete=True
            )
            self.assertFalse(is_deferred(self.test, stopped))

    def test_unknown_failure_and_unsupported_preparation_cannot_defer(self):
        for state, reason, enabled in (
            ("UNKNOWN", "Resources", True),
            ("PENDING", "BadConstraints", True),
            ("PENDING", "(BadConstraints)", True),
            ("FAILED", "Resources", True),
            ("PENDING", "Resources", False),
        ):
            with self.subTest(state=state, reason=reason, enabled=enabled):
                changed = copy.deepcopy(self.status)
                changed["nodes"][0]["attempts"][0]["details"].update(
                    state=state, reason=reason
                )
                controller = Mock()
                defer_busy(controller, self.test, changed, enabled=enabled)
                controller.cancel.assert_not_called()
                self.assertFalse((Path(self.temp.name) / "deferred-gpu.json").exists())

    def test_receipt_read_does_not_depend_on_negative_exists_cache(self):
        from hstream_workflow import executor

        # A descriptor-bound reader sees the new receipt even if an advisory
        # exists() lookup is still negative on the dispatcher.
        with patch.object(Path, "exists", return_value=False), patch.object(
            executor, "read_json", return_value={"stopped": True}
        ) as read:
            self.assertEqual(
                executor.completion(Path(self.temp.name)), {"stopped": True}
            )
            read.assert_called_once_with(Path(self.temp.name) / "completion.json")

    def test_gpu_preparation_rejects_unsupported_backend_before_deferral(self):
        import json
        from hstream_workflow import setup_runtime
        from hstream_workflow.schema import WorkflowError

        prepared = {
            "task": "/installed/task",
            "runtime": {"root": "/installed", "files": {}},
        }
        for backend, supported in (("amd", False), ("nvidia", False), ("nvidia", True)):
            capabilities = {
                "features": setup_runtime.REQUIRED_FEATURES,
                "gpu_backend": backend,
                "gpu_operations_supported": supported,
            }
            with patch.object(
                setup_runtime, "run_readonly", return_value=json.dumps(capabilities)
            ):
                if supported:
                    self.assertTrue(
                        setup_runtime.require_current_task(
                            prepared, 30, ["mini"], gpu=True
                        )["gpu_supported"]
                    )
                else:
                    with self.assertRaisesRegex(WorkflowError, "does not support"):
                        setup_runtime.require_current_task(
                            prepared, 30, ["mini"], gpu=True
                        )

    def test_activation_reuses_exact_file_pins_only_within_one_operation(self):
        from hstream_workflow import profiles
        from hstream_workflow.files import file_digest
        from hstream_workflow.schema import WorkflowError

        task = Path(self.temp.name) / "task"
        task.write_bytes(b"runtime fixture")
        task.chmod(0o700)
        profile = {
            "runtime": {"files": {str(task): file_digest(task, 1024)["sha256"]}},
            "task_command": [str(task)],
        }
        verified = set()
        with patch.object(profiles, "validate_profile"), patch.object(
            profiles, "file_digest", wraps=file_digest
        ) as read:
            profiles.preflight_runtime(profile, verified)
            profiles.preflight_runtime(profile, verified)
            self.assertEqual(read.call_count, 1)
            changed = copy.deepcopy(profile)
            changed["runtime"]["files"][str(task)] = "f" * 64
            with self.assertRaisesRegex(WorkflowError, "deployment changed"):
                profiles.preflight_runtime(changed, verified)


if __name__ == "__main__":
    unittest.main()
