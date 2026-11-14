"""Exercise checkout clients against a separate pinned controller installation."""

import copy
import json
import os
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch

import hstream_workflow_controller_test as fixtures
from hstream_workflow.dispatch import installed_launcher
from hstream_workflow.files import file_digest, read_json, write_json
from hstream_workflow.profiles import controller_files
from hstream_workflow.schema import WorkflowError


class DispatchTest(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ControllerTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.root = self.fixture.root
        self.installed = self.copy_controller("installed")
        self.client = self.copy_controller("checkout")
        # A newer client may contain modules absent from the installed release.
        (self.client / "hstream_workflow/client_only.py").write_text(
            "# new client module\n"
        )
        pins = self.fixture.profile["runtime"]["files"]
        for path in controller_files():
            pins.pop(str(path))
        pins.update(self.controller_pins(self.installed))
        self.config = read_json(self.fixture.registry / "config.json")
        self.config["profiles"]["cpu"] = self.fixture.profile
        self.save_config()
        self.plan_path = self.root / "plan.json"
        write_json(self.plan_path, self.fixture.plan)

    def copy_controller(self, name):
        root = self.root / name
        (root / "hstream_workflow").mkdir(parents=True, mode=0o700)
        for path in controller_files():
            destination = (
                root / path.name
                if path.name == "hstream-workflow"
                else root / "hstream_workflow" / path.name
            )
            shutil.copy2(path, destination)
        return root

    @staticmethod
    def controller_pins(root):
        return {
            str(path): file_digest(path, 4 * 1024**2)["sha256"]
            for path in [
                root / "hstream-workflow",
                *sorted((root / "hstream_workflow").glob("*.py")),
            ]
        }

    def save_config(self):
        write_json(self.fixture.registry / "config.json", self.config)

    def call(self, *args, expected=0):
        result = subprocess.run(
            [
                sys.executable,
                str(self.client / "hstream-workflow"),
                *args,
                "--registry",
                str(self.fixture.registry),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=45,
        )
        self.assertEqual(result.returncode, expected, result.stderr)
        return json.loads(result.stdout if expected == 0 else result.stderr)

    def test_checkout_runs_and_exports_using_installed_controller(self):
        profiles = self.call("profiles")
        self.assertEqual(
            profiles["profiles"][0]["runtime"]["files"],
            self.fixture.profile["runtime"]["files"],
        )
        result = self.call("run", "--plan", str(self.plan_path))
        self.assertEqual(result["state"], "succeeded")
        self.assertFalse((self.fixture.game / "_hockeymon/test-owner.json").exists())
        script = self.root / "batch.sh"
        self.call("export", "--plan", str(self.plan_path), "--output", str(script))
        self.assertIn(str(self.installed / "hstream-workflow"), script.read_text())
        self.assertNotIn(str(self.client), script.read_text())
        # A second submission reaches the same installed controller and returns
        # the existing completion, without creating another workflow or task.
        self.assertEqual(
            self.call("submit", "--plan", str(self.plan_path))["state"], "succeeded"
        )
        self.assertEqual(len(self.fixture.controller.registry.read()["workflows"]), 1)

    def test_package_change_rejects_before_admission(self):
        with (self.installed / "hstream_workflow/controller.py").open("a") as stream:
            stream.write("\n# deployment changed\n")
        error = self.call("submit", "--plan", str(self.plan_path), expected=2)
        self.assertIn("controller changed", error["error"])
        self.assertEqual(error["submission"], "rejected")
        self.assertEqual(error["workflow_id"], self.fixture.plan["workflow_id"])
        self.assertEqual(self.fixture.controller.registry.read()["workflows"], {})
        self.assertFalse((self.fixture.game / "_hockeymon/test-owner.json").exists())

    def test_client_python_path_cannot_shadow_installed_package(self):
        with patch.dict(
            os.environ,
            {
                "PYTHONPATH": str(self.client),
                "PYTHONHOME": sys.prefix,
                "PYTHONSAFEPATH": "1",
            },
        ):
            profiles = self.call("profiles")
        self.assertEqual(
            profiles["profiles"][0]["runtime"]["files"],
            self.fixture.profile["runtime"]["files"],
        )

    def test_missing_and_extra_modules_are_rejected(self):
        path = self.installed / "hstream_workflow/schema.py"
        saved = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(WorkflowError, "incomplete or changed"):
            installed_launcher(self.config)
        path.write_bytes(saved)
        (path.parent / "unexpected.py").write_text("# unpinned module\n")
        with self.assertRaisesRegex(WorkflowError, "incomplete or changed"):
            installed_launcher(self.config)

    def test_symlinked_module_is_rejected(self):
        path = self.installed / "hstream_workflow/schema.py"
        other = self.root / "schema.py"
        path.rename(other)
        path.symlink_to(other)
        with self.assertRaises(OSError):
            installed_launcher(self.config)

    def test_mixed_profile_pins_are_rejected(self):
        profile = copy.deepcopy(self.config["profiles"]["cpu"])
        profile["runtime"]["files"][str(self.installed / "hstream-workflow")] = "f" * 64
        self.config["profiles"]["other"] = profile
        with self.assertRaisesRegex(WorkflowError, "different controller deployments"):
            installed_launcher(self.config)

    def test_ambiguous_controller_is_rejected(self):
        other = self.copy_controller("another-installation")
        self.config["profiles"]["cpu"]["runtime"]["files"].update(
            self.controller_pins(other)
        )
        with self.assertRaisesRegex(WorkflowError, "exactly one installed controller"):
            installed_launcher(self.config)

    def test_non_executable_controller_is_rejected(self):
        (self.installed / "hstream-workflow").chmod(0o600)
        with self.assertRaisesRegex(WorkflowError, "not executable"):
            installed_launcher(self.config)

    def test_compute_profile_does_not_need_controller_package(self):
        profile = copy.deepcopy(self.config["profiles"]["cpu"])
        for path in self.controller_pins(self.installed):
            profile["runtime"]["files"].pop(path)
        self.config["profiles"]["compute"] = profile
        self.assertEqual(
            installed_launcher(self.config), self.installed / "hstream-workflow"
        )


if __name__ == "__main__":
    unittest.main()
