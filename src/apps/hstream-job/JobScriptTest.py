#!/usr/bin/env python3
"""Execute exported jobs against a recording CLI, without a GPU or Slurm."""
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())


class JobScriptTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / "games" / "a 'quoted' $(touch INJECTED) game"
        self.game.mkdir(parents=True)
        self.output = self.root / "observed.json"
        self.runner = self.root / "fake 'hstream-cli'"
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport json, os, sys\n"
            "with open(os.environ['JOB_TEST_OUTPUT'], 'w') as f:\n"
            " json.dump({'argv':sys.argv[1:], 'cwd':os.getcwd(), 'env':dict(os.environ)}, f)\n"
            "sys.exit(7)\n"
        )
        self.runner.chmod(0o700)
        self.config = self.root / "pipeline config.yaml"
        self.config.write_text("pipeline: {}\n")
        self.arguments = ["--enable-sinks=ENCODE_FILE", "--options=key=a 'quote' $(touch INJECTED)\nnext",
                          "--stitch-frame-time=00:00:17"]
        # JSON is valid YAML, and avoids requiring PyYAML in the test environment.
        (self.game / "config.yaml").write_text(json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}}))
        self.command = [TOOL, "--game-dir", str(self.game), "--runner", str(self.runner),
                        "--config", str(self.config), "--working-directory", str(self.root)]
        self.env = dict(os.environ, JOB_TEST_OUTPUT=str(self.output), HSTREAM_UI_PARENT_PID="999999",
                        HM_OUTPUT_WORK_DIR=str(self.root / "work 'quoted'"))

    def generate(self, *extra):
        return subprocess.run(self.command + list(extra), env=self.env, text=True, capture_output=True)

    def test_direct_execution_preserves_arguments_and_exit_code(self):
        result = self.generate("--sbatch", "--partition=gpu", "--sbatch", "--time=02:00:00")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists(), "export must not launch the job")
        script = self.game / "hstream-job.sh"
        self.assertTrue(os.access(script, os.X_OK))
        text = script.read_text()
        self.assertIn("#SBATCH --gres=gpu:1\n", text)
        self.assertIn("#SBATCH --partition=gpu\n", text)
        self.assertNotIn("srun", text)
        subprocess.run(["bash", "-n", str(script)], check=True)
        run = subprocess.run([str(script)], cwd="/", env=self.env)
        self.assertEqual(run.returncode, 7)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"], ["-g", self.game.name, "-c", str(self.config),
                                         "--enable-sources=URI-MULTIPLE"] + self.arguments)
        self.assertEqual(actual["cwd"], str(self.root))
        self.assertEqual(actual["env"]["HM_GAME_DIR"], str(self.game.parent))
        self.assertEqual(actual["env"]["HM_OUTPUT_WORK_DIR"], self.env["HM_OUTPUT_WORK_DIR"])
        self.assertNotIn("HSTREAM_UI_PARENT_PID", actual["env"])
        self.assertFalse((self.root / "INJECTED").exists())

    def test_symlinked_game_keeps_its_logical_id(self):
        alias = self.root / "linked-game"
        alias.symlink_to(self.game, target_is_directory=True)
        result = self.generate("--game-dir", str(alias))
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([str(alias / "hstream-job.sh")], env=self.env)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"][:2], ["-g", "linked-game"])
        self.assertEqual(actual["env"]["HM_GAME_DIR"], str(self.root))

    def test_overwrite_and_invalid_directives(self):
        self.assertEqual(self.generate().returncode, 0)
        script = self.game / "hstream-job.sh"
        original = script.read_bytes()
        self.assertNotEqual(self.generate().returncode, 0)
        self.assertEqual(script.read_bytes(), original)
        self.assertEqual(self.generate("--force").returncode, 0)
        self.assertNotEqual(self.generate("--force", "--sbatch", "--partition=gpu\ntouch INJECTED").returncode, 0)
        self.assertEqual(script.read_bytes(), original)

    def test_installed_and_bazel_path_discovery(self):
        for installed in (True, False):
            layout = self.root / ("installed" if installed else "workspace")
            bin_dir = layout / "bin" if installed else layout / "bin/src/apps/hstream-job"
            bin_dir.mkdir(parents=True)
            tool = bin_dir / "hstream-job"
            shutil.copy2(TOOL, tool)
            runner = bin_dir / "hstream-cli" if installed else bin_dir.parent / "pipeline-app/hstream-cli"
            runner.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.runner, runner)
            config = layout / "configs/ds_hockey_app_config.yaml"
            config.parent.mkdir(parents=True)
            config.write_text("pipeline: {}\n")
            if not installed:
                runfile = Path(str(tool) + ".runfiles/kstream/configs/ds_hockey_app_config.yaml")
                runfile.parent.mkdir(parents=True)
                runfile.symlink_to(config)
            env = dict(self.env)
            env.pop("BUILD_WORKSPACE_DIRECTORY", None)
            result = subprocess.run([str(tool), "--game-dir", str(self.game), "--force"],
                                    cwd="/", env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(self.game / "hstream-job.sh")], cwd="/", env=env)
            actual = json.loads(self.output.read_text())
            self.assertEqual(actual["cwd"], str(layout))
            self.assertIn(str(config), actual["argv"])

    def test_plain_config_and_custom_output(self):
        (self.game / "config.yaml").write_text("pipeline: {}\n")
        custom = self.root / "custom.sh"
        result = self.generate("--output", str(custom))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(custom.exists())
        self.assertNotIn("--enable-sinks", custom.read_text())
        (self.game / "config.yaml").write_text("[invalid")
        self.assertNotEqual(self.generate().returncode, 0)


if __name__ == "__main__":
    unittest.main()
