#!/usr/bin/env python3

from pathlib import Path
from contextlib import redirect_stdout
import csv
import io
import sys
import tempfile
import unittest
from unittest import mock

import yaml

try:
  import capture_stitching_projection_frames as capture
except ModuleNotFoundError:
  from scripts import capture_stitching_projection_frames as capture


class ProjectionFrameConfigTest(unittest.TestCase):
  def write_runtime_fixture(self, root: Path, labels: tuple[str, ...] = ("first",)) -> tuple[Path, Path]:
    source_game = root / "source-game"
    source_game.mkdir()
    (source_game / "config.yaml").write_text("{}\n", encoding="utf-8")
    executable = root / "pipeline-app"
    executable.touch()
    pipeline_config = root / "pipeline.yaml"
    pipeline_config.touch()
    matrix_config = root / "matrix.yaml"
    matrix_config.write_text(
        yaml.safe_dump(
            {
                "version": 1,
                "source_game_dir": str(source_game),
                "output_dir": str(root / "results"),
                "pipeline": {
                    "workspace": str(root),
                    "executable": str(executable),
                    "config_root": str(root),
                    "config": str(pipeline_config),
                },
                "projections": [
                    {
                        "name": "rectilinear",
                        "variants": [
                            {"label": label, "parameters": [], "auto_fov": True} for label in labels
                        ],
                    }
                ],
            },
            sort_keys=False,
        ),
        encoding="utf-8",
    )
    return matrix_config, root / "results"

  def test_starter_config_is_exhaustive_and_bounded(self) -> None:
    config_path = Path(__file__).resolve().parents[1] / "configs/stitching_projection_frames.yaml"
    with config_path.open("r", encoding="utf-8") as stream:
      cases = capture.expand_cases(yaml.safe_load(stream))
    grouped: dict[str, list[dict[str, object]]] = {}
    for case in cases:
      grouped.setdefault(str(case["projection"]), []).append(case)

    self.assertEqual(set(grouped), set(capture.SUPPORTED_PROJECTIONS))
    self.assertEqual(len(cases), 58)
    for projection, projection_cases in grouped.items():
      self.assertEqual(len(projection_cases), 10 if projection in capture.PARAMETER_SPECS else 1)
    for projection in capture.PARAMETER_SPECS:
      fixed_fovs = {
          int(case["horizontal_fov"])
          for case in grouped[projection]
          if not bool(case["auto_fov"])
      }
      self.assertTrue({170, 180, 190}.issubset(fixed_fovs))

  def test_output_name_describes_effective_projection_config(self) -> None:
    state = {
        "projection": "general-panini",
        "parameters": (100.0, -50.0, 25.5),
        "auto_fov": False,
        "horizontal_fov": 190.0,
        "auto_canvas": False,
        "auto_crop": True,
        "label": "review",
    }
    self.assertEqual(
        capture.output_stem(7, state),
        "007__general-panini__params-100-m50-25p5__fov-190__canvas-retained__crop-auto__review",
    )

  def test_outcome_labels_are_green_and_red(self) -> None:
    self.assertEqual(capture.outcome_label("pass", False), "PASS")
    self.assertEqual(capture.outcome_label("fail", False), "FAIL")
    self.assertEqual(capture.outcome_label("pass", True), "\033[32mPASS\033[0m")
    self.assertEqual(capture.outcome_label("fail", True), "\033[31mFAIL\033[0m")

  def test_panorama_conversion_publishes_atomically(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-convert-test-") as temporary:
      root = Path(temporary)
      source = root / "panorama.tif"
      source.touch()
      destination = root / "frame.png"
      destination.write_bytes(b"previous")

      def fail_after_partial_output(command: list[str], **_kwargs: object) -> None:
        Path(command[-1]).write_bytes(b"partial")
        raise capture.subprocess.CalledProcessError(1, command)

      with mock.patch.object(capture.subprocess, "run", side_effect=fail_after_partial_output):
        with self.assertRaises(capture.subprocess.CalledProcessError):
          capture.convert_panorama(source, destination, "ffmpeg")
      self.assertEqual(destination.read_bytes(), b"previous")
      self.assertEqual(list(root.glob(".*.tmp-*.png")), [])

  def test_rejects_parameters_on_parameterless_projection(self) -> None:
    with self.assertRaisesRegex(ValueError, "exactly 0"):
      capture.expand_cases(
          {"version": 1, "projections": [{"name": "rectilinear", "variants": [{"label": "bad", "parameters": [1]}]}]}
      )

  def test_rejects_fractional_biplane_corner_switch(self) -> None:
    with self.assertRaisesRegex(ValueError, "exactly 0 or 1"):
      capture.expand_cases(
          {"version": 1, "projections": [{"name": "biplane", "variants": [{"label": "bad", "parameters": [45, 0.5]}]}]}
      )

  def test_rejects_fixed_fov_above_projection_limit(self) -> None:
    with self.assertRaisesRegex(ValueError, "allows at most 179"):
      capture.expand_cases(
          {
              "version": 1,
              "projections": [
                  {
                      "name": "rectilinear",
                      "variants": [
                          {"label": "bad", "parameters": [], "auto_fov": False, "horizontal_fov": 190}
                      ],
                  }
              ],
          }
      )

  def test_resource_failure_is_recorded_per_case_and_matrix_continues(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-failure-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root, ("first", "second"))
      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config)]
      with (
          mock.patch.object(sys, "argv", arguments),
          mock.patch.object(capture.shutil, "which", return_value="/usr/bin/ffmpeg"),
          mock.patch.object(capture.fcntl, "flock"),
          mock.patch.object(
              capture.calibration_matrix,
              "wait_for_resource_headroom",
              side_effect=[RuntimeError("headroom one"), RuntimeError("headroom two")],
          ) as wait,
          mock.patch.object(capture.calibration_matrix, "create_isolated_game") as isolate,
      ):
        output = io.StringIO()
        with redirect_stdout(output):
          self.assertEqual(capture.main(), 1)

      self.assertEqual(wait.call_count, 2)
      isolate.assert_not_called()
      with (results / "manifest.csv").open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
      self.assertEqual([row["outcome"] for row in rows], ["fail", "fail"])
      self.assertEqual([row["first_failure"] for row in rows], ["headroom one", "headroom two"])
      self.assertTrue(all(Path(row["log"]).is_file() for row in rows))
      self.assertIn("Results:\n  FAIL 001 rectilinear--first :: headroom one", output.getvalue())
      self.assertIn("  FAIL 002 rectilinear--second :: headroom two", output.getvalue())

  def test_completed_case_is_skipped_and_force_reruns_it(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-resume-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root)
      config = capture.load_config(matrix_config)
      state = capture.expand_cases(config)[0]
      capture.prepare_output_directory(results)
      stem = capture.output_stem(1, state)
      png_path = results / f"{stem}.png"
      png_path.write_bytes(b"png")
      effective_path = results / "effective-configs" / f"{stem}.yaml"
      effective_path.write_text(yaml.safe_dump(capture.effective_case_config(state), sort_keys=False), encoding="utf-8")
      log_path = results / "logs" / f"{stem}.log"
      log_path.touch()
      capture.write_manifest(
          results / "manifest.csv",
          {
              "1": capture.manifest_row(
                  1, state, png_path, effective_path, log_path, {"outcome": "pass", "return_code": 0}
              )
          },
      )

      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config)]
      with (
          mock.patch.object(sys, "argv", arguments),
          mock.patch.object(capture.shutil, "which", return_value="/usr/bin/ffmpeg"),
          mock.patch.object(capture.fcntl, "flock"),
          mock.patch.object(capture.calibration_matrix, "wait_for_resource_headroom") as wait,
          mock.patch.object(capture.calibration_matrix, "create_isolated_game") as isolate,
      ):
        output = io.StringIO()
        with redirect_stdout(output):
          self.assertEqual(capture.main(), 0)
      wait.assert_not_called()
      isolate.assert_not_called()
      self.assertIn("SKIP 001 rectilinear--first", output.getvalue())
      self.assertIn("PASS 001 rectilinear--first :: already captured (skipped)", output.getvalue())

      stale_log_text = "old successful log\n"
      log_path.write_text(stale_log_text, encoding="utf-8")
      stale_pto = results / "evidence" / f"{stem}.pto"
      stale_provenance = results / "evidence" / f"{stem}.provenance.txt"
      stale_pto.write_text("old pto\n", encoding="utf-8")
      stale_provenance.write_text("old provenance\n", encoding="utf-8")
      force_arguments = [*arguments, "--force"]
      with (
          mock.patch.object(sys, "argv", force_arguments),
          mock.patch.object(capture.shutil, "which", return_value="/usr/bin/ffmpeg"),
          mock.patch.object(capture.fcntl, "flock"),
          mock.patch.object(
              capture.calibration_matrix, "wait_for_resource_headroom", side_effect=RuntimeError("forced failure")
          ) as forced_wait,
          mock.patch.object(capture.calibration_matrix, "create_isolated_game") as forced_isolate,
      ):
        with redirect_stdout(io.StringIO()):
          self.assertEqual(capture.main(), 1)
      forced_wait.assert_called_once()
      forced_isolate.assert_not_called()
      self.assertFalse(png_path.exists())
      forced_manifest = capture.read_manifest(results / "manifest.csv")
      self.assertEqual(forced_manifest["1"]["outcome"], "fail")
      self.assertNotIn(stale_log_text.strip(), log_path.read_text(encoding="utf-8"))
      self.assertFalse(stale_pto.exists())
      self.assertFalse(stale_provenance.exists())

  def test_changed_effective_config_is_not_skipped(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-identity-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root)
      config = capture.load_config(matrix_config)
      state = capture.expand_cases(config)[0]
      capture.prepare_output_directory(results)
      stem = capture.output_stem(1, state)
      png_path = results / f"{stem}.png"
      png_path.write_bytes(b"png")
      effective_path = results / "effective-configs" / f"{stem}.yaml"
      effective_path.write_text(yaml.safe_dump(capture.effective_case_config(state), sort_keys=False), encoding="utf-8")
      log_path = results / "logs" / f"{stem}.log"
      log_path.touch()
      capture.write_manifest(
          results / "manifest.csv",
          {"1": capture.manifest_row(1, state, png_path, effective_path, log_path, {"outcome": "pass"})},
      )
      config["defaults"] = {"max_output_width": 2048}
      matrix_config.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")

      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config)]
      with (
          mock.patch.object(sys, "argv", arguments),
          mock.patch.object(capture.shutil, "which", return_value="/usr/bin/ffmpeg"),
          mock.patch.object(capture.fcntl, "flock"),
          mock.patch.object(
              capture.calibration_matrix, "wait_for_resource_headroom", side_effect=RuntimeError("reran changed case")
          ) as wait,
          mock.patch.object(capture.calibration_matrix, "create_isolated_game") as isolate,
      ):
        with redirect_stdout(io.StringIO()):
          self.assertEqual(capture.main(), 1)
      wait.assert_called_once()
      isolate.assert_not_called()
      self.assertFalse(png_path.exists())
      self.assertEqual(capture.read_manifest(results / "manifest.csv")["1"]["first_failure"], "reran changed case")

  def test_interrupted_force_is_marked_in_progress_before_artifacts_change(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-interrupt-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root)
      state = capture.expand_cases(capture.load_config(matrix_config))[0]
      capture.prepare_output_directory(results)
      stem = capture.output_stem(1, state)
      png_path = results / f"{stem}.png"
      png_path.write_bytes(b"png")
      effective_path = results / "effective-configs" / f"{stem}.yaml"
      effective_path.write_text(yaml.safe_dump(capture.effective_case_config(state), sort_keys=False), encoding="utf-8")
      log_path = results / "logs" / f"{stem}.log"
      log_path.touch()
      capture.write_manifest(
          results / "manifest.csv",
          {"1": capture.manifest_row(1, state, png_path, effective_path, log_path, {"outcome": "pass"})},
      )

      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config), "--force"]
      with (
          mock.patch.object(sys, "argv", arguments),
          mock.patch.object(capture.shutil, "which", return_value="/usr/bin/ffmpeg"),
          mock.patch.object(capture.fcntl, "flock"),
          mock.patch.object(
              capture.calibration_matrix, "wait_for_resource_headroom", side_effect=KeyboardInterrupt
          ),
          mock.patch.object(capture.calibration_matrix, "create_isolated_game") as isolate,
      ):
        with redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
          capture.main()
      isolate.assert_not_called()
      self.assertFalse(png_path.exists())
      self.assertEqual(capture.read_manifest(results / "manifest.csv")["1"]["outcome"], "in_progress")

  def test_clean_removes_only_owned_output_and_exits(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-clean-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root)
      capture.prepare_output_directory(results)
      (results / "artifact.png").write_bytes(b"png")
      state = capture.expand_cases(capture.load_config(matrix_config))[0]
      retained_work = Path(tempfile.mkdtemp(prefix="hstream-stitching-matrix-work-"))
      self.addCleanup(capture.shutil.rmtree, retained_work, ignore_errors=True)
      (retained_work / "fixture-matrix").mkdir()
      capture.write_manifest(
          results / "manifest.csv",
          {
              "1": capture.manifest_row(
                  1,
                  state,
                  results / "missing.png",
                  results / "effective-configs/missing.yaml",
                  results / "logs/missing.log",
                  {"outcome": "fail"},
                  str(retained_work),
              )
          },
      )
      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config), "--clean"]
      with mock.patch.object(sys, "argv", arguments), mock.patch.object(capture.fcntl, "flock"):
        output = io.StringIO()
        with redirect_stdout(output):
          self.assertEqual(capture.main(), 0)
      self.assertFalse(results.exists())
      self.assertFalse(retained_work.exists())
      self.assertIn("Cleaned projection capture artifacts", output.getvalue())

      results.mkdir()
      (results / "unrelated.txt").write_text("keep\n", encoding="utf-8")
      with mock.patch.object(sys, "argv", arguments), mock.patch.object(capture.fcntl, "flock"):
        with self.assertRaisesRegex(ValueError, "unrecognized output directory"):
          capture.main()
      self.assertTrue((results / "unrelated.txt").is_file())

  def test_clean_refuses_output_symlink_without_following_it(self) -> None:
    with tempfile.TemporaryDirectory(prefix="projection-frame-symlink-test-") as temporary:
      root = Path(temporary)
      matrix_config, results = self.write_runtime_fixture(root)
      target = root / "owned-target"
      capture.prepare_output_directory(target)
      results.symlink_to(target, target_is_directory=True)
      arguments = ["capture_stitching_projection_frames.py", "--config", str(matrix_config), "--clean"]
      with mock.patch.object(sys, "argv", arguments), mock.patch.object(capture.fcntl, "flock"):
        with self.assertRaisesRegex(ValueError, "unrecognized output directory"):
          capture.main()
      self.assertTrue(results.is_symlink())
      self.assertTrue(target.is_dir())
      self.assertTrue((target / capture.OUTPUT_MARKER).is_file())


if __name__ == "__main__":
  unittest.main()
