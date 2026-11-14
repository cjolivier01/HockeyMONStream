"""Scheduler identity and ambiguous-acceptance regressions, without a cluster."""

import os
import pwd
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from uuid import uuid4

from hstream_workflow import slurm
from hstream_workflow.files import read_json, write_json
from hstream_workflow.schema import WorkflowError


class SlurmTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.attempt = str(uuid4())
        self.user = pwd.getpwuid(os.getuid()).pw_name
        self.intent = {
            "backend": "slurm",
            "attempt_id": self.attempt,
            "name": "hsw-" + self.attempt,
            "cluster": "fixture",
            "user": self.user,
            "submitted_after": "2026-01-01T00:00:00",
        }
        write_json(self.root / "submission.json", self.intent)
        self.profile = {
            "resources": {
                "cpus": 4,
                "memory_mb": 4096,
                "time_seconds": 121,
                "partition": "gpu",
                "nodes": ["compute-a", "compute-b"],
                "gres": "gpu:rtx5090:1",
                "constraint": "",
                "account": "",
                "qos": "",
            }
        }
        self.calls = []
        self.queue = self.row()
        self.accounting = ""
        self.acceptance = "42;fixture\n"
        self.errors = {}
        self.patcher = patch.object(slurm, "run_command", side_effect=self.command)
        self.patcher.start()

    def tearDown(self):
        self.patcher.stop()
        self.temporary.cleanup()

    def row(self, job="42", state="RUNNING", user=None, comment=None):
        return f"{job}|{self.intent['name']}|{state}|None|compute-a|{user or self.user}|{comment or self.attempt}\n"

    def account(self, state="COMPLETED", exit_code="0:0", cluster="fixture"):
        return f"42|{self.intent['name']}|{state}|{exit_code}|compute-a|{self.user}|{self.attempt}|{cluster}\n"

    def command(self, arguments, timeout):
        self.calls.append(arguments)
        if arguments[0] in self.errors:
            raise self.errors[arguments[0]]
        values = {
            "scontrol": "ClusterName = fixture\nProctrackType = proctrack/cgroup\nTaskPlugin = task/cgroup,task/affinity\n",
            "squeue": self.queue,
            "sacct": self.accounting,
            "sbatch": self.acceptance,
            "scancel": "",
        }
        return SimpleNamespace(stdout=values[arguments[0]])

    def test_lost_acceptance_is_adopted_by_exact_identity_without_resubmission(self):
        self.errors["sbatch"] = subprocess.TimeoutExpired("sbatch", 1)
        with self.assertRaises(subprocess.TimeoutExpired):
            slurm.submit(
                self.root,
                self.profile,
                [
                    "/opt/hstream/bin/hstream-runtime-worker",
                    "--request",
                    "/shared/request.json",
                ],
                1,
            )
        result = slurm.lookup(self.root, 1)
        self.assertEqual(result["scheduler_id"], "42")
        self.assertEqual(
            read_json(self.root / "launch.json")["attempt_id"], self.attempt
        )
        slurm.cancel(self.root, 1)
        self.assertEqual(sum(args[0] == "sbatch" for args in self.calls), 1)
        self.assertIn(
            "--gres=gpu:rtx5090:1",
            next(args for args in self.calls if args[0] == "sbatch"),
        )
        self.assertEqual(self.calls[-1], ["scancel", "--clusters=fixture", "42"])

    def test_accounting_proves_completion_during_queue_outage(self):
        self.errors["squeue"] = subprocess.CalledProcessError(1, "squeue")
        self.accounting = self.account()
        result = slurm.lookup(self.root, 1)
        self.assertTrue(result["terminal"])
        self.assertEqual(result["exit_code"], "0:0")
        slurm.cancel(self.root, 1)
        self.assertFalse(any(args[0] == "scancel" for args in self.calls))

    def test_unknown_acceptance_never_invents_terminal_or_retries_submission(self):
        self.errors["squeue"] = subprocess.TimeoutExpired("squeue", 1)
        self.errors["sacct"] = subprocess.TimeoutExpired("sacct", 1)
        with self.assertRaisesRegex(WorkflowError, "unresolved"):
            slurm.cancel(self.root, 1)
        self.assertEqual([args[0] for args in self.calls], ["squeue", "sacct"])

    def test_conflicting_comment_user_cluster_and_duplicate_jobs_are_rejected(self):
        for queue, accounting in (
            (self.row(comment=str(uuid4())), ""),
            (self.row(user="someone"), ""),
            ("", self.account(cluster="another")),
            (self.row() + self.row(job="43"), ""),
        ):
            with self.subTest(queue=queue, accounting=accounting):
                self.queue, self.accounting = queue, accounting
                with self.assertRaises(WorkflowError):
                    slurm.cancel(self.root, 1)
        self.assertFalse(any(args[0] == "scancel" for args in self.calls))

    def test_clusters_without_stored_job_comments_use_exact_uuid_name(self):
        self.queue = ""
        self.accounting = self.account().replace(
            "|" + self.attempt + "|fixture", "||fixture"
        )
        self.assertTrue(slurm.lookup(self.root, 1)["terminal"])

    def test_reused_job_id_cannot_replace_persisted_launch(self):
        write_json(
            self.root / "launch.json",
            {"attempt_id": self.attempt, "scheduler_id": "99", "cluster": "fixture"},
        )
        with self.assertRaisesRegex(WorkflowError, "identity changed"):
            slurm.lookup(self.root, 1)

    def test_cgroup_preflight_rejects_before_sbatch(self):
        with patch.object(
            slurm,
            "run_command",
            return_value=SimpleNamespace(stdout="ClusterName = fixture\n"),
        ) as command:
            with self.assertRaisesRegex(WorkflowError, "cgroup"):
                slurm.submit(self.root, self.profile, ["/bin/true"], 1)
            self.assertEqual(command.call_count, 1)


if __name__ == "__main__":
    unittest.main()
