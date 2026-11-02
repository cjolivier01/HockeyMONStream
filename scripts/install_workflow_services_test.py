#!/usr/bin/env python3
"""Exercise real temporary files with simulated package/systemd/Slurm commands."""

import copy
import hashlib
import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from install_workflow_services import (
    Installer,
    InstallError,
    read_regular,
    unit_argument,
    validate_manifest,
    validate_slurm_configuration,
)

SLURM_CONFIG = b"""ClusterName=cluster
SlurmctldHost=controller
SlurmUser=slurm
StateSaveLocation=/var/spool/slurmctld
SlurmdSpoolDir=/var/spool/slurmd
AccountingStorageHost=controller
AccountingStorageType=accounting_storage/slurmdbd
ProctrackType=proctrack/cgroup
TaskPlugin=task/cgroup
SelectType=select/cons_tres
NodeName=gpu01 CPUs=16 RealMemory=64000 Gres=gpu:rtx5090:1
PartitionName=gpu Nodes=gpu01 State=UP
"""
CGROUP_CONFIG = b"CgroupPlugin=cgroup/v2\nConstrainDevices=yes\nConstrainCores=yes\nConstrainRAMSpace=yes\n"


class HostCommands:
    def __init__(self):
        self.calls = []
        self.active = set()
        self.fail = None

    def __call__(self, argv, **kwargs):
        self.calls.append(argv)
        if self.fail and self.fail(argv):
            raise InstallError("injected command failure")
        output = ""
        if argv[:3] == ["scontrol", "show", "config"]:
            output = SLURM_CONFIG.decode()
        elif argv[:2] == ["scontrol", "ping"]:
            output = "Slurmctld(primary) at controller is UP\n"
        elif argv[:2] == ["systemctl", "is-active"]:
            if argv[-1] not in self.active:
                raise InstallError("inactive service")
        elif argv[0] == "systemctl" and argv[1] in {"start", "restart"}:
            self.active.add(argv[-1])
        elif argv[:2] == ["systemctl", "stop"]:
            self.active.discard(argv[-1])
        elif argv[:2] == ["systemctl", "show"]:
            output = "1234\n"
        return subprocess.CompletedProcess(argv, 0, output, "")


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.commands = HostCommands()
        for directory in (
            "shared",
            "inputs",
            "etc/slurm",
            "opt/hstream/scripts",
            "var/lib/workflows",
        ):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / "etc/slurm/cgroup.conf").write_bytes(CGROUP_CONFIG)
        registry = {
            "schema": 1,
            "defaults": {},
            "profiles": {
                "cpu": {
                    "executor": "local",
                    "resources": {"gpus": 0},
                    "runtime": {"backend": "cpu"},
                }
            },
            "admission_profile": "cpu",
            "allowed_game_roots": ["/shared"],
            "observer_registry": None,
        }
        data = json.dumps(registry).encode()
        (self.root / "inputs/registry.json").write_bytes(data)
        self.manifest = {
            "schema": 1,
            "id": "test",
            "health_timeout_seconds": 30,
            "identity": {"user": "worker", "uid": 1001, "gid": 1001},
            "state_dir": "/var/lib/workflow-install",
            "shared_paths": ["/shared"],
            "slurm": {
                "cluster": "cluster",
                "roles": ["submission"],
                "config_dir": "/etc/slurm",
                "packages": {},
                "bootstrap": None,
            },
            "workflow": {
                "kind": "standalone",
                "unit": "hstream-workflow-controller",
                "python": "/usr/bin/python3",
                "code_dir": "/opt/hstream/scripts",
                "registry": "/var/lib/workflows",
                "registry_config": {
                    "path": "/inputs/registry.json",
                    "sha256": hashlib.sha256(data).hexdigest(),
                },
                "environment_file": None,
                "handover": None,
            },
            "dagster": None,
            "adopt": {},
        }
        self.installer = Installer(self.manifest, root=self.root, run=self.commands)
        (self.root / "var/lib/workflows/heartbeat.json").write_text(
            json.dumps({"pid": 1234, "time_ns": time.time_ns()})
        )

    def tearDown(self):
        self.temp.cleanup()

    def test_read_only_check_makes_no_files_or_service_changes(self):
        self.commands.active.add("hstream-workflow-controller")
        before = sorted(
            str(path.relative_to(self.root)) for path in self.root.rglob("*")
        )
        self.assertEqual(self.installer.check()["state"], "healthy")
        after = sorted(
            str(path.relative_to(self.root)) for path in self.root.rglob("*")
        )
        self.assertEqual(before, after)
        self.assertFalse(
            any(call[:2] == ["systemctl", "restart"] for call in self.commands.calls)
        )

    def test_install_twice_is_idempotent_and_preserves_cluster(self):
        original = (self.root / "etc/slurm/cgroup.conf").read_bytes()
        installed = self.installer.install("integrate")
        self.assertEqual(installed["state"], "installed")
        calls = len(self.commands.calls)
        second = self.installer.install("integrate")
        self.assertEqual(second["state"], "unchanged")
        self.assertFalse(
            any(
                call[0] == "systemctl"
                and call[1] in {"start", "restart", "stop", "enable"}
                for call in self.commands.calls[calls:]
            )
        )
        self.assertEqual((self.root / "etc/slurm/cgroup.conf").read_bytes(), original)
        self.assertTrue((Path(installed["backup"]) / "complete").exists())

    def test_unmanaged_file_is_rejected_before_mutation(self):
        path = self.root / "etc/systemd/system/hstream-workflow-controller.service"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"operator owned\n")
        with self.assertRaisesRegex(InstallError, "unmanaged"):
            self.installer.install("integrate")
        self.assertEqual(path.read_bytes(), b"operator owned\n")
        self.assertNotIn("hstream-workflow-controller", self.commands.active)

    def test_explicit_adoption_then_operator_edits_require_reconciliation(self):
        path = self.root / "etc/systemd/system/hstream-workflow-controller.service"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"operator owned\n")
        self.manifest["adopt"][
            "/etc/systemd/system/hstream-workflow-controller.service"
        ] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.installer.install("integrate")
        path.write_bytes(b"operator override\n")
        with self.assertRaisesRegex(InstallError, "edited"):
            self.installer.install("integrate")
        self.assertEqual(path.read_bytes(), b"operator override\n")

    def test_failed_activation_restores_files_and_previous_service_activity(self):
        path = self.root / "etc/systemd/system/hstream-workflow-controller.service"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"previous unit\n")
        self.manifest["adopt"][
            "/etc/systemd/system/hstream-workflow-controller.service"
        ] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.commands.active.add("hstream-workflow-controller")
        failures = []

        def fail_once(argv):
            if argv[:2] == ["systemctl", "restart"] and not failures:
                failures.append(True)
                return True
            return False

        self.commands.fail = fail_once
        with self.assertRaisesRegex(InstallError, "restored"):
            self.installer.install("integrate")
        self.assertEqual(path.read_bytes(), b"previous unit\n")
        self.assertIn("hstream-workflow-controller", self.commands.active)
        self.assertFalse((self.root / "var/lib/workflows/config.json").exists())

    def test_failed_rollback_is_distinguished(self):
        self.commands.fail = lambda argv: argv[:2] in (
            ["systemctl", "start"],
            ["systemctl", "stop"],
        )
        with self.assertRaisesRegex(InstallError, "rollback FAILED"):
            self.installer.install("integrate")

    def test_interrupted_activation_journal_is_recovered_before_reinstall(self):
        self.commands.fail = lambda argv: argv[:2] == ["systemctl", "start"]
        with mock.patch.object(
            self.installer,
            "rollback",
            side_effect=InstallError("injected controller death"),
        ), self.assertRaises(InstallError):
            self.installer.install("integrate")
        self.assertTrue((self.installer.state / "pending.json").exists())
        self.commands.fail = None
        self.assertEqual(self.installer.install("integrate")["state"], "installed")
        self.assertFalse((self.installer.state / "pending.json").exists())
        self.assertTrue(any(self.installer.state.glob("backups/*/rolled-back")))

    def test_pending_rollback_preserves_unexpected_external_edit(self):
        self.commands.fail = lambda argv: argv[:2] == ["systemctl", "start"]
        with mock.patch.object(
            self.installer,
            "rollback",
            side_effect=InstallError("injected controller death"),
        ), self.assertRaises(InstallError):
            self.installer.install("integrate")
        target = self.root / "etc/systemd/system/hstream-workflow-controller.service"
        target.write_text("external edit after failed activation")
        self.commands.fail = None
        with self.assertRaisesRegex(InstallError, "rollback FAILED"):
            self.installer.install("integrate")
        self.assertEqual(target.read_text(), "external edit after failed activation")

    def test_ambiguous_handover_does_not_roll_back_healthy_independent_worker(self):
        with mock.patch.object(
            self.installer,
            "finish_handover",
            side_effect=InstallError("lost schedule stop response"),
        ), self.assertRaisesRegex(InstallError, "stays running"):
            self.installer.install("integrate")
        self.assertIn("hstream-workflow-controller", self.commands.active)
        self.assertTrue((self.installer.state / "files.json").exists())
        self.assertFalse((self.installer.state / "pending.json").exists())

    def test_stale_or_wrong_process_heartbeat_is_not_health(self):
        self.commands.active.add("hstream-workflow-controller")
        heartbeat = self.root / "var/lib/workflows/heartbeat.json"
        heartbeat.write_text(json.dumps({"pid": 1234, "time_ns": 1}))
        with self.assertRaisesRegex(InstallError, "health bound"):
            self.installer.check()
        heartbeat.write_text(json.dumps({"pid": 999, "time_ns": time.time_ns()}))
        with self.assertRaisesRegex(InstallError, "current service"):
            self.installer.check()

    def test_config_digest_drift_and_symlinks_fail_closed(self):
        (self.root / "inputs/registry.json").write_bytes(b"{}")
        with self.assertRaisesRegex(InstallError, "digest"):
            self.installer.install("integrate")
        path = self.root / "inputs/private"
        path.write_text("secret")
        alias = self.root / "inputs/alias"
        alias.symlink_to(path)
        with self.assertRaises(OSError):
            read_regular(alias)
        fifo = self.root / "inputs/fifo"
        os.mkfifo(fifo)
        with self.assertRaises(InstallError):
            read_regular(fifo)

    def test_bootstrap_requires_accounting_containment_and_topology(self):
        configs = {
            "slurm.conf": SLURM_CONFIG,
            "cgroup.conf": CGROUP_CONFIG,
            "gres.conf": b"Name=gpu Type=rtx5090 File=/dev/nvidia0\n",
        }
        validate_slurm_configuration(configs, "cluster", ["compute"])
        for key, needle, replacement in (
            ("slurm.conf", b"accounting_storage/slurmdbd", b"accounting_storage/none"),
            ("cgroup.conf", b"ConstrainDevices=yes", b"ConstrainDevices=no"),
            ("slurm.conf", b"Gres=gpu:rtx5090:1", b""),
        ):
            broken = dict(configs)
            broken[key] = broken[key].replace(needle, replacement)
            with self.assertRaises(InstallError):
                validate_slurm_configuration(broken, "cluster", ["compute"])
        with self.assertRaisesRegex(InstallError, "accounting"):
            validate_slurm_configuration(configs, "cluster", ["controller"])

    def test_pinned_version_and_unit_escaping(self):
        self.assertIn("$$", unit_argument("/tmp/a$(touch nope)"))
        broken = copy.deepcopy(self.manifest)
        broken["identity"]["user"] = "user\nExecStart=bad"
        with self.assertRaises(InstallError):
            validate_manifest(broken)

    def test_handover_releases_dispatch_lock_before_waiting_for_worker(self):
        self.manifest["workflow"].update(
            kind="web",
            registry_config=None,
            handover={
                "lock": "/var/lib/workflows/dispatcher.lock",
                "receipt": "/var/lib/workflows/handover-receipt.json",
                "location": "web",
                "repository": "__repository__",
                "schedule": "stream_dispatcher_schedule",
                "database_identity": "d" * 64,
            },
        )
        installer = Installer(self.manifest, root=self.root, run=self.commands)
        installer.state.mkdir(parents=True)
        installer.handover_token = "x" * 32
        installer.begin_handover()
        import fcntl

        with (self.root / "var/lib/workflows/dispatcher.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(
            json.loads((installer.state / "handover.json").read_text())["state"],
            "pending",
        )


if __name__ == "__main__":
    unittest.main()
