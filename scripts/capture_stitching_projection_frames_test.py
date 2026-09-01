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
                              {"label": "first", "parameters": [], "auto_fov": True},
                              {"label": "second", "parameters": [], "auto_fov": True},
                          ],
                      }
                  ],
              },
              sort_keys=False,
          ),
          encoding="utf-8",
      )
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
      with (root / "results/manifest.csv").open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
      self.assertEqual([row["outcome"] for row in rows], ["fail", "fail"])
      self.assertEqual([row["first_failure"] for row in rows], ["headroom one", "headroom two"])
      self.assertTrue(all(Path(row["log"]).is_file() for row in rows))
      self.assertIn("Results:\n  FAIL 001 rectilinear--first :: headroom one", output.getvalue())
      self.assertIn("  FAIL 002 rectilinear--second :: headroom two", output.getvalue())


if __name__ == "__main__":
  unittest.main()
