import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import restart_workflow_controllers as deploy
from hstream_workflow.files import write_json
from hstream_workflow.processes import host_identity, process_identity


class RestartControllersTest(unittest.TestCase):
    def test_matches_only_the_package_serving_a_registry(self):
        with tempfile.TemporaryDirectory() as directory:
            process = Path(directory)
            package = process / "package"
            package.mkdir()
            (process / "cwd").symlink_to(package)
            with patch.object(deploy, "SCRIPTS", package):
                for entry, expected in [
                    (["hstream-workflow", "serve"], True),
                    ([str(package / "hstream-workflow"), "serve"], True),
                    (["-m", "hstream_workflow.cli", "serve"], True),
                    (["/other/release/hstream-workflow", "serve"], False),
                    (["hstream-workflow", "status"], False),
                    (["-m", "hstream_workflow.dagster_registry", "serve"], False),
                ]:
                    with self.subTest(entry=entry):
                        command = [
                            "python3",
                            *entry,
                            "--registry",
                            "/shared/my registry",
                        ]
                        (process / "cmdline").write_bytes(
                            os.fsencode("\0".join(command))
                        )
                        self.assertEqual(
                            deploy.controller_registry(process),
                            Path(command[-1]) if expected else None,
                        )
                (process / "cwd").unlink()
                (process / "cwd").symlink_to(process)
                (process / "cmdline").write_bytes(
                    b"python3\0-m\0hstream_workflow.cli\0serve\0--registry=/old"
                )
                self.assertIsNone(deploy.controller_registry(process))

    def test_visits_system_and_all_running_user_managers(self):
        user_manager = {"Id": "user@1001.service"}
        system_service = {"Id": "custom-name.service"}
        user_service = {"Id": "hstream-workflow-123.service"}
        with patch.object(Path, "is_dir", return_value=True), patch.object(
            deploy,
            "running_services",
            side_effect=[[system_service, user_manager], [user_service]],
        ), patch.object(deploy, "restart_controller") as restart:
            deploy.restart_services()
        self.assertEqual(
            [call.args for call in restart.call_args_list],
            [
                (["systemctl", "--system"], system_service, None),
                (["systemctl", "--system"], user_manager, None),
                (["systemctl", "--user", "--machine=1001@.host"], user_service, 1001),
            ],
        )

    def test_restarts_only_current_mainpid_and_checks_the_replacement(self):
        manager = ["systemctl", "--user"]
        pid = os.getpid()
        service = {
            "Id": "hstream-workflow-ab.service",
            "MainPID": str(pid + 1),
            "KillMode": "process",
        }
        registry = Path("/registry")
        with patch.object(
            deploy, "controller_registry", return_value=registry
        ), patch.object(
            deploy, "main_pid", side_effect=[0, pid + 1, pid, pid]
        ), patch.object(
            subprocess, "run"
        ) as run:
            deploy.restart_controller(manager, service)
            run.assert_not_called()
            deploy.restart_controller(manager, service)
            self.assertEqual(
                run.call_args_list[0].args[0],
                [*manager, "try-restart", "--", service["Id"]],
            )
            self.assertEqual(
                run.call_args_list[1].args[0][-3:],
                ["--heartbeat", str(registry), str(pid)],
            )

    def test_replaced_heartbeat_is_retried_and_old_heartbeat_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            registry = Path(directory)
            pid = os.getpid()
            config = {"defaults": {"command_timeout_seconds": 1}}
            identity = {**host_identity(), **process_identity(pid)}
            write_json(registry / "config.json", config)
            write_json(registry / "heartbeat.json", {**identity, "start_time": 0})
            with patch.object(
                deploy.time, "monotonic", side_effect=[0, 0, 2]
            ), patch.object(deploy.time, "sleep"):
                with self.assertRaisesRegex(RuntimeError, "No heartbeat"):
                    deploy.wait_for_heartbeat(registry, pid)
            with patch.object(
                deploy,
                "read_json",
                side_effect=[config, deploy.WorkflowError("replaced"), identity],
            ):
                deploy.wait_for_heartbeat(registry, pid)


if __name__ == "__main__":
    unittest.main()
