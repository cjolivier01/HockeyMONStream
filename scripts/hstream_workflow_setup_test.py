"""Registry creation, recovery and qualification admission regressions."""

import copy
from contextlib import contextmanager
import json
import os
import shutil
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from hstream_workflow import (
    setup,
    setup_execution as execution,
    setup_runtime as runtime_setup,
)
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

    def test_transport_errors_explain_legacy_profiles_without_nested_json(self):
        legacy = "Slurm profiles need a pinned runtime.worker; requalify legacy interpreter profiles"
        wrapped = json.dumps({"error": json.dumps({"error": legacy})})
        with self.assertRaises(WorkflowError) as raised:
            setup.run_readonly(
                [
                    sys.executable,
                    "-c",
                    "import sys; print(sys.argv[1], file=sys.stderr); sys.exit(2)",
                    wrapped,
                ],
                10,
            )
        message = str(raised.exception)
        self.assertIn("make deploy installs the programs", message)
        self.assertIn("Save and build registry", message)
        self.assertNotIn('{"error"', message)
        self.assertEqual(
            setup.readable_error('{"error": "Permission denied"}'), "Permission denied"
        )

    def test_start_before_apply_explains_required_steps_without_loading_legacy_config(
        self,
    ):
        self.save()
        config = self.compile()
        del config["profiles"]["gpu"]["runtime"]["worker"]
        write_json(self.registry / "config.json", config)
        with patch.object(execution, "run_readonly") as run:
            with self.assertRaisesRegex(WorkflowError, "Apply tested settings"):
                execution.start_service(self.registry, 30)
            run.assert_not_called()

    def test_missing_runtime_names_saved_folder_and_repair_action(self):
        old = self.root / "old-release"
        for cpu, action in (
            (True, "Use installed HStream for main server"),
            (False, "Advanced profile settings"),
        ):
            with self.assertRaises(WorkflowError) as raised:
                execution.inventory(str(old), cpu=cpu)
            self.assertIn(str(old), str(raised.exception))
            self.assertIn(action, str(raised.exception))
        old.mkdir()
        shutil.copyfile(sys.executable, old / "hstream-experiment-task")
        (old / "hstream-experiment-task").chmod(0o700)
        with self.assertRaises(WorkflowError) as raised:
            execution.inventory(str(old), cpu=True)
        self.assertIn("hstream-runtime-worker", str(raised.exception))
        self.assertIn("make deploy just succeeded", str(raised.exception))

    def test_discovery_resolves_installed_program_links_without_qualifying(self):
        binary = self.root / "commands"
        binary.mkdir()
        installation = self.root / "installed"
        (installation / "bin").mkdir(parents=True)
        for name in ("hstream-runtime-worker", "hstream-experiment-task"):
            program = installation / "bin" / name
            shutil.copyfile(sys.executable, program)
            program.chmod(0o700)
            (binary / name).symlink_to(program)
        for present in (True, False):
            if not present:
                (binary / "hstream-runtime-worker").unlink()
            output = subprocess.check_output(
                [sys.executable, "-c", setup.PROBE, "10"],
                env={**os.environ, "HOME": str(self.root), "PATH": str(binary)},
                text=True,
                timeout=15,
            )
            with patch.object(setup, "run_readonly", return_value=output):
                result = setup.discover("ripper", 10)
            self.assertEqual(
                result["installed_runtime"], str(installation) if present else None
            )
            self.assertFalse(result["ready"])

    def test_shared_storage_probe_needs_visible_marker_and_write_receipt(self):
        from hstream_workflow import setup_storage as storage

        self.save()

        def remote(argv, timeout):
            command = shlex.split(argv[-1])
            self.assertEqual(command[:2], ["python3", "-c"])
            if argv[-2] == "stubby":
                command[3] = str(self.root / "local-overlay/absent-marker")
            return subprocess.check_output(command, text=True, timeout=timeout)

        with patch.object(storage, "run_readonly", side_effect=remote):
            self.assertEqual(
                storage.check(self.registry, ["mini", "stubby"], 30, 2), ["stubby"]
            )
        with patch.object(storage, "run_readonly", return_value='{"shared":true}'):
            self.assertEqual(
                storage.check(self.registry, ["stubby"], 30, 1), ["stubby"]
            )
        self.assertFalse(list(self.registry.glob("setup-storage-*.json")))

    def storage_request(self):
        saved = self.save()
        target = self.root / "shared-registry"
        target.mkdir(mode=0o700)
        return {
            "action": "build",
            "registry": str(self.registry),
            **saved,
            "shared_registry": str(target),
            "timeout_seconds": 30,
            "defaults": self.defaults,
            "metadata_resources": self.cpu,
        }

    def test_unshared_registry_recovers_in_shared_storage_without_moving_history(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        write_json(self.registry / "retained-history.json", {"history": "unchanged"})

        def check(root, *args):
            return ["stubby"] if Path(root) == self.registry else []

        with patch.object(storage, "check", side_effect=check):
            moved, message = storage.prepare(request)
            repeated, _ = storage.prepare(request)
        self.assertEqual(moved, repeated)
        self.assertEqual(moved["registry"], request["shared_registry"])
        self.assertIn("Created shared settings", message)
        self.assertEqual(setup.SetupStore(self.registry).read()["draft"], self.draft)
        self.assertEqual(
            read_json(self.registry / "retained-history.json"), {"history": "unchanged"}
        )
        loaded = setup.handle_request(
            {"action": "load", "registry": str(self.registry)}
        )
        self.assertEqual(loaded["draft"], moved["draft"])
        self.assertEqual(loaded["progress"]["revision"], moved["revision"])
        self.assertFalse(loaded["progress"]["installed"])
        # Editing the original choices supersedes the old relocation reference.
        changed = copy.deepcopy(self.draft)
        changed["profiles"]["gpu"]["label"] = "New original choices"
        setup.SetupStore(self.registry).save(changed, request["revision"])
        self.assertEqual(storage.recovered(self.registry)["draft"], changed)

    def test_relocation_preserves_existing_destination_and_concurrent_edits(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])
        existing = {**self.draft, "registry": str(target), "server": "another-server"}
        setup.SetupStore(target).save(existing, None)

        def check(root, *args):
            return ["stubby"] if Path(root) == self.registry else []

        with patch.object(storage, "check", side_effect=check):
            moved, _ = storage.prepare(request)
        self.assertNotEqual(moved["registry"], str(target))
        self.assertEqual(setup.SetupStore(target).read()["draft"], existing)
        (self.registry / "setup-location.json").unlink()

        def changed(root, *args):
            if Path(root) != self.registry:
                draft = {**self.draft, "server": "edited"}
                setup.SetupStore(self.registry).save(draft, request["revision"])
            return check(root)

        with patch.object(storage, "check", side_effect=changed):
            with self.assertRaisesRegex(WorkflowError, "choices changed"):
                storage.prepare(request)
        self.assertFalse((self.registry / "setup-location.json").exists())

    def test_existing_shared_setup_resumes_without_adopting_broken_old_history(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])
        existing = setup.SetupStore(target).save(
            {**self.draft, "registry": str(target)}, None
        )
        ledger = {
            "schema": 1,
            "reservations": {},
            "workflows": {
                "old-run": {
                    "game": "/recordings/old-game",
                    "directory": "/missing/old-run",
                }
            },
        }
        write_json(self.registry / "registry.json", ledger)
        write_json(target / "retained-history.json", {"keep": True})
        with patch.object(storage, "check", side_effect=[["stubby"], []]), patch.object(
            execution, "idle", side_effect=AssertionError("Must not adopt old history")
        ), locked(self.registry / "controller-service.lock"):
            resumed, message = storage.prepare(request)
        self.assertEqual(resumed["registry"], str(target))
        self.assertEqual(resumed["revision"], existing["revision"])
        self.assertEqual(resumed["draft"], existing["draft"])
        self.assertIn("Recovered your existing shared setup", message)
        self.assertFalse((self.registry / "setup-location.json").exists())
        self.assertEqual(read_json(self.registry / "registry.json"), ledger)
        self.assertEqual(read_json(target / "retained-history.json"), {"keep": True})

    def test_missing_experiment_history_explains_the_recording_and_full_path(self):
        self.save()
        directory = self.root / "missing-old-guid"
        write_json(
            self.registry / "registry.json",
            {
                "schema": 1,
                "reservations": {},
                "workflows": {
                    "old-run": {
                        "game": "/recordings/old-game",
                        "directory": str(directory),
                    }
                },
            },
        )
        with self.assertRaisesRegex(
            WorkflowError, "history is missing for old-game"
        ) as raised:
            execution.idle(self.registry)
        self.assertIn(str(directory / "owner.json"), str(raised.exception))
        self.assertFalse(directory.exists())

    def test_shared_recovery_rechecks_relocation_under_destination_lock(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])
        existing = setup.SetupStore(target).save(
            {**self.draft, "registry": str(target)}, None
        )

        @contextmanager
        def relocating_lock(path, **kwargs):
            if (
                Path(path) == target / "setup.lock"
                and not (target / "setup-location.json").exists()
            ):
                write_json(
                    target / "setup-location.json",
                    {
                        "source_revision": existing["revision"],
                        "registry": str(self.registry),
                    },
                )
            with locked(path, **kwargs):
                yield

        with patch.object(
            storage,
            "check",
            side_effect=lambda path, *args: ["stubby"] if path == self.registry else [],
        ), patch.object(storage, "locked", relocating_lock):
            resumed, _ = storage.prepare(request)
        self.assertNotEqual(resumed["registry"], str(target))
        self.assertTrue((target / "setup-location.json").exists())

    def test_remote_build_refuses_a_draft_relocated_after_recovery(self):
        request = self.storage_request()
        target = Path(request["shared_registry"])
        setup.SetupStore(target).save({**self.draft, "registry": str(target)}, None)
        write_json(
            self.registry / "setup-location.json",
            {"source_revision": request["revision"], "registry": str(target)},
        )
        with patch.object(runtime_setup, "prepare") as prepare:
            with self.assertRaisesRegex(WorkflowError, "setup moved"):
                execution.execute(
                    {**request, "on_server": True, "challenge": "fixture"}
                )
        prepare.assert_not_called()

    def test_relocation_keeps_active_controller_and_unfinished_test_state(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        with patch.object(storage, "check", side_effect=[["stubby"], []]):
            with locked(self.registry / "controller-service.lock"):
                with self.assertRaisesRegex(WorkflowError, "Stop the controller"):
                    storage.prepare(request)
        self.assertFalse((self.registry / "setup-location.json").exists())
        write_json(self.registry / "setup-build.json", {"generation": "fixture"})
        setup.SetupStore(Path(request["shared_registry"])).save(
            {**self.draft, "registry": request["shared_registry"]}, None
        )
        with patch.object(storage, "check", side_effect=[["stubby"], []]), patch.object(
            execution,
            "generation",
            return_value=(self.root, {"tests": {"gpu/stubby": {}}}),
        ), patch.object(
            execution, "remote", return_value={"tests": [{"finished": False}]}
        ) as remote:
            with self.assertRaisesRegex(WorkflowError, "unfinished sample"):
                storage.prepare(request)
        self.assertFalse(remote.call_args.args[0]["tick"])
        self.assertFalse((self.registry / "setup-location.json").exists())

    def test_delayed_relocation_cannot_replace_recovered_edits(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])

        def concurrent_move(root, *args):
            if root == self.registry:
                return ["stubby"]
            moved = setup.SetupStore(target).save(
                {**self.draft, "registry": str(target), "server": "edited-server"}, None
            )
            write_json(
                self.registry / "setup-location.json",
                {"source_revision": request["revision"], "registry": str(target)},
            )
            return []

        with patch.object(storage, "check", side_effect=concurrent_move):
            with self.assertRaisesRegex(WorkflowError, "Setup moved"):
                storage.prepare(request)
        self.assertEqual(
            storage.recovered(self.registry)["draft"]["server"], "edited-server"
        )
        self.assertEqual(list(self.root.glob("shared-registry-*")), [])

    def test_relocation_never_reuses_a_destination_that_redirects_back(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])
        existing = setup.SetupStore(target).save(
            {**self.draft, "registry": str(target)}, None
        )
        pointer = {
            "source_revision": existing["revision"],
            "registry": str(self.registry),
        }
        write_json(target / "setup-location.json", pointer)
        with patch.object(storage, "check", side_effect=[["stubby"], [], []]):
            moved, _ = storage.prepare(request)
        self.assertNotEqual(moved["registry"], str(target))
        self.assertEqual(storage.recovered(self.registry)["draft"], moved["draft"])
        self.assertEqual(setup.SetupStore(target).read(), existing)
        self.assertEqual(read_json(target / "setup-location.json"), pointer)

    def test_relocation_requires_legacy_controller_retirement_on_its_host(self):
        from hstream_workflow import setup_storage as storage
        from hstream_workflow.processes import host_identity, process_identity

        request = self.storage_request()
        identity = {**host_identity(), **process_identity(os.getpid())}
        heartbeat = self.registry / "heartbeat.json"

        def remote(argv, timeout):
            self.assertEqual(argv[-2], self.draft["server"])
            return subprocess.check_output(
                shlex.split(argv[-1]), text=True, timeout=timeout
            )

        for changed in ({}, {"host": "another-host"}):
            write_json(heartbeat, {**identity, **changed})
            with patch.object(
                storage, "check", side_effect=[["stubby"], []]
            ), patch.object(storage, "run_readonly", side_effect=remote):
                with self.assertRaisesRegex(WorkflowError, "Stop the previous"):
                    storage.prepare(request)
            self.assertFalse((self.registry / "setup-location.json").exists())
        write_json(heartbeat, {**identity, "boot_id": "retired-boot"})
        with patch.object(storage, "check", side_effect=[["stubby"], []]), patch.object(
            storage, "run_readonly", side_effect=remote
        ):
            moved, _ = storage.prepare(request)
        self.assertEqual(moved["registry"], request["shared_registry"])

    def test_recovered_setup_locations_reject_cycles(self):
        from hstream_workflow import setup_storage as storage

        request = self.storage_request()
        target = Path(request["shared_registry"])
        other = setup.SetupStore(target).save(
            {**self.draft, "registry": str(target)}, None
        )
        write_json(
            self.registry / "setup-location.json",
            {"source_revision": request["revision"], "registry": str(target)},
        )
        write_json(
            target / "setup-location.json",
            {"source_revision": other["revision"], "registry": str(self.registry)},
        )
        with self.assertRaisesRegex(WorkflowError, "loop"):
            storage.recovered(self.registry)

    def installed_fixture(self, name):
        root = self.root / name
        (root / "bin").mkdir(parents=True)
        header = bytearray(20)
        header[:4] = b"\x7fELF"
        header[5] = 1
        header[18:20] = (62).to_bytes(2, "little")
        for program in (
            "hstream-runtime-worker",
            "hstream-experiment-task",
            "hstream-cli",
            "hstream-job",
        ):
            target = root / "bin" / program
            target.write_bytes(header + name.encode())
            target.chmod(0o755)
        (root / "configs").mkdir()
        (root / "configs/ds_hockey_app_config.yaml").write_text("settings: {}")
        (root / "lib").mkdir()
        (root / "lib/library.so.1").write_bytes(b"library")
        (root / "lib/library.so").symlink_to("library.so.1")
        return root

    def test_missing_installation_recovers_from_path_and_snapshots_package_only(self):
        installation = self.installed_fixture("installed")
        owned = list(installation.rglob("*"))
        (installation / "unowned-backup").write_text("not package data")
        paths = {
            name: str(installation / "bin" / name)
            for name in ("hstream-runtime-worker", "hstream-experiment-task")
        }
        with patch.object(
            runtime_setup.shutil, "which", side_effect=paths.get
        ), patch.object(runtime_setup, "payload_paths", return_value=owned):
            first = runtime_setup.snapshot(
                self.registry, str(self.root / "deleted-preview"), cpu=False
            )
            second = runtime_setup.snapshot(self.registry, str(installation), cpu=False)
        self.assertEqual(first, second)
        self.assertEqual(first["source"], str(installation))
        target = Path(first["runtime"]["root"])
        self.assertFalse((target / "unowned-backup").exists())
        self.assertEqual((target / "lib/library.so").read_bytes(), b"library")
        self.assertTrue(os.access(first["task"], os.X_OK))
        self.assertFalse(
            os.access(target / "configs/ds_hockey_app_config.yaml", os.X_OK)
        )
        self.assertTrue(
            all(Path(path).is_relative_to(target) for path in first["runtime"]["files"])
        )
        # A damaged retained version is never overwritten; the replacement is reusable.
        Path(first["task"]).write_bytes(b"damaged")
        with patch.object(runtime_setup, "payload_paths", return_value=owned):
            repaired = runtime_setup.snapshot(
                self.registry, str(installation), cpu=False
            )
            repeated = runtime_setup.snapshot(
                self.registry, str(installation), cpu=False
            )
        self.assertNotEqual(repaired["task"], first["task"])
        self.assertEqual(repeated, repaired)
        self.assertEqual(Path(first["task"]).read_bytes(), b"damaged")

    @unittest.skipUnless(shutil.which("cc"), "C compiler is unavailable")
    def test_prepared_task_loads_retained_library_instead_of_deployed_prefix(self):
        from hstream_workflow import executor
        from hstream_workflow.controller import Controller

        installation = self.installed_fixture("installed")
        (installation / "lib/gst-plugins").mkdir()
        (installation / "lib/gst-plugins/plugin.so").write_bytes(b"plugin")
        library = self.root / "library.c"
        library.write_text("int value(void) { return 17; }")
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                str(library),
                "-o",
                str(installation / "lib/libretained.so"),
            ],
            check=True,
        )
        external = self.root / "vendor"
        external.mkdir()
        library.write_text("int external(void) { return 0; }")
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                str(library),
                "-o",
                str(external / "libexternal.so"),
            ],
            check=True,
        )
        program = self.root / "task.c"
        program.write_text(r"""#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern int value(void);
extern int external(void);
int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--admission") == 0) {
    printf("{\"value\": %d, \"versioned_plugins\": %d}", value() + external(), getenv("GST_PLUGIN_PATH_1_0") != NULL);
    return 0;
  }
  return value() + external();
}
""")
        subprocess.run(
            [
                "cc",
                str(program),
                "-L" + str(installation / "lib"),
                "-lretained",
                "-L" + str(external),
                "-lexternal",
                "-Wl,-rpath," + str(installation / "lib"),
                "-o",
                str(installation / "bin/hstream-experiment-task"),
            ],
            check=True,
        )
        prepared = runtime_setup.snapshot(self.registry, str(installation), cpu=False)
        # A subsequent deployment changes the absolute prefix embedded in ELF.
        library.write_text("int value(void) { return 23; }")
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                str(library),
                "-o",
                str(installation / "lib/libretained.so"),
            ],
            check=True,
        )
        with patch.dict(
            os.environ,
            {
                "LD_LIBRARY_PATH": str(external) + ":" + str(installation / "lib"),
                "GST_PLUGIN_PATH_1_0": "/unrelated/plugins",
                "HSTREAM_SETUP_TEST_SECRET": "never write to a worker request",
            },
        ):
            self.assertEqual(
                subprocess.run([prepared["task"]], check=False).returncode, 23
            )
            profile = self.compile()["profiles"]["gpu"]
            profile.update(runtime=prepared["runtime"], task_command=[prepared["task"]])
            attempt = self.root / "attempt"
            attempt.mkdir(mode=0o700)
            for name in ("request.json", "profile.json", "settings.json"):
                write_json(attempt / name, {})
            request = {
                "attempt_id": "retained",
                "workflow_id": "workflow",
                "epoch": 1,
                "runtime": {},
            }
            executor.prepare(
                attempt, request, profile, self.defaults, {"attempt_id": "retained"}
            )
            spec = read_json(attempt / "worker/request.json")["batch"]["spec"]
            child_environment = dict(os.environ)
            for key in spec["unset_environment"]:
                child_environment.pop(key, None)
            child_environment.update(spec["environment"])
            result = subprocess.run(spec["argv"], env=child_environment, check=False)
            self.assertEqual(result.returncode, 17)
            profile["executor"] = "local"
            profile["resources"]["gpus"] = 0
            profile["runtime"]["storage_namespace"] = "retained"
            controller = Controller.__new__(Controller)
            controller.config = {
                "admission_profile": "metadata",
                "profiles": {"metadata": profile},
            }
            controller.defaults = self.defaults
            self.assertIn("GST_PLUGIN_PATH_1_0", spec["unset_environment"])
            self.assertEqual(
                controller._native(attempt, "inspect", {}),
                {"value": 17, "versioned_plugins": 0},
            )
            self.assertNotIn("HSTREAM_SETUP_TEST_SECRET", spec["environment"])

    def test_interrupted_copy_never_publishes_a_runtime(self):
        installation = self.installed_fixture("installed")
        with patch.object(
            runtime_setup.shutil, "copyfileobj", side_effect=OSError("copy interrupted")
        ):
            with self.assertRaisesRegex(OSError, "interrupted"):
                runtime_setup.snapshot(self.registry, str(installation), cpu=False)
        self.assertFalse(
            any(path.is_dir() for path in (self.registry / "setup-runtimes").iterdir())
        )

    def test_preparation_groups_builds_and_preserves_logical_capacity(self):
        self.save()
        self.draft["profiles"]["all-gpus"] = copy.deepcopy(
            self.draft["profiles"]["gpu"]
        )
        self.draft["profiles"]["all-gpus"]["concurrency"] = 8
        self.draft["profiles"]["all-gpus"]["nodes"].append("monster")

        def installed(root, cpu):
            task, runtime = self.inspect(root, cpu=cpu)
            return {"source": "/opt/hstream", "task": task, "runtime": runtime}

        def node(registry, name, preferred, timeout):
            return installed(
                "/shared/old-os" if name == "monster" else "/shared/new-os", False
            )

        with patch.object(
            runtime_setup, "snapshot", return_value=installed("/shared/cpu", True)
        ), patch.object(runtime_setup, "from_node", side_effect=node):
            draft, prepared, messages = runtime_setup.prepare(
                self.registry, self.draft, 30, 3
            )
        self.assertEqual(self.draft["profiles"]["all-gpus"]["concurrency"], 8)
        self.assertEqual(draft["profiles"]["all-gpus"]["concurrency"], 3)
        self.assertEqual(draft["profiles"]["gpu"]["runtime_root"], "/opt/hstream")
        config = execution.compile_configuration(
            draft, self.defaults, self.cpu, prepared=prepared
        )
        profiles = [
            profile for key, profile in config["profiles"].items() if key != "metadata"
        ]
        self.assertEqual(len(profiles), 3)
        self.assertEqual(len({profile["pool"] for profile in profiles}), 1)
        self.assertEqual(
            {node for profile in profiles for node in profile["resources"]["nodes"]},
            {"mini", "meshy", "monster"},
        )
        for profile in profiles:
            for node in profile["resources"]["nodes"]:
                self.assertEqual(
                    profile["qualified_nodes"][node]["runtime_identity"],
                    profile["runtime"],
                )
                self.assertEqual(
                    profile["runtime"]["root"],
                    "/shared/old-os" if node == "monster" else "/shared/new-os",
                )
        self.assertTrue(messages)
        write_json(self.registry / "config.json", config)
        self.assertEqual(load_config(self.registry), config)

    def test_repaired_draft_commits_only_after_validated_build(self):
        saved = self.save()
        repaired = copy.deepcopy(self.draft)
        repaired["profiles"]["gpu"]["runtime_root"] = "/opt/hstream"
        request = {
            "revision": saved["revision"],
            "defaults": self.defaults,
            "metadata_resources": self.cpu,
        }
        with patch.object(
            execution,
            "compile_configuration",
            side_effect=WorkflowError("invalid configuration"),
        ):
            with self.assertRaisesRegex(WorkflowError, "invalid configuration"):
                execution.build(
                    self.registry, request, prepared={}, repaired_draft=repaired
                )
        self.assertEqual(setup.SetupStore(self.registry).read(), saved)
        with patch.object(
            execution, "compile_configuration", return_value=self.compile()
        ):
            result = execution.build(
                self.registry, request, prepared={}, repaired_draft=repaired
            )
        self.assertEqual(result["draft"], repaired)
        self.assertEqual(result["revision"], digest(repaired))
        self.assertTrue(setup.progress(self.registry)["build_current"])

    def test_concurrent_edit_wins_over_runtime_preparation(self):
        saved = self.save()
        changed = copy.deepcopy(self.draft)
        changed["server"] = "another-server"
        configuration = self.compile()

        def compile_after_edit(*args, **kwargs):
            setup.SetupStore(self.registry).save(changed, saved["revision"])
            return configuration

        with patch.object(
            execution, "compile_configuration", side_effect=compile_after_edit
        ):
            with self.assertRaisesRegex(WorkflowError, "another window"):
                execution.build(
                    self.registry,
                    {
                        "revision": saved["revision"],
                        "defaults": self.defaults,
                        "metadata_resources": self.cpu,
                    },
                    prepared={},
                    repaired_draft=self.draft,
                )
        self.assertEqual(setup.SetupStore(self.registry).read()["draft"], changed)
        self.assertFalse((self.registry / "setup-build.json").exists())

    def test_build_error_identifies_the_profile_with_a_missing_compute_installation(
        self,
    ):
        self.draft["profiles"]["gpu"]["runtime_root"] = str(self.root / "missing-gpu")

        def inspect(root, *, cpu):
            return (
                self.inspect(root, cpu=True)
                if cpu
                else execution.inventory(root, cpu=False)
            )

        with self.assertRaises(WorkflowError) as raised:
            execution.compile_configuration(
                self.draft, self.defaults, self.cpu, inspect
            )
        self.assertIn("Fast computers:", str(raised.exception))
        self.assertIn("Advanced profile settings", str(raised.exception))

    def test_save_response_cannot_borrow_a_concurrent_writers_revision(self):
        second = copy.deepcopy(self.draft)
        second["profiles"]["gpu"]["label"] = "Another window"

        @contextmanager
        def competing_save(path):
            with locked(path):
                yield
            with locked(path):
                write_json(self.registry / "setup.json", second)

        with patch.object(setup, "locked", competing_save):
            first = self.save()
        self.assertEqual(first["draft"], self.draft)
        self.assertEqual(first["revision"], digest(self.draft))
        self.assertEqual(setup.SetupStore(self.registry).read()["draft"], second)
        with self.assertRaisesRegex(WorkflowError, "another window"):
            setup.SetupStore(self.registry).save(self.draft, first["revision"])

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
        progress = setup.handle_request(
            {"action": "load", "registry": str(self.registry)}
        )["progress"]
        self.assertTrue(progress["has_build"])
        self.assertTrue(progress["build_current"])
        self.assertFalse(progress["applied"])
        self.assertFalse(progress["installed"])
        self.assertFalse(status["tested"])
        self.assertEqual([row["node"] for row in status["tests"]], ["mini", "meshy"])
        with self.assertRaisesRegex(WorkflowError, "Every computer"):
            execution.activate(self.registry)
        self.assertEqual(
            execution.generation(self.registry)[0].name, result["generation"]
        )
        changed = copy.deepcopy(self.draft)
        changed["profiles"]["gpu"]["label"] = "Edited after building"
        setup.SetupStore(self.registry).save(changed, saved["revision"])
        self.assertFalse(setup.progress(self.registry)["build_current"])

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

    def test_interrupted_service_creation_can_retry_without_manual_removal(self):
        self.save()
        config = self.compile()
        write_json(self.registry / "config.json", config)
        write_json(
            self.registry / "setup-active.json",
            {"configuration_revision": digest(config)},
        )
        home = self.root / "service-home"
        with patch.object(Path, "home", return_value=home), patch.object(
            execution, "run_readonly"
        ):
            with patch.object(
                execution.os, "fsync", side_effect=OSError("interrupted unit")
            ):
                with self.assertRaisesRegex(OSError, "interrupted unit"):
                    execution.start_service(self.registry, 30)
            self.assertFalse(list(home.rglob("*.service")))
            result = execution.start_service(self.registry, 30)
            unit = home / ".config/systemd/user" / result["service"]
            self.assertIn("ExecStart=", unit.read_text())
            execution.stop_service(self.registry, 30)
            self.assertFalse(unit.exists())

    def test_definitely_rejected_test_can_be_stopped_corrected_and_rebuilt(self):
        self.draft["profiles"]["gpu"]["time_seconds"] = 30 * 24 * 3600
        saved = self.save()
        game = self.root / "recordings/sample"
        game.mkdir(parents=True)
        request = {
            "revision": saved["revision"],
            "defaults": self.defaults,
            "metadata_resources": self.cpu,
        }
        with patch.object(
            execution, "compile_configuration", return_value=self.compile()
        ):
            execution.build(self.registry, request)
        result = execution.start_test(
            self.registry,
            {
                "profile": "gpu",
                "node": "mini",
                "game": str(game),
                "scan_seconds": 60,
                "user_config_snapshot": {"schema": 1, "source_path": "", "values": {}},
            },
        )
        rejected = result["tests"][0]
        self.assertEqual(rejected["state"], "rejected")
        self.assertTrue(rejected["finished"])
        self.assertFalse(rejected["passed"])
        self.assertIn("30 days", rejected["log"])
        execution.execute(
            {
                "registry": str(self.registry),
                "action": "cancel-test",
                "challenge": "test",
            }
        )
        self.draft["profiles"]["gpu"]["time_seconds"] = 1800
        saved = setup.SetupStore(self.registry).save(self.draft, saved["revision"])
        request["revision"] = saved["revision"]
        with patch.object(
            execution, "compile_configuration", return_value=self.compile()
        ):
            execution.build(self.registry, request)
        self.assertEqual(
            execution.qualification_status(self.registry, tick=False)["tests"][0][
                "state"
            ],
            "not-tested",
        )

    def test_ambiguous_registration_never_becomes_a_terminal_rejection(self):
        saved = self.save()
        game = self.root / "recordings/sample"
        game.mkdir(parents=True)
        with patch.object(
            execution, "compile_configuration", return_value=self.compile()
        ):
            execution.build(
                self.registry,
                {
                    "revision": saved["revision"],
                    "defaults": self.defaults,
                    "metadata_resources": self.cpu,
                },
            )
        with patch(
            "hstream_workflow.controller.Controller.register",
            side_effect=WorkflowError("Response lost"),
        ):
            with self.assertRaisesRegex(WorkflowError, "Response lost"):
                execution.start_test(
                    self.registry,
                    {
                        "profile": "gpu",
                        "node": "mini",
                        "game": str(game),
                        "scan_seconds": 60,
                        "user_config_snapshot": {
                            "schema": 1,
                            "source_path": "",
                            "values": {},
                        },
                    },
                )
            with self.assertRaisesRegex(WorkflowError, "Response lost"):
                execution.qualification_status(self.registry, tick=False)
        self.assertFalse(list(self.registry.rglob("rejected.json")))

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

    def test_rebuilding_changed_programs_at_the_same_paths_needs_apply_again(self):
        saved = self.save()
        original = self.compile()
        updated = copy.deepcopy(original)
        for profile in updated["profiles"].values():
            runtime = profile["runtime"]
            runtime["files"][runtime["worker"]] = "c" * 64
            for node in profile["qualified_nodes"].values():
                node["runtime_identity"] = copy.deepcopy(runtime)

        def build_config(config):
            with patch.object(execution, "compile_configuration", return_value=config):
                execution.build(
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
            build_config(original)
            execution.activate(self.registry)
            self.assertTrue(setup.progress(self.registry)["applied"])
            build_config(original)
            self.assertTrue(setup.progress(self.registry)["applied"])
            build_config(updated)
            progress = setup.progress(self.registry)
            self.assertEqual(progress["revision"], saved["revision"])
            self.assertTrue(progress["build_current"])
            self.assertFalse(progress["applied"])
            execution.activate(self.registry)
            self.assertTrue(setup.progress(self.registry)["applied"])

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

    def test_readiness_preserves_qualified_installation_aliases(self):
        self.save()
        config = self.compile()
        write_json(self.registry / "config.json", config)
        write_json(
            self.registry / "heartbeat.json",
            {
                "host": "another-host",
                "time_ns": time.time_ns(),
                "configuration_digest": digest(config),
            },
        )
        for path in ("/installed/cpu/", "/installation-alias"):
            draft = copy.deepcopy(self.draft)
            draft["controller_runtime"] = path
            draft["profiles"]["gpu"]["runtime_root"] = "/installed/gpu/"
            if path.endswith("/"):
                self.assertTrue(
                    setup.inspect_registry(self.registry, 30, draft)["ready"]
                )
            write_json(
                self.registry / "setup-active.json",
                {"revision": digest(draft), "configuration_revision": digest(config)},
            )
            self.assertTrue(setup.inspect_registry(self.registry, 30, draft)["ready"])
            draft["profiles"]["gpu"]["cpus"] += 1
            self.assertFalse(setup.inspect_registry(self.registry, 30, draft)["ready"])

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
        self.assertEqual(first["tests"][0]["game"], str(fixture.game))
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
