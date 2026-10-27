#!/usr/bin/env python3
"""Execute exported jobs against a recording CLI, without a GPU or Slurm."""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOL = str(Path(sys.argv.pop(1)).resolve())
PACKAGER = Path(sys.argv.pop(1)).resolve()
PUBLICATION = Path(sys.argv.pop(1)).resolve()


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
        self.arguments = [
            "--enable-sinks=ENCODE_FILE",
            "--options=key=a 'quote' $(touch INJECTED)\nnext",
            "--stitch-frame-time=00:00:17",
        ]
        # JSON is valid YAML, and avoids requiring PyYAML in the test environment.
        (self.game / "config.yaml").write_text(
            json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}})
        )
        self.command = [
            TOOL,
            "--game-dir",
            str(self.game),
            "--runner",
            str(self.runner),
            "--config",
            str(self.config),
            "--working-directory",
            str(self.root),
        ]
        self.runtime_bin = self.root / "runtime-bin"
        self.runtime_bin.mkdir()
        (self.runtime_bin / "hstream-finalize").symlink_to(
            Path(TOOL).parent.parent / "hstream-finalize/hstream-finalize"
        )
        self.env = dict(
            os.environ,
            PATH=str(self.runtime_bin) + os.pathsep + os.environ["PATH"],
            JOB_TEST_OUTPUT=str(self.output),
            HSTREAM_UI_PARENT_PID="999999",
            HM_OUTPUT_WORK_DIR=str(self.root / "work 'quoted'"),
        )

    def generate(self, *extra):
        return subprocess.run(
            self.command + list(extra),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_all_exported_modes_keep_the_managed_marker_through_native_exec(self):
        finalizer_output = self.root / "finalizer-environment.json"
        finalizer = self.root / "recording-finalizer"
        finalizer.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            "with open(os.environ['JOB_TEST_FINALIZER_OUTPUT'],'w') as f:\n"
            " json.dump({'argv':sys.argv[1:],'env':dict(os.environ)},f)\n"
            "start=sys.argv.index('--')+1\n"
            "os.execv(sys.argv[start],sys.argv[start:])\n"
        )
        finalizer.chmod(0o700)
        self.env.update(
            HSTREAM_RUNTIME_CACHE_DIR=str(self.root / "private-runtime"),
            HSTREAM_JOB_MANAGED_RUNTIME="stale-parent-value",
            JOB_TEST_FINALIZER_OUTPUT=str(finalizer_output),
        )
        for mode in (None, "--interactive", "--runtime-only", "--interactive-publication"):
            with self.subTest(mode=mode or "unattended-encode"):
                self.output.unlink(missing_ok=True)
                finalizer_output.unlink(missing_ok=True)
                options = ["--force", "--finalizer", str(finalizer)]
                if mode:
                    options.append(mode)
                exported = self.generate(*options)
                self.assertEqual(exported.returncode, 0, exported.stderr)
                extra = ["--tensorrt-runtime-info"] if mode == "--runtime-only" else []
                run = subprocess.run([str(self.game / "hstream-job.sh"), *extra], env=self.env,
                                     text=True, capture_output=True)
                self.assertEqual(run.returncode, 7, run.stderr)
                actual = json.loads(self.output.read_text())
                self.assertEqual(actual["env"]["HSTREAM_JOB_MANAGED_RUNTIME"], "1")
                if mode == "--runtime-only":
                    self.assertEqual(actual["argv"], extra)
                elif mode is None:
                    self.assertIn("--encode-job", actual["argv"])
                else:
                    self.assertNotIn("--encode-job", actual["argv"])
                if mode in (None, "--interactive-publication"):
                    admitted = json.loads(finalizer_output.read_text())
                    self.assertEqual(admitted["env"]["HSTREAM_JOB_MANAGED_RUNTIME"], "1")
                else:
                    self.assertFalse(finalizer_output.exists())

    def test_supervisor_run_identity_reaches_the_native_finalizer(self):
        identity = "admitted-run_123"
        exported = self.generate("--run-id", identity)
        self.assertEqual(exported.returncode, 0, exported.stderr)
        self.assertFalse(self.output.exists())
        run = subprocess.run(
            [str(self.game / "hstream-job.sh")], env=self.env, text=True, capture_output=True
        )
        self.assertEqual(run.returncode, 7, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["env"]["HSTREAM_ARCHIVE_RUN_ID"], identity)
        self.assertTrue((self.game / f"hstream-job-{identity}.yaml").is_file())
        self.assertEqual(len(list(self.game.glob("hstream-job-*.yaml"))), 1)

    def test_invalid_or_unmanaged_run_identity_cannot_publish_a_script(self):
        for identity in ("", "short", "x" * 101, "../outside", "unsafe/run", "new\nline", "$(touch INJECTED)"):
            with self.subTest(identity=identity):
                self.assertNotEqual(self.generate("--run-id", identity).returncode, 0)
                self.assertFalse((self.game / "hstream-job.sh").exists())
        for mode in ("--interactive", "--runtime-only"):
            self.assertNotEqual(self.generate(mode, "--run-id", "admitted-run_123").returncode, 0)
            self.assertFalse((self.game / "hstream-job.sh").exists())
        self.assertNotEqual(self.generate("--run-id", "first-run-id", "--run-id", "second-run-id").returncode, 0)
        self.assertFalse((self.game / "hstream-job.sh").exists())
        self.assertFalse(self.output.exists())
        self.assertFalse((self.root / "INJECTED").exists())

    def test_direct_execution_preserves_arguments_and_exit_code(self):
        result = self.generate(
            "--sbatch", "--partition=gpu", "--sbatch", "--time=02:00:00"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.output.exists(), "export must not launch the job")
        script = self.game / "hstream-job.sh"
        self.assertTrue(os.access(script, os.X_OK))
        text = script.read_text()
        self.assertIn("#SBATCH --gres=gpu:1\n", text)
        self.assertIn("#SBATCH --partition=gpu\n", text)
        self.assertNotIn("srun", text)
        subprocess.run(["bash", "-n", str(script)], check=True)
        run = subprocess.run(
            [str(script)],
            cwd="/",
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertIn(
            "Publishing completed outputs to",
            run.stdout,
        )
        self.assertIn(str(self.game), run.stdout)
        self.assertEqual(run.returncode, 7)
        actual = json.loads(self.output.read_text())
        self.assertEqual(
            actual["argv"],
            [
                "-g",
                self.game.name,
                "-c",
                str(self.config),
                "--enable-sources=URI-MULTIPLE",
            ]
            + self.arguments[1:]
            + ["--enable-sinks=ENCODE_FILE", "--encode-job"],
        )
        self.assertEqual(actual["cwd"], str(self.root))
        self.assertEqual(actual["env"]["HM_GAME_DIR"], str(self.game.parent))
        self.assertEqual(
            actual["env"]["HM_OUTPUT_WORK_DIR"], self.env["HM_OUTPUT_WORK_DIR"]
        )
        self.assertNotIn("HSTREAM_UI_PARENT_PID", actual["env"])
        self.assertFalse((self.root / "INJECTED").exists())

    def test_managed_calibration_prompts_are_explicit_and_keep_finalization(self):
        for enabled in (False, True):
            for flows in ({}, {"HSTREAM_RINK_LEVELING_FLOW": "1", "HSTREAM_PROJECTION_CROP_FLOW": "0"}):
                with self.subTest(enabled=enabled, flows=flows):
                    env = dict(self.env)
                    for key in ("HSTREAM_RINK_LEVELING_FLOW", "HSTREAM_PROJECTION_CROP_FLOW"):
                        env.pop(key, None)
                    env.update(flows)
                    env.update(HSTREAM_CALIBRATION_PENDING="1", HSTREAM_CALIBRATION_START_STAGE="features", HSTREAM_CALIBRATION_INVALIDATION_ID="stale")
                    result = self.generate("--force", *(["--calibration-prompts"] if enabled else []))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = subprocess.run([str(self.game / "hstream-job.sh")], env=env, capture_output=True, text=True)
                    self.assertEqual(run.returncode, 7, run.stderr)
                    self.assertIn("Publishing completed outputs", run.stdout)
                    actual = json.loads(self.output.read_text())["env"]
                    for key in ("HSTREAM_RINK_LEVELING_FLOW", "HSTREAM_PROJECTION_CROP_FLOW"):
                        self.assertEqual(actual.get(key), flows.get(key) if enabled else None)
                    for key in ("HSTREAM_CALIBRATION_PENDING", "HSTREAM_CALIBRATION_START_STAGE", "HSTREAM_CALIBRATION_INVALIDATION_ID"):
                        self.assertNotIn(key, actual)

    def test_interactive_preserves_protocol_sinks_and_runtime_without_finalizer(self):
        self.arguments[0] = "--enable-sinks=WEBRTC"
        (self.game / "config.yaml").write_text(json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}}))
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            "with open(os.environ['JOB_TEST_OUTPUT'],'w') as f:\n"
            " json.dump({'argv':sys.argv[1:], 'stdin':sys.stdin.readline(), 'env':dict(os.environ)},f)\n"
        )
        self.env.update(HSTREAM_RUNTIME_CACHE_DIR=str(self.root / "private runtime"), HSTREAM_RINK_LEVELING_FLOW="1", HSTREAM_CALIBRATION_PENDING="1", HSTREAM_CALIBRATION_INVALIDATION_ID="server-generation")
        result = self.generate("--interactive", "--finalizer", str(self.root / "nonexistent-finalizer"))
        self.assertEqual(result.returncode, 0, result.stderr)
        script = self.game / "hstream-job.sh"
        run = subprocess.run([str(script), "--options=pipeline.sink4.webrtc-port=20001", "literal $(touch INJECTED)"], input="inspector-session\n", env=self.env, text=True, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertIn("--enable-sinks=WEBRTC", actual["argv"])
        self.assertNotIn("--encode-job", actual["argv"])
        self.assertEqual(actual["argv"][-2:], ["--options=pipeline.sink4.webrtc-port=20001", "literal $(touch INJECTED)"])
        self.assertEqual(actual["stdin"], "inspector-session\n")
        self.assertEqual(actual["env"]["HSTREAM_RINK_LEVELING_FLOW"], "1")
        self.assertEqual(actual["env"]["HSTREAM_CALIBRATION_PENDING"], "1")
        self.assertEqual(actual["env"]["HSTREAM_CALIBRATION_INVALIDATION_ID"], "server-generation")
        # The runner owns plugin and registry discovery; the script only hands
        # it the supervisor's private cache, resolved to a real directory.
        self.assertEqual(
            actual["env"]["HSTREAM_RUNTIME_CACHE_DIR"],
            os.path.realpath(self.root / "private runtime"),
        )
        self.assertNotIn("nonexistent-finalizer", script.read_text())
        self.assertFalse((self.root / "INJECTED").exists())

    def test_interactive_publication_preserves_stdin_and_does_not_adopt_failure(self):
        self.arguments[0] = "--enable-sinks=WEBRTC,ENCODE_FILE,RTMP"
        self.arguments.append("--render-realtime")
        (self.game / "config.yaml").write_text(json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}}))
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            "with open(os.environ['JOB_TEST_OUTPUT'],'w') as f:\n"
            " json.dump({'argv':sys.argv[1:], 'stdin':sys.stdin.readline(), 'env':dict(os.environ)},f)\n"
            "sys.exit(7)\n"
        )
        exported = self.generate("--interactive-publication")
        self.assertEqual(exported.returncode, 0, exported.stderr)
        run = subprocess.run(
            [str(self.game / "hstream-job.sh"), "--options=pipeline.sink4.webrtc-port=20001", "literal $(touch INJECTED)"],
            input="q\n", env=self.env, text=True, capture_output=True,
        )
        self.assertEqual(run.returncode, 7, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"][5:5 + len(self.arguments)], self.arguments)
        self.assertEqual(actual["argv"][-2:], ["--options=pipeline.sink4.webrtc-port=20001", "literal $(touch INJECTED)"])
        self.assertEqual(actual["stdin"], "q\n")
        self.assertNotIn("--encode-job", actual["argv"])
        self.assertEqual(actual["env"]["HSTREAM_JOB_MANAGED_RUNTIME"], "1")
        manifest = Path(actual["env"]["HSTREAM_JOB_MANIFEST"])
        self.assertEqual(Path(actual["env"]["HSTREAM_RUNTIME_CACHE_DIR"]), manifest.parent / "runtime")
        self.assertFalse(list(self.game.glob("*.mp4")))
        receipts = list(self.game.glob("hstream-job-*.yaml"))
        self.assertEqual(len(receipts), 1)
        self.assertIn("runner_succeeded: false", manifest.read_text())
        self.assertNotIn("runner_succeeded: true", receipts[0].read_text())
        self.assertNotIn("state: complete", receipts[0].read_text())
        self.assertFalse((self.root / "INJECTED").exists())

    def test_interactive_publication_finishes_exact_native_manifest_after_stop(self):
        # The real publication fixture records telemetry for game ID "game".
        game = self.game.with_name("game")
        self.game.rename(game)
        self.game = game
        self.command[self.command.index("--game-dir") + 1] = str(game)
        fixture = self.root / "fixture.mkv"
        subprocess.run([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=size=64x64:rate=5",
            "-t", "0.4", "-c:v", "libx264", "-y", str(fixture),
        ], check=True)
        self.arguments[0] = "--enable-sinks=WEBRTC,ENCODE_FILE,ENCODE_PROGRAM_4K_FILE,ENCODE_STITCHED_FILE"
        (self.game / "config.yaml").write_text(json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}}))
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport subprocess,sys\n"
            "assert sys.stdin.readline() == 'q\\n'\n"
            + "subprocess.run(" + repr([str(PUBLICATION), "--produce", str(fixture)]) + ",check=True)\n"
        )
        self.assertEqual(self.generate("--interactive-publication", "--run-id", "interactive-admitted-id").returncode, 0)
        script = self.game / "hstream-job.sh"
        run = subprocess.run([str(script)], input="q\n", env=self.env, text=True, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(len(list(self.game.glob("*.mp4"))), 3)
        self.assertEqual(len(list(self.game.glob("*.db"))), 1)
        self.assertEqual(len(list(self.game.glob("*.hstream-archive.json"))), 3)
        receipts = list(self.game.glob("hstream-job-*.yaml"))
        self.assertEqual(len(receipts), 1)
        self.assertEqual(receipts[0].name, "hstream-job-interactive-admitted-id.yaml")
        self.assertIn("state: complete", receipts[0].read_text())
        self.assertIn("runner_succeeded: true", receipts[0].read_text())
        self.assertFalse(list((Path(self.env["HM_OUTPUT_WORK_DIR"]) / self.game.name / "runs").glob("*")))
        # Reconciliation retries the frozen script and native identity.
        self.runner.unlink()
        retry = subprocess.run([str(script)], env=self.env, text=True, capture_output=True)
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(len(list(self.game.glob("*.mp4"))), 3)

    def test_interactive_publication_rejects_preview_only_or_program_calibration(self):
        for arguments in (
            ["--enable-sinks=WEBRTC"],
            ["--stitching-calibration-only", "--enable-sinks=WEBRTC,ENCODE_FILE"],
        ):
            (self.game / "config.yaml").write_text(json.dumps({"hstream_ui": {"job": {"arguments": arguments}}}))
            result = self.generate("--interactive-publication", "--force")
            self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(self.generate("--interactive-publication", "--runtime-only").returncode, 0)

    def test_interactive_requires_supervisor_runtime_directory(self):
        result = self.generate("--interactive")
        self.assertEqual(result.returncode, 0, result.stderr)
        blocked = self.root / "blocked"
        blocked.mkdir()
        unwritable, unsearchable = blocked / "read-only", blocked / "no-exec"
        unwritable.mkdir(mode=0o500)
        # Writable but not searchable: mkdir -p and -w both pass, cd does not.
        unsearchable.mkdir(mode=0o600)
        self.addCleanup(lambda: [path.chmod(0o700) for path in (unwritable, unsearchable)])
        # 78 is the documented "supervisor did not supply one" status; the
        # unsearchable directory only fails later, at the resolving cd.
        cases = [("unset", None, 78), ("empty", "", 78), ("not a directory", str(self.config), 78)]
        if os.geteuid() != 0:  # Root ignores the permission bits these rely on.
            cases += [("unwritable", str(unwritable), 78), ("unsearchable", str(unsearchable), 1)]
        for name, value, status in cases:
            with self.subTest(case=name):
                environment = dict(self.env)
                if value is None:
                    environment.pop("HSTREAM_RUNTIME_CACHE_DIR", None)
                else:
                    environment["HSTREAM_RUNTIME_CACHE_DIR"] = value
                run = subprocess.run(
                    [str(self.game / "hstream-job.sh")], env=environment, capture_output=True, text=True
                )
                self.assertEqual(run.returncode, status, run.stdout)
                if status == 78:
                    self.assertIn("Managed runtime directory is unavailable", run.stderr)
                self.assertFalse(self.output.exists(), "the runner must not start")

    def test_runtime_only_forwards_exact_native_subcommand_arguments(self):
        self.env["HSTREAM_RUNTIME_CACHE_DIR"] = str(self.root / "private runtime")
        result = self.generate("--runtime-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        arguments = ["--tensorrt-runtime-info", "--game-id", str(self.game)]
        run = subprocess.run([str(self.game / "hstream-job.sh"), *arguments], env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 7, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"], arguments)
        self.assertNotIn("--encode-job", actual["argv"])

    def test_export_freezes_explicit_telemetry_checkbox(self):
        for capture in (False, True):
            with self.subTest(capture=capture):
                config = self.game / "config.yaml"
                config.write_text(json.dumps({"hstream_ui": {"job": {
                    "arguments": self.arguments, "capture_telemetry": capture,
                }}}))
                result = self.generate("--force")
                self.assertEqual(result.returncode, 0, result.stderr)
                config.write_text("{}")
                run = subprocess.run(
                    [str(self.game / "hstream-job.sh")], env=self.env,
                    capture_output=True, check=False,
                )
                self.assertEqual(run.returncode, 7)
                actual = json.loads(self.output.read_text())
                self.assertIn(
                    "--options=hstream_ui.job.capture_telemetry=" + str(capture).lower(),
                    actual["argv"],
                )

    def test_execution_resolves_layered_output_root(self):
        home = self.root / "root-resolution-home"
        (home / ".hstream").mkdir(parents=True)
        baseline = self.root / "root-resolution-config"
        baseline.mkdir()
        self.env["HOME"] = str(home)
        self.env["HM_CONFIG_ROOT"] = str(baseline)
        self.env.pop("HM_OUTPUT_WORK_DIR", None)
        baseline_root = self.root / "baseline work"
        user_root = self.root / "user work"
        game_root = self.root / "game work"
        cli_root = self.root / "CLI 'quoted' work"
        env_root = self.root / "environment work"
        for name, user_paths, game_paths, options, environment, expected in (
            ("baseline", None, None, [], None, baseline_root),
            ("user", {"output-root": str(user_root)}, None, [], None, user_root),
            ("game", {"output-root": str(user_root)}, {"output-root": str(game_root)}, [], None, game_root),
            ("other-path", {"output-root": str(user_root)}, {"game-root": "unrelated"}, [], None, user_root),
            ("clear-paths", {"output-root": str(user_root)}, None, [], None, home / "hstream_output"),
            ("relative", None, {"output-root": "relative-work"}, [], None, home / "relative-work"),
            ("tilde", None, {"output-root": "~/tilde-work"}, [], None, home / "tilde-work"),
            ("CLI", {"output-root": str(user_root)}, {"output-root": str(game_root)},
             ["--options=paths.output-root=" + str(cli_root)], None, cli_root),
            ("group-order", None, {"output-root": str(game_root)},
             ["--options", "paths.output-root=ignored,paths.output-root=also-ignored",
              "-p", "unrelated=1,paths.output-root=" + str(cli_root)], None, cli_root),
            ("first-in-group", None, None,
             ["-ppaths.output-root=" + str(cli_root) + ",paths.output-root=ignored"], None, cli_root),
            ("environment", {"output-root": str(user_root)}, {"output-root": str(game_root)},
             ["--options=paths.output-root=" + str(cli_root)], env_root, env_root),
        ):
            with self.subTest(layer=name):
                # Export against different file contents, then change them:
                # the script must resolve live config, while preserving its
                # saved option arguments.
                (baseline / "baseline.yaml").write_text(json.dumps({
                    "hstream_ui": {"export_archive_routes": ["program_4k"]},
                }))
                (home / ".hstream/hstream.yaml").write_text("{}")
                (self.game / "config.yaml").write_text(json.dumps({
                    "hstream_ui": {"job": {"arguments": options}}
                }))
                self.env.pop("HM_OUTPUT_WORK_DIR", None)
                self.assertEqual(self.generate("--force").returncode, 0)
                (baseline / "baseline.yaml").write_text(json.dumps({
                    "paths": {"output-root": str(baseline_root)}
                }))
                (home / ".hstream/hstream.yaml").write_text(json.dumps(
                    {"paths": user_paths} if user_paths is not None else {}
                ))
                (self.game / "config.yaml").write_text(json.dumps(
                    {"paths": game_paths} if game_paths is not None or name == "clear-paths" else {}
                ))
                if environment:
                    self.env["HM_OUTPUT_WORK_DIR"] = str(environment)
                run = subprocess.run([str(self.game / "hstream-job.sh")], env=self.env,
                                     text=True, capture_output=True, check=False)
                self.assertEqual(run.returncode, 7, run.stderr)
                actual = json.loads(self.output.read_text())
                manifest = Path(actual["env"]["HSTREAM_JOB_MANIFEST"])
                self.assertEqual(manifest.parents[3], expected)
                self.assertEqual(actual["argv"][5:5 + len(options)], options)

    def test_explicit_finalizer_work_root_remains_authoritative(self):
        finalizer = Path(TOOL).parent.parent / "hstream-finalize/hstream-finalize"
        explicit = self.root / "explicit helper work"
        run = subprocess.run([
            str(finalizer), "--game-dir", str(self.game), "--run-id", "explicit-root-test",
            "--work-root", str(explicit), "--options", "paths.output-root=ignored",
            "--", str(self.runner),
        ], env=self.env, text=True, capture_output=True, check=False)
        self.assertEqual(run.returncode, 7, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertEqual(Path(actual["env"]["HSTREAM_JOB_MANIFEST"]).parents[3], explicit)

    def test_failed_retry_keeps_admitted_root_after_configuration_changes(self):
        self.env.pop("HM_OUTPUT_WORK_DIR", None)
        self.env["HOME"] = str(self.root / "retry-home")
        Path(self.env["HOME"]).mkdir()
        original = self.root / "original work"
        changed = self.root / "changed work"
        config = json.loads((self.game / "config.yaml").read_text())
        config["paths"] = {"output-root": str(original)}
        (self.game / "config.yaml").write_text(json.dumps(config))
        generated = self.generate()
        self.assertEqual(generated.returncode, 0, generated.stderr)
        script = self.game / "hstream-job.sh"
        run = subprocess.run([str(script)], env=self.env, text=True, capture_output=True)
        self.assertEqual(run.returncode, 7, (run.stdout, run.stderr))
        actual = json.loads(self.output.read_text())
        manifest = Path(actual["env"]["HSTREAM_JOB_MANIFEST"])
        self.assertEqual(manifest.parents[3], original)
        self.output.unlink()
        config["paths"]["output-root"] = str(changed)
        for contents in (json.dumps(config), "invalid: ["):
            (self.game / "config.yaml").write_text(contents)
            retried = subprocess.run([str(script)], env=self.env, text=True, capture_output=True)
            self.assertEqual(retried.returncode, 1, (retried.stdout, retried.stderr))
            self.assertIn("Encoding did not complete", retried.stderr)
            self.assertIn(str(manifest.parent), retried.stderr)
            self.assertFalse(self.output.exists(), "failed encoding cannot be restarted")
            self.assertFalse(changed.exists(), "retry cannot create a second workspace")

    def test_symlinked_game_keeps_its_logical_id(self):
        alias = self.root / "linked-game"
        alias.symlink_to(self.game, target_is_directory=True)
        result = self.generate("--game-dir", str(alias))
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([str(alias / "hstream-job.sh")], env=self.env, check=False)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"][:2], ["-g", "linked-game"])
        self.assertEqual(actual["env"]["HM_GAME_DIR"], str(self.root))

    def test_cancellation_reaches_runner_and_preserves_failure(self):
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport json, os, time\n"
            "with open(os.environ['JOB_TEST_OUTPUT'], 'w') as f: json.dump({'pid':os.getpid()}, f)\n"
            "time.sleep(60)\n"
        )
        self.assertEqual(self.generate().returncode, 0)
        process = subprocess.Popen([str(self.game / "hstream-job.sh")], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not self.output.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(self.output.exists(), "runner started")
            runner_pid = json.loads(self.output.read_text())["pid"]
            process.terminate()
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143, (stdout, stderr))
            with self.assertRaises(ProcessLookupError):
                os.kill(runner_pid, 0)
            self.assertTrue(list(Path(self.env["HM_OUTPUT_WORK_DIR"]).glob("*/runs/*/run.yaml")))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_overwrite_and_invalid_directives(self):
        self.assertEqual(self.generate().returncode, 0)
        script = self.game / "hstream-job.sh"
        original = script.read_bytes()
        self.assertNotEqual(self.generate().returncode, 0)
        self.assertEqual(script.read_bytes(), original)
        self.assertEqual(self.generate("--force").returncode, 0)
        original = script.read_bytes()
        self.assertNotEqual(
            self.generate(
                "--force", "--sbatch", "--partition=gpu\ntouch INJECTED"
            ).returncode,
            0,
        )
        self.assertEqual(script.read_bytes(), original)

    def test_export_uses_execution_host_path_without_export_host_files(self):
        for installed in (True, False):
            with self.subTest(installed=installed):
                layout = self.root / ("installed" if installed else "workspace")
                bin_dir = (layout / "bin" if installed else
                           layout / "bazel-out/k8-fastbuild/bin/src/apps/hstream-job")
                bin_dir.mkdir(parents=True)
                tool = bin_dir / "hstream-job"
                shutil.copy2(TOOL, tool)
                # Export must not need any sibling binaries or source configs.
                # Neither the export-time PATH nor its baseline belongs in the job.
                export_bin = layout / "export-bin"
                export_bin.mkdir()
                for name in ("hstream-cli", "hstream-finalize"):
                    stub = export_bin / name
                    stub.write_text("#!/bin/sh\nexit 99\n")
                    stub.chmod(0o700)
                export_env = dict(self.env, PATH=str(export_bin),
                                  HM_CONFIG_ROOT=str(layout / "configs"))
                result = subprocess.run(
                    [str(tool), "--game-dir", str(self.game), "--force"],
                    cwd=layout, env=export_env, text=True, capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                script = self.game / "hstream-job.sh"
                script_text = script.read_text()
                self.assertNotIn(str(layout), script_text)
                self.assertNotIn("HM_CONFIG_ROOT", script_text)
                self.assertNotIn("bazel", script_text.lower())
                self.assertNotIn("cd --", script_text)
                self.assertIn("exec 'hstream-finalize'", script_text)
                self.assertIn(" -- 'hstream-cli'", script_text)
                shutil.rmtree(layout)
                runner = self.runtime_bin / "hstream-cli"
                shutil.copy2(self.runner, runner)
                runtime_config = self.root / "execution-configs"
                runtime_config.mkdir(exist_ok=True)
                (runtime_config / "baseline.yaml").write_text("{}")
                runtime_env = dict(self.env, HM_CONFIG_ROOT=str(runtime_config))
                run = subprocess.run([str(script)], cwd="/", env=runtime_env,
                                     text=True, capture_output=True)
                self.assertEqual(run.returncode, 7, run.stderr)
                actual = json.loads(self.output.read_text())
                self.assertEqual(actual["cwd"], "/")
                self.assertNotIn("-c", actual["argv"])
                self.assertEqual(actual["argv"][:3],
                                 ["-g", self.game.name, "--enable-sources=URI-MULTIPLE"])
                self.assertEqual(actual["env"]["HM_CONFIG_ROOT"], str(runtime_config))
                self.assertEqual(actual["env"]["PATH"], runtime_env["PATH"])
                manifest = Path(actual["env"]["HSTREAM_JOB_MANIFEST"])
                self.assertEqual(Path(actual["env"]["HSTREAM_RUNTIME_CACHE_DIR"]),
                                 manifest.parent / "runtime")

    def install_packaged_launcher(self):
        """Put the real Debian launcher on PATH as hstream-cli, relocated."""
        install = self.root / "install 'quoted'"
        (install / "bin").mkdir(parents=True)
        shutil.copy2(self.runner, install / "bin/hstream-cli")
        # Exercise the actual Debian launcher without building a package or
        # replacing system binaries. Only relocate its install prefix.
        wrapper = PACKAGER.read_text().split("<<'RUNSH'\n", 1)[1].split("\nRUNSH\n", 1)[0]
        wrapper = wrapper.replace("INSTALL_DIR=/opt/hstream",
                                  "INSTALL_DIR=" + shlex.quote(str(install)))
        launcher = self.runtime_bin / "hstream-cli"
        launcher.write_text(wrapper)
        launcher.chmod(0o700)
        return install

    def test_packaged_launcher_forwards_managed_interactive_arguments(self):
        # These modes carry no --encode-job, so without the managed marker the
        # launcher would inject its own config, sources and sink.
        install = self.install_packaged_launcher()
        self.env["HSTREAM_RUNTIME_CACHE_DIR"] = str(self.root / "private runtime")
        self.arguments[0] = "--enable-sinks=WEBRTC"
        (self.game / "config.yaml").write_text(
            json.dumps({"hstream_ui": {"job": {"arguments": self.arguments}}})
        )
        for mode, extra, expected in (
            ("--interactive", [],
             ["-g", self.game.name, "--enable-sources=URI-MULTIPLE"] + self.arguments),
            ("--runtime-only", ["--tensorrt-runtime-info"], ["--tensorrt-runtime-info"]),
        ):
            with self.subTest(mode=mode):
                # No --runner: resolve the command from the execution host PATH.
                exported = subprocess.run(
                    [TOOL, "--game-dir", str(self.game), "--force", mode],
                    cwd=self.root, env=self.env, text=True, capture_output=True,
                )
                self.assertEqual(exported.returncode, 0, exported.stderr)
                script = self.game / "hstream-job.sh"
                self.assertIn("exec 'hstream-cli'", script.read_text())
                run = subprocess.run([str(script), *extra], cwd="/", env=self.env,
                                     text=True, capture_output=True)
                self.assertEqual(run.returncode, 7, run.stderr)
                actual = json.loads(self.output.read_text())
                self.assertEqual(actual["argv"], expected)
                self.assertNotIn("--encode-job", actual["argv"])
                self.assertEqual(actual["env"]["HSTREAM_JOB_MANAGED_RUNTIME"], "1")
                self.assertEqual(actual["cwd"], "/")
                # The marker must be read after the launcher's runtime setup,
                # which is the only part of it a managed run still needs.
                self.assertIn(str(install / "bin"), actual["env"]["PATH"].split(os.pathsep))

    def test_packaged_launcher_still_completes_unmarked_invocations(self):
        # The managed bypass must stay narrow: an ordinary operator command
        # keeps the launcher's config, sources, sink and install cwd.
        install = self.install_packaged_launcher()
        home = self.root / "launcher-home"
        home.mkdir()
        environment = dict(self.env, HOME=str(home))
        environment.pop("HSTREAM_JOB_MANAGED_RUNTIME", None)
        environment.pop("DISPLAY", None)
        run = subprocess.run([str(install.parent / "runtime-bin/hstream-cli"), "-g", "mygame"],
                             cwd="/", env=environment, text=True, capture_output=True)
        self.assertEqual(run.returncode, 7, run.stderr)
        actual = json.loads(self.output.read_text())
        self.assertEqual(actual["argv"][:2], ["-c", str(install / "configs/ds_hockey_app_config.yaml")])
        self.assertIn("--enable-sources=URI-MULTIPLE", actual["argv"])
        self.assertIn("--options=pipeline.hmaudio.enable=1", actual["argv"])
        self.assertIn("--enable-sinks=ENCODE_FILE", actual["argv"])
        self.assertEqual(actual["argv"][-2:], ["-g", "mygame"])
        self.assertEqual(actual["cwd"], str(install))
        self.assertNotIn("HSTREAM_JOB_MANAGED_RUNTIME", actual["env"])

    def test_packaged_launcher_preserves_job_arguments_and_managed_cache(self):
        self.install_packaged_launcher()
        for config in (None, str(self.config), self.config.name):
            for home in (None, str(self.root / "unwritable-home")):
                with self.subTest(config=config, home=home):
                    command = [TOOL, "--game-dir", str(self.game), "--force",
                               "--working-directory", str(self.root)]
                    if config:
                        command += ["--config", config]
                    env = dict(self.env)
                    if home is None:
                        env.pop("HOME", None)
                    else:
                        Path(home).write_text("not a cache directory")
                        env["HOME"] = home
                    exported = subprocess.run(command, cwd=self.root, env=env,
                                              text=True, capture_output=True)
                    self.assertEqual(exported.returncode, 0, exported.stderr)
                    run = subprocess.run([str(self.game / "hstream-job.sh")],
                                         cwd="/", env=env, text=True, capture_output=True)
                    self.assertEqual(run.returncode, 7, run.stderr)
                    actual = json.loads(self.output.read_text())
                    self.assertEqual(actual["env"]["HSTREAM_JOB_MANAGED_RUNTIME"], "1")
                    expected = ["-g", self.game.name]
                    if config:
                        expected += ["-c", str(self.config)]
                    expected += (["--enable-sources=URI-MULTIPLE"] + self.arguments[1:]
                                 + ["--enable-sinks=ENCODE_FILE", "--encode-job"])
                    self.assertEqual(actual["argv"], expected)
                    self.assertEqual(actual["cwd"], str(self.root))
                    runtime = Path(actual["env"]["HSTREAM_RUNTIME_CACHE_DIR"])
                    registry = Path(actual["env"]["GST_REGISTRY"])
                    self.assertEqual(registry.parent, runtime / "gstreamer-1.0")
                    self.assertTrue(registry.parent.is_dir())

    def test_rejected_concurrent_launch_creates_no_runtime_scratch(self):
        self.runner.write_text(
            "#!/usr/bin/env python3\nimport json, os, time\n"
            "with open(os.environ['JOB_TEST_OUTPUT'], 'w') as f: json.dump(dict(os.environ), f)\n"
            "time.sleep(60)\n"
        )
        self.assertEqual(self.generate().returncode, 0)
        script = self.game / "hstream-job.sh"
        process = subprocess.Popen([str(script)], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not self.output.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(self.output.exists(), "runner started")
            actual = json.loads(self.output.read_text())
            runtime = Path(actual["HSTREAM_RUNTIME_CACHE_DIR"])
            self.assertEqual(runtime, Path(actual["HSTREAM_JOB_MANIFEST"]).parent / "runtime")
            before = {str(path) for path in self.root.rglob("*")}
            rejected = subprocess.run([str(script)], env=self.env, text=True,
                                      capture_output=True, timeout=5)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual({str(path) for path in self.root.rglob("*")}, before)
            self.assertFalse((self.root / ".cache").exists())
        finally:
            process.terminate()
            process.communicate(timeout=5)

    def test_headless_encoding_defaults_and_selections(self):
        for sinks, expected in (
            ("RENDER", "ENCODE_PROGRAM_4K_FILE"),
            ("FAKE", "ENCODE_PROGRAM_4K_FILE"),
            ("RTMP,RENDER", "ENCODE_PROGRAM_4K_FILE"),
            ("RENDER,ENCODE_FILE", "ENCODE_FILE"),
            ("RENDER,ENCODE_STITCHED_FILE", "ENCODE_STITCHED_FILE"),
            ("ENCODE_STITCHED_FILE,RENDER,ENCODE_PROGRAM_4K_FILE",
             "ENCODE_STITCHED_FILE,ENCODE_PROGRAM_4K_FILE"),
            ("ENCODE_FILE,ENCODE_PROGRAM_4K_FILE", "ENCODE_FILE,ENCODE_PROGRAM_4K_FILE"),
            ("ENCODE_FILE,ENCODE_PROGRAM_4K_FILE,ENCODE_STITCHED_FILE",
             "ENCODE_FILE,ENCODE_PROGRAM_4K_FILE,ENCODE_STITCHED_FILE"),
        ):
            with self.subTest(sinks=sinks):
                (self.game / "config.yaml").write_text(json.dumps({
                    "hstream_ui": {"job": {"arguments": [
                        "--enable-sinks", sinks, "--show", "--render-realtime",
                        "--show-scaled=0.5", "--show-stitching", "0.5",
                        "--options=rink.camera.zoom_in_aggressiveness=0.7",
                    ]}}
                }))
                result = self.generate("--force")
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(self.game / "hstream-job.sh")], env=self.env, check=False)
                args = json.loads(self.output.read_text())["argv"]
                self.assertIn("--enable-sinks=" + expected, args)
                self.assertEqual(args.count("--encode-job"), 1)
                self.assertFalse(any(a.startswith("--show") for a in args))
                self.assertNotIn("--render-realtime", args)
                self.assertEqual(
                    "--stitching-calibration-only" in args,
                    expected == "ENCODE_STITCHED_FILE",
                )
                self.assertIn("--options=rink.camera.zoom_in_aggressiveness=0.7", args)

    def test_stitched_only_recipe_and_telemetry(self):
        for explicit_mode in (False, True):
            for capture in (False, True):
                with self.subTest(explicit_mode=explicit_mode, capture=capture):
                    arguments = ["--enable-sinks=ENCODE_STITCHED_FILE"]
                    if explicit_mode:
                        arguments.append("--stitching-calibration-only")
                    (self.game / "config.yaml").write_text(json.dumps({
                        "hstream_ui": {"job": {
                            "arguments": arguments, "capture_telemetry": capture,
                        }}
                    }))
                    result = self.generate("--force")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    subprocess.run([str(self.game / "hstream-job.sh")], env=self.env, check=False)
                    args = json.loads(self.output.read_text())["argv"]
                    self.assertEqual(args.count("--stitching-calibration-only"), 1)
                    self.assertIn("--options=hstream_ui.job.capture_telemetry=false", args)
                    self.assertNotIn("--options=hstream_ui.job.capture_telemetry=true", args)

    def test_calibration_rejects_program_archives(self):
        self.assertEqual(self.generate().returncode, 0)
        script = self.game / "hstream-job.sh"
        original = script.read_bytes()
        for sinks in ("ENCODE_FILE", "ENCODE_PROGRAM_4K_FILE", "ENCODE_FILE,ENCODE_STITCHED_FILE"):
            with self.subTest(sinks=sinks):
                (self.game / "config.yaml").write_text(json.dumps({
                    "hstream_ui": {"job": {"arguments": [
                        "--stitching-calibration-only", "--enable-sinks=" + sinks,
                    ]}}
                }))
                result = self.generate("--force")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("require only Archive Stitched", result.stderr)
                self.assertEqual(script.read_bytes(), original)

    def test_explicit_selection_and_rejected_nonencoding_job(self):
        result = self.generate("--enable-sinks", "ENCODE_STITCHED_FILE")
        self.assertEqual(result.returncode, 0, result.stderr)
        script = self.game / "hstream-job.sh"
        self.assertIn("--enable-sinks=ENCODE_STITCHED_FILE", script.read_text())
        self.assertIn("--stitching-calibration-only", script.read_text())
        original = script.read_bytes()
        result = self.generate("--force", "--enable-sinks", "FAKE")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(script.read_bytes(), original)
        legacy_config = self.root / "legacy.txt"
        legacy_config.write_text("[application]\nenable-perf-measurement=0\n")
        result = self.generate("--force", "--config", str(legacy_config))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("require YAML pipeline configs", result.stderr)
        self.assertEqual(script.read_bytes(), original)
        (self.game / "config.yaml").write_text(json.dumps({
            "hstream_ui": {"job": {"arguments": ["--stitching-calibration-only"]}}
        }))
        result = self.generate("--force")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(script.read_bytes(), original)

    def test_plain_config_and_custom_output(self):
        (self.game / "config.yaml").write_text("pipeline: {}\n")
        custom = self.root / "custom.sh"
        result = self.generate("--output", str(custom))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(custom.exists())
        self.assertIn("--enable-sinks=ENCODE_PROGRAM_4K_FILE", custom.read_text())
        self.assertIn("--encode-job", custom.read_text())
        (self.game / "config.yaml").write_text("[invalid")
        self.assertNotEqual(self.generate().returncode, 0)

    def test_export_fallback_requires_the_shared_baseline_archive_recipe(self):
        baseline = self.root / "baseline"
        baseline.mkdir()
        self.env["HM_CONFIG_ROOT"] = str(baseline)
        (self.game / "config.yaml").write_text("pipeline: {}\n")
        (baseline / "baseline.yaml").write_text(json.dumps({
            "hstream_ui": {"export_archive_routes": ["stitched"]},
        }))
        exported = self.generate()
        self.assertEqual(exported.returncode, 0, exported.stderr)
        script = self.game / "hstream-job.sh"
        self.assertIn("--enable-sinks=ENCODE_STITCHED_FILE", script.read_text())
        self.assertIn("--stitching-calibration-only", script.read_text())
        original = script.read_bytes()
        for defaults in ({}, {"export_archive_routes": []}, {"export_archive_routes": ["unknown"]}):
            (baseline / "baseline.yaml").write_text(json.dumps({"hstream_ui": defaults}))
            self.assertNotEqual(self.generate("--force").returncode, 0)
            self.assertEqual(script.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
