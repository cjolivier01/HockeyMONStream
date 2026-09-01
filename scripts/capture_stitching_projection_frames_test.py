#!/usr/bin/env python3

from pathlib import Path
import unittest

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


if __name__ == "__main__":
  unittest.main()
