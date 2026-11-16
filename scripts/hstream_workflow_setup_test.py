"""Registry creation, recovery and qualification admission regressions."""

import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from hstream_workflow import setup, setup_execution as execution
from hstream_workflow.files import locked, read_json, write_json
from hstream_workflow.profiles import load_config
from hstream_workflow.schema import WorkflowError, digest


class SetupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.registry = self.root / "cluster"
        self.draft = {
            "schema": 1,
            "server": "ripper",
            "registry": str(self.registry),
            "controller_runtime": "/installed/cpu",
            "observer_registry": None,
            "game_roots": [str(self.root / "recordings")],
            "profiles": {
                "gpu": {
                    "label": "Fast computers",
                    "nodes": ["mini", "meshy"],
                    "partition": "gpu",
                    "gres": "gpu:rtx5090:1",
                    "cpus": 4,
                    "memory_mb": 16000,
                    "time_seconds": 1800,
                    "concurrency": 3,
                    "per_user": 3,
                    "account": "",
                    "qos": "",
                    "constraint": "",
                    "runtime_root": "/installed/gpu",
                }
            },
        }
        self.defaults = {
            "max_parallel": 3,
            "poll_interval_ms": 1000,
            "command_timeout_seconds": 30,
            "shutdown_timeout_seconds": 30,
            "observer_timeout_seconds": 86400,
        }
        self.cpu = {"cpus": 1, "memory_mb": 4096, "time_seconds": 600}

    def inspect(self, root, *, cpu):
        task, worker = (
            root + "/hstream-experiment-task",
            root + "/hstream-runtime-worker",
        )
        return task, {
            "root": root,
            "worker": worker,
            "architecture": "x86_64",
            "backend": "cpu" if cpu else "nvidia",
            "files": {task: "a" * 64, worker: "b" * 64},
        }

    def save(self):
        return setup.SetupStore(self.registry).save(self.draft, None)

    def compile(self):
        return execution.compile_configuration(
            self.draft, self.defaults, self.cpu, self.inspect
        )

    def test_new_registry_draft_can_be_saved_reopened_and_conflicts_preserve_last_save(
        self,
    ):
        self.assertEqual(
            setup.handle_request({"action": "load", "registry": str(self.registry)})[
                "draft"
            ],
            None,
        )
        saved = self.save()
        self.assertEqual(setup.SetupStore(self.registry).read(), saved)
        changed = copy.deepcopy(self.draft)
        changed["profiles"]["gpu"]["label"] = "My cluster"
        newer = setup.SetupStore(self.registry).save(changed, saved["revision"])
        with self.assertRaisesRegex(WorkflowError, "another window"):
            setup.SetupStore(self.registry).save(self.draft, saved["revision"])
        self.assertEqual(setup.SetupStore(self.registry).read(), newer)
        self.assertFalse((self.registry / "config.json").exists())

    def test_legacy_registry_can_be_edited_without_enabling_or_replacing_it(self):
        self.save()
        config = self.compile()
        del config["profiles"]["gpu"]["runtime"]["worker"]
        write_json(self.registry / "config.json", config)
        before = (self.registry / "config.json").read_bytes()
        imported = setup.import_configuration(self.registry, "ripper")
        self.assertEqual(imported["draft"], self.draft)
        self.assertIn("older", imported["issues"][0]["message"])
        self.assertFalse(setup.inspect_registry(self.registry, 30)["ready"])
        self.assertEqual(before, (self.registry / "config.json").read_bytes())

    def test_builder_produces_complete_native_configuration_and_aliases_share_capacity(
        self,
    ):
        self.save()
        self.draft["profiles"]["mini-only"] = copy.deepcopy(
            self.draft["profiles"]["gpu"]
        )
        self.draft["profiles"]["mini-only"]["nodes"] = ["mini"]
        config = self.compile()
        write_json(self.registry / "config.json", config)
        self.assertEqual(load_config(self.registry), config)
        self.assertEqual(
            config["profiles"]["gpu"]["pool"], config["profiles"]["mini-only"]["pool"]
        )
        self.draft["profiles"]["mini-only"]["concurrency"] = 5
        with self.assertRaisesRegex(WorkflowError, "same parallel"):
            self.compile()

    def test_drafts_cannot_supply_commands_or_fabricated_qualification(self):
        for mutate in (
            lambda d: d.update(server="ripper; touch /tmp/injected"),
            lambda d: d["profiles"]["gpu"].update(qualified_nodes={"mini": {}}),
            lambda d: d["profiles"]["gpu"].update(gres="gpu:1\n--wrap=bad"),
        ):
            draft = copy.deepcopy(self.draft)
            mutate(draft)
            with self.assertRaises(WorkflowError):
                setup.validate_draft(draft)

    def test_build_is_private_resumable_and_cannot_be_activated_before_tests(self):
        saved = self.save()
        config = self.compile()
        with patch.object(execution, "compile_configuration", return_value=config):
            result = execution.build(
                self.registry,
                {
                    "revision": saved["revision"],
                    "defaults": self.defaults,
                    "metadata_resources": self.cpu,
                },
            )
        self.assertFalse((self.registry / "config.json").exists())
        status = execution.qualification_status(self.registry, tick=False)
        self.assertFalse(status["tested"])
        self.assertEqual([row["node"] for row in status["tests"]], ["mini", "meshy"])
        with self.assertRaisesRegex(WorkflowError, "Every computer"):
            execution.activate(self.registry)
        self.assertEqual(
            execution.generation(self.registry)[0].name, result["generation"]
        )

    def test_qualification_requires_every_real_stage_and_one_explicit_node(self):
        plan = execution.test_plan(
            self.compile(),
            "gpu",
            "mini",
            self.root / "game",
            {"schema": 1, "source_path": "", "values": {}},
            60,
        )
        self.assertEqual(plan["profiles"]["gpu"]["resources"]["nodes"], ["mini"])
        self.assertEqual(
            set(stage["kind"] for stage in plan["stages"]),
            set(execution.CPU + execution.GPU),
        )
        self.assertTrue(
            all("catalog_key" not in stage["inputs"] for stage in plan["stages"])
        )
        scan = next(stage for stage in plan["stages"] if stage["id"] == "scan")
        self.assertEqual(scan["inputs"]["search_duration_seconds"], 60)
        self.assertEqual(plan["limits"]["max_parallel"], 1)

    def test_packaged_runtime_selects_only_the_setup_controller(self):
        from hstream_workflow.dispatch import installed_launcher
        from hstream_workflow.profiles import controller_files

        package = self.root / "installed"
        (package / "bin").mkdir(parents=True)
        for name in ("hstream-experiment-task", "hstream-runtime-worker"):
            shutil.copy2(shutil.which("true"), package / "bin" / name)
        (package / "scripts/hstream_workflow").mkdir(parents=True)
        (package / "scripts/hstream-workflow").write_text("old controller")
        (package / "scripts/hstream_workflow/cli.py").write_text("old module")
        self.draft["controller_runtime"] = str(package)
        config = execution.compile_configuration(
            self.draft,
            self.defaults,
            self.cpu,
            lambda root, cpu: (
                execution.inventory(root, cpu=True)
                if cpu
                else self.inspect(root, cpu=False)
            ),
        )
        self.assertEqual(installed_launcher(config), controller_files()[0])
        self.assertNotIn(
            str(package / "scripts/hstream-workflow"),
            config["profiles"]["metadata"]["runtime"]["files"],
        )

    def test_interrupted_generation_can_resume_without_erasing_inputs(self):
        saved = self.save()
        config = self.compile()
        request = {
            "revision": saved["revision"],
            "defaults": self.defaults,
            "metadata_resources": self.cpu,
        }

        def interrupted(path, value, **kwargs):
            if Path(path).name == "config.json":
                raise RuntimeError("interrupted")
            write_json(path, value, **kwargs)

        with patch.object(execution, "compile_configuration", return_value=config):
            with patch.object(execution, "write_json", side_effect=interrupted):
                with self.assertRaisesRegex(RuntimeError, "interrupted"):
                    execution.build(self.registry, request)
            execution.build(self.registry, request)
        self.assertEqual(
            execution.generation(self.registry)[1]["configuration"], config
        )

    def test_interrupted_controller_copy_never_publishes_partial_modules(self):
        self.save()
        with patch.object(
            execution.os, "fsync", side_effect=OSError("interrupted copy")
        ):
            with self.assertRaisesRegex(OSError, "interrupted copy"):
                execution.deploy_controller(self.registry)
        self.assertFalse(
            list((self.registry / "setup-releases").rglob("hstream-workflow"))
        )
        launcher = execution.deploy_controller(self.registry)
        self.assertTrue(os.access(launcher, os.X_OK))
        self.assertEqual(
            launcher.read_bytes(), execution.controller_files()[0].read_bytes()
        )
        self.assertEqual(execution.deploy_controller(self.registry), launcher)

    def test_a_client_opened_before_activation_cannot_admit_old_profiles(self):
        import hstream_workflow_controller_test as fixtures

        fixture = fixtures.ControllerTest()
        fixture.setUp()
        self.addCleanup(fixture.tearDown)
        config = read_json(fixture.registry / "config.json")
        config["profiles"]["cpu"]["label"] = "Changed after the client opened"
        write_json(fixture.registry / "config.json", config)
        with self.assertRaisesRegex(WorkflowError, "settings changed before admission"):
            fixture.controller.register(
                fixture.plan, fixture.game, defer_admission=True
            )
        self.assertEqual(fixture.controller.registry.read()["workflows"], {})
        self.assertFalse(
            (
                fixture.game / "_hockeymon/workflows" / fixture.plan["workflow_id"]
            ).exists()
        )

    def test_activation_requires_retired_controller_even_with_stale_heartbeat(self):
        from hstream_workflow.processes import host_identity, process_identity

        saved = self.save()
        config = self.compile()
        with patch.object(execution, "compile_configuration", return_value=config):
            execution.build(
                self.registry,
                {
                    "revision": saved["revision"],
                    "defaults": self.defaults,
                    "metadata_resources": self.cpu,
                },
            )
        heartbeat = {**host_identity(), **process_identity(os.getpid()), "time_ns": 0}
        write_json(self.registry / "heartbeat.json", heartbeat)
        with patch.object(
            execution, "qualification_status", return_value={"tested": True}
        ), patch("hstream_workflow.profiles.preflight_runtime"):
            with self.assertRaisesRegex(WorkflowError, "stale heartbeat"):
                execution.activate(self.registry)
            self.assertFalse((self.registry / "config.json").exists())
            with locked(self.registry / "controller-service.lock"):
                with self.assertRaisesRegex(ValueError, "Stop the HStream controller"):
                    execution.activate(self.registry)
            child = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"]
            )
            try:
                retired = {
                    **host_identity(),
                    **process_identity(child.pid),
                    "time_ns": 0,
                }
            finally:
                child.terminate()
                child.wait(timeout=5)
            write_json(self.registry / "heartbeat.json", retired)
            self.assertTrue(execution.activate(self.registry)["applied"])

    def test_rebuilding_previous_choices_can_replace_a_newer_configuration(self):
        saved = self.save()
        original = copy.deepcopy(self.draft)

        def build_current(saved):
            with patch.object(
                execution, "compile_configuration", return_value=self.compile()
            ):
                return execution.build(
                    self.registry,
                    {
                        "revision": saved["revision"],
                        "defaults": self.defaults,
                        "metadata_resources": self.cpu,
                    },
                )

        with patch.object(
            execution,
            "qualification_status",
            return_value={"tested": True, "tests": []},
        ), patch("hstream_workflow.profiles.preflight_runtime"):
            first = build_current(saved)
            execution.activate(self.registry)
            self.draft["profiles"]["gpu"]["label"] = "Other computers"
            saved = setup.SetupStore(self.registry).save(self.draft, saved["revision"])
            build_current(saved)
            execution.activate(self.registry)
            installed = execution.current_revision(self.registry)
            self.draft = original
            saved = setup.SetupStore(self.registry).save(self.draft, saved["revision"])
            restored = build_current(saved)
            self.assertNotEqual(first["generation"], restored["generation"])
            self.assertEqual(
                execution.generation(self.registry)[1]["configuration_revision"],
                installed,
            )
            execution.activate(self.registry)
        self.assertEqual(
            read_json(self.registry / "config.json")["profiles"]["gpu"]["label"],
            original["profiles"]["gpu"]["label"],
        )

    def test_discovery_recovers_managed_service_registry_with_spaces(self):
        # Run the actual remote probe against fixed local command fixtures.
        binary = self.root / "commands"
        binary.mkdir()
        unit = "hstream-workflow-0123456789abcdef.service"
        units = self.root / ".config/systemd/user"
        units.mkdir(parents=True)
        registry = str(self.root / "custom cluster")
        (units / unit).write_text(
            "# HStream registry: " + json.dumps(registry) + "\n[Unit]\n"
        )
        for name in ("systemctl", "scontrol", "sinfo", "squeue", "sacct"):
            path = binary / name
            path.write_text(
                "#!/usr/bin/python3\nimport sys\n"
                + "if 'list-unit-files' in sys.argv: print('"
                + unit
                + " enabled')\n"
                + "elif 'show' in sys.argv: print('ActiveState=active')\n"
            )
            path.chmod(0o700)
        output = subprocess.check_output(
            [sys.executable, "-c", setup.PROBE, "30"],
            env={**os.environ, "HOME": str(self.root), "PATH": str(binary)},
            text=True,
            timeout=10,
        )
        with patch.object(setup, "run_readonly", return_value=output):
            result = setup.discover("ripper", 30)
        self.assertEqual(result["service_registry"], registry)
        self.assertFalse(result["ready"])

    def test_remote_rejects_unsaved_edits_before_ssh_or_code_deployment(self):
        self.save()
        changed = copy.deepcopy(self.draft)
        changed["server"] = "different"
        with patch.object(execution, "deploy_controller") as deploy:
            with self.assertRaisesRegex(WorkflowError, "Save the edited"):
                execution.remote(
                    {
                        "action": "build",
                        "registry": str(self.registry),
                        "draft": changed,
                    }
                )
            deploy.assert_not_called()

    def test_discovery_connection_failure_has_next_step_and_never_qualifies(self):
        with patch.object(
            setup, "run_readonly", side_effect=WorkflowError("Permission denied")
        ):
            result = setup.discover("ripper", 30)
        self.assertFalse(result["ready"])
        self.assertIn("SSH", result["checks"][0]["next"])

    def test_setup_cli_works_without_an_installed_registry(self):
        request = self.root / "request.json"
        write_json(
            request,
            {
                "action": "save",
                "registry": str(self.registry),
                "revision": None,
                "draft": self.draft,
            },
        )
        process = subprocess.run(
            [
                sys.executable,
                "-m",
                "hstream_workflow.cli",
                "setup",
                "--request",
                str(request),
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(json.loads(process.stdout)["revision"], digest(self.draft))

    def test_unreachable_named_cluster_never_records_submission_intent(self):
        self.save()
        configuration = "ClusterName = local\nProctrackType = proctrack/cgroup\nTaskPlugin = task/cgroup\n"
        with patch.object(
            execution,
            "run_readonly",
            side_effect=[configuration, WorkflowError("Connection failed")],
        ) as command, patch.object(execution, "start_test") as start:
            with self.assertRaisesRegex(ValueError, "main submission server"):
                execution.execute(
                    {
                        "action": "start-test",
                        "registry": str(self.registry),
                        "draft": self.draft,
                        "timeout_seconds": 30,
                        "challenge": "test",
                    }
                )
            self.assertIn("--clusters=local", command.call_args.args[0])
            start.assert_not_called()
        self.assertFalse((self.registry / "setup-build.json").exists())

    def test_scheduler_preflight_requires_containment_and_accounting(self):
        with patch.object(
            execution,
            "run_readonly",
            return_value="ClusterName = local\nProctrackType = proctrack/linuxproc\n",
        ):
            with self.assertRaisesRegex(WorkflowError, "cgroup"):
                execution.check_scheduler(30)
        configuration = "ClusterName = local\nProctrackType = proctrack/cgroup\nTaskPlugin = task/cgroup,task/affinity\n"
        with patch.object(
            execution, "run_readonly", side_effect=[configuration, "", ""]
        ) as command:
            execution.check_scheduler(30)
        self.assertEqual(command.call_count, 3)
        self.assertEqual(command.call_args.args[0][0], "sacct")
        self.assertIn("--clusters=local", command.call_args.args[0])

    def test_readiness_requires_the_controller_to_load_this_configuration(self):
        self.save()
        write_json(self.registry / "config.json", self.compile())
        write_json(
            self.registry / "heartbeat.json",
            {
                "time_ns": time.time_ns(),
                "host": "another-host",
                "configuration_digest": "old",
            },
        )
        with patch("hstream_workflow.dispatch.installed_launcher"):
            result = setup.inspect_registry(self.registry, 30)
        self.assertFalse(result["ready"])
        self.assertIn("not loaded", result["checks"][-1]["message"])

    def test_qualification_resumes_real_native_workers_before_releasing_sample(self):
        import hstream_workflow_controller_test as fixtures

        fixture = fixtures.ControllerTest()
        fixture.setUp()
        self.addCleanup(fixture.tearDown)
        self.draft["game_roots"] = [str(fixture.root)]
        self.draft["profiles"]["gpu"]["nodes"] = ["mini"]
        saved = self.save()
        config = self.compile()
        # Execute the real controller/native worker protocol with CPU fixtures;
        # Slurm command acceptance/retirement has its separate scheduler suite.
        for key in ("metadata", "gpu"):
            profile = copy.deepcopy(fixture.profile)
            profile["capabilities"] = execution.CPU + execution.GPU
            profile["resources"]["nodes"] = ["mini"] if key == "gpu" else []
            profile["pool"] = key
            config["profiles"][key] = profile
        with patch.object(execution, "compile_configuration", return_value=config):
            execution.build(
                self.registry,
                {
                    "revision": saved["revision"],
                    "defaults": self.defaults,
                    "metadata_resources": self.cpu,
                },
            )
        request = {
            "profile": "gpu",
            "node": "mini",
            "game": str(fixture.game),
            "scan_seconds": 60,
            "user_config_snapshot": {"schema": 1, "source_path": "", "values": {}},
        }
        first = execution.start_test(self.registry, request)
        self.assertFalse(first["tested"])
        with self.assertRaisesRegex(WorkflowError, "previous test"):
            execution.start_test(self.registry, request)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            status = execution.qualification_status(self.registry, tick=True)
            if status["tested"]:
                break
            time.sleep(0.02)
        self.assertTrue(status["tested"], status)
        self.assertFalse((fixture.game / "_hockeymon/test-owner.json").exists())
        self.assertEqual(len(status["tests"][0]["status"]["nodes"]), 10)
        self.assertTrue(
            all(
                len(stage["attempts"]) == 1
                for stage in status["tests"][0]["status"]["nodes"]
            )
        )
        self.assertTrue(
            execution.qualification_status(self.registry, tick=False)["tested"]
        )


if __name__ == "__main__":
    unittest.main()
