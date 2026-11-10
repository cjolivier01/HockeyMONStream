#!/usr/bin/env python3
"""Geometry and artifact regressions for the offline rink warp."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import cv2
import numpy as np

from constrained_rink_warp import Curve, RinkWarp, main


def specification():
  return {"schema": 1, "source_size": [321, 151],
          "guides": [[0, .15, .45], [.25, .2, .7], [.5, .23, .85],
                     [.75, .2, .7], [1, .15, .45]],
          "target_y": .9, "max_vertical_stretch": 4,
          "transition_fraction": .15, "limit_transition_fraction": .2}


class WarpTest(unittest.TestCase):
  def test_curve_does_not_overshoot_and_hits_marks(self):
    x = np.array([0., .1, .2, .8, 1.])
    y = np.array([.2, .8, .8, .1, .3])
    curve = Curve(x, y)
    np.testing.assert_allclose(curve(x), y, atol=1e-15)
    for i in range(len(x) - 1):
      result = curve(np.linspace(x[i], x[i + 1], 501))
      self.assertGreaterEqual(result.min(), min(y[i], y[i + 1]) - 1e-15)
      self.assertLessEqual(result.max(), max(y[i], y[i + 1]) + 1e-15)

  def test_exact_near_constraint_and_fixed_far_region(self):
    warp = RinkWarp(specification())
    x = np.linspace(0, 1, 1001)
    far, near, _ = warp.columns(x)
    np.testing.assert_allclose(warp.forward_y(x, near), warp.target, atol=1e-14)
    for fraction in (0, .2, .8, 1):
      np.testing.assert_array_equal(warp.forward_y(x, far * fraction), far * fraction)

  def test_stretch_budget_monotonicity_and_continuous_shoulders(self):
    spec = specification()
    spec["max_vertical_stretch"] = 2
    warp = RinkWarp(spec)
    x = np.linspace(0, 1, 251)
    y = np.linspace(0, 1, 10001)[:, None]
    forward = warp.forward_y(x, y)
    scale = np.diff(forward, axis=0) * 10000
    self.assertGreaterEqual(scale.min(), 1 - 1e-10)
    self.assertLessEqual(scale.max(), 2 + 1e-10)
    far, near, _ = warp.columns(x)
    # Derivatives return to one on both sides of both constraints.
    for boundary in (far, near):
      for sign in (-1, 1):
        eps = 1e-7
        slope = (warp.forward_y(x, boundary + sign * eps) - warp.forward_y(x, boundary)) / (sign * eps)
        np.testing.assert_allclose(slope, 1, atol=1e-7)
    actual = warp.forward_y(x, near)
    self.assertGreater(warp.target - actual.min(), .1)
    self.assertLessEqual(actual.max(), warp.target + 1e-15)

  def test_inverse_roundtrip_including_outside_the_canvas(self):
    rng = np.random.default_rng(123)
    for transition in (.001, .15, .5):
      spec = specification()
      spec["transition_fraction"] = transition
      warp = RinkWarp(spec)
      x = rng.uniform(0, 1, 20000)
      y = rng.uniform(-.1, 1.6, 20000)
      np.testing.assert_allclose(warp.inverse_y(x, warp.forward_y(x, y)), y, atol=1e-9)
      np.testing.assert_allclose(warp.forward_y(x, warp.inverse_y(x, y)), y, atol=1e-9)

  def test_scalar_inverse_and_zero_displacement(self):
    spec = specification()
    spec["max_vertical_stretch"] = 1
    warp = RinkWarp(spec)
    for x in (0, .33, 1):
      for y in (0, .2, .7, 1):
        self.assertAlmostEqual(float(warp.inverse_y(x, y)), y)
        self.assertAlmostEqual(float(warp.forward_y(x, y)), y)

  def test_source_coordinates_stay_on_canvas(self):
    warp = RinkWarp(specification())
    x = np.linspace(0, 1, 321)
    y = np.linspace(0, 1, 151)[:, None]
    source = warp.inverse_y(x, y)
    self.assertGreaterEqual(source.min(), 0)
    self.assertLessEqual(source.max(), 1)
    self.assertTrue(np.all(np.diff(source, axis=0) > 0))

  def test_render_keeps_protected_pixels_and_vertical_lines(self):
    image = np.zeros((151, 321, 3), dtype=np.uint8)
    image[:] = np.arange(321, dtype=np.uint16)[None, :, None] % 256
    warp = RinkWarp(specification())
    np.testing.assert_array_equal(warp.render(image), image)
    rng = np.random.default_rng(1)
    image = rng.integers(0, 256, image.shape, dtype=np.uint8)
    output = warp.render(image)
    np.testing.assert_array_equal(output[:23], image[:23])

  def test_invalid_documents_are_rejected(self):
    cases = [("schema", True), ("schema", 2), ("source_size", [1, 151]),
             ("source_size", [321.0, 151]), ("source_size", [32767, 151]),
             ("target_y", .5), ("target_y", float("nan")),
             ("max_vertical_stretch", .9), ("max_vertical_stretch", True),
             ("transition_fraction", 0), ("transition_fraction", .51),
             ("limit_transition_fraction", 1), ("guides", []),
             ("guides", [[0, .2, .5], [1, .6, .5]]),
             ("guides", [[0, .2, .5], [0, .2, .5]]),
             ("guides", [[0, .2, .5], [.9, .2, .5]]),
             ("guides", [[0, .2, .5], [1, float("inf"), .5]])]
    for key, value in cases:
      with self.subTest(key=key, value=value):
        spec = specification()
        spec[key] = value
        with self.assertRaises(ValueError):
          RinkWarp(spec)
    spec = specification()
    spec["typo"] = 0
    with self.assertRaises(ValueError):
      RinkWarp(spec)

  def test_cli_writes_comparison_and_refuses_overwrite(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      image = np.zeros((151, 321, 3), dtype=np.uint8)
      image[35:120, :, 1] = 255
      cv2.imwrite(str(root / "input.png"), image)
      cv2.imwrite(str(root / "mask.png"), image[:, :, 1])
      (root / "guides.json").write_text(json.dumps(specification()))
      args = [str(root / "input.png"), "--guides", str(root / "guides.json"),
              "--output-dir", str(root / "result"), "--comparison-width", "321",
              "--mask", str(root / "mask.png")]
      with contextlib.redirect_stdout(io.StringIO()):
        self.assertEqual(main(args), 0)
      report = json.loads((root / "result/report.json").read_text())
      self.assertGreater(report["ice_fraction_after"], report["ice_fraction_before"])
      self.assertGreater(report["input_ice_pixels_mapped_outside_canvas"], 0)
      self.assertEqual(cv2.imread(str(root / "result/warped.png")).shape, image.shape)
      self.assertTrue((root / "result/comparison.jpg").is_file())
      with self.assertRaises(FileExistsError):
        main(args)
      np.testing.assert_array_equal(cv2.imread(str(root / "input.png")), image)

  def test_cli_rejects_shape_mismatch_and_precision_loss_before_writing(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "guides.json").write_text(json.dumps(specification()))
      args = [str(root / "input.png"), "--guides", str(root / "guides.json"),
              "--output-dir", str(root / "result"), "--comparison-width", "321"]
      for shape, dtype in (((150, 321, 3), np.uint8), ((151, 321, 3), np.uint16),
                           ((151, 321, 4), np.uint8)):
        cv2.imwrite(str(root / "input.png"), np.zeros(shape, dtype=dtype))
        with self.assertRaises(ValueError):
          main(args)
        self.assertFalse((root / "result").exists())

  def test_comparison_height_stays_positive_for_very_wide_inputs(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      spec = specification()
      spec["source_size"] = [2000, 2]
      (root / "guides.json").write_text(json.dumps(spec))
      cv2.imwrite(str(root / "input.png"), np.zeros((2, 2000, 3), dtype=np.uint8))
      with contextlib.redirect_stdout(io.StringIO()):
        main([str(root / "input.png"), "--guides", str(root / "guides.json"),
              "--output-dir", str(root / "result"), "--comparison-width", "320"])
      self.assertEqual(cv2.imread(str(root / "result/comparison.jpg")).shape[1], 320)


if __name__ == "__main__":
  unittest.main()
