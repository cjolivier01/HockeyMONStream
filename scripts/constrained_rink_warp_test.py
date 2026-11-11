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

from constrained_rink_warp import Curve, RinkWarp, interval, main


def specification():
  return {"schema": 2, "source_size": [321, 151],
          "guides": [[0, .15, .45], [.25, .4, .7], [.5, .55, .85],
                     [.75, .4, .7], [1, .15, .45]],
          "target_rows": [.6, .9], "horizontal_crop": [0, 1]}


class WarpTest(unittest.TestCase):
  def test_half_pixel_output_width_rounds_to_even(self):
    for schema in (2, 3):
      for width, expected in ((8194, 4097), (8196, 4099)):
        spec = specification()
        spec.update(schema=schema, source_size=[width, 201],
                    guides=[[0, .6, .9], [1, .6, .9]], target_rows=[.6, .9],
                    horizontal_crop=[0, .5], strength=0, aspect_balance=0)
        if schema == 2:
          del spec["strength"], spec["aspect_balance"]
        self.assertEqual(RinkWarp(spec).output_size, (expected, 201))

  def test_curve_does_not_overshoot_and_hits_marks(self):
    x = np.array([0., .1, .2, .8, 1.])
    y = np.array([.2, .8, .8, .1, .3])
    curve = Curve(x, y)
    np.testing.assert_allclose(curve(x), y, atol=1e-15)
    for i in range(len(x) - 1):
      result = curve(np.linspace(x[i], x[i + 1], 501))
      self.assertGreaterEqual(result.min(), min(y[i], y[i + 1]) - 1e-15)
      self.assertLessEqual(result.max(), max(y[i], y[i + 1]) + 1e-15)

  def test_both_rails_are_level_to_the_extreme_ends(self):
    warp = RinkWarp(specification())
    x = np.linspace(0, 1, 1001)
    upper, lower = warp.columns(x)
    np.testing.assert_allclose(warp.forward_y(x, upper), .6, atol=1e-14)
    np.testing.assert_allclose(warp.forward_y(x, lower), .9, atol=1e-14)
    self.assertLess(warp.report()["max_guide_error_pixels"], 1e-10)
    # Symmetric inputs have symmetric output; the endpoints are not subject
    # to an independent stretch cap that could leave one side higher.
    y = np.linspace(0, 1, 501)[:, None]
    np.testing.assert_allclose(warp.inverse_y(x, y), warp.inverse_y(1 - x, y), atol=1e-13)

  def test_positive_jacobian_and_continuous_slopes_at_rails(self):
    warp = RinkWarp(specification())
    x = np.linspace(0, 1, 251)
    y = np.linspace(0, 1, 10001)[:, None]
    scale = np.diff(warp.forward_y(x, y), axis=0) * 10000
    self.assertGreater(scale.min(), 0)
    self.assertTrue(np.all(np.isfinite(scale)))
    # Line thickness does not jump at a guide or the translation boundary.
    upper, lower, _, middle, shared, _ = warp.segments(x)
    for boundary, expected in ((upper, 1 / shared), (lower, 1 / middle)):
      for sign in (-1, 1):
        eps = 1e-9
        slope = (warp.forward_y(x, boundary + sign * eps) - warp.forward_y(x, boundary)) / (sign * eps)
        np.testing.assert_allclose(slope, expected, atol=1e-6)

  def test_inverse_roundtrip_with_compression_expansion_and_linear_tails(self):
    rng = np.random.default_rng(123)
    warp = RinkWarp(specification())
    x = rng.uniform(0, 1, 20000)
    y = rng.uniform(-.1, 1.6, 20000)
    np.testing.assert_allclose(warp.inverse_y(x, warp.forward_y(x, y)), y, atol=1e-12)
    np.testing.assert_allclose(warp.forward_y(x, warp.inverse_y(x, y)), y, atol=1e-12)
    for slope in (.0001, .01, 1, 10, 10000):
      t = np.linspace(0, 1, 1001)
      result = interval(t, 0, 1, 0, slope, 1, 1)
      np.testing.assert_allclose(interval(result, 0, 1, 0, slope, 1, 1, inverse=True), t, atol=1e-7)
      self.assertTrue(np.all(np.diff(result) > 0))

  def test_identity_and_scalars(self):
    spec = specification()
    spec["guides"] = [[0, .6, .9], [1, .6, .9]]
    warp = RinkWarp(spec)
    for x in (0, .33, 1):
      for y in (0, .2, .7, 1):
        self.assertAlmostEqual(float(warp.inverse_y(x, y)), y)
        self.assertAlmostEqual(float(warp.forward_y(x, y)), y)

  def test_source_sampling_bounds_and_whole_rink_preservation(self):
    spec = specification()
    spec["horizontal_crop"] = [.02, .98]
    warp = RinkWarp(spec)
    source_x = warp.inverse_x(np.linspace(0, 1, 321))
    np.testing.assert_allclose(warp.forward_x(source_x), np.linspace(0, 1, 321))
    self.assertTrue(np.all(np.diff(source_x) > 0))
    y = np.linspace(0, 1, 151)[:, None]
    source_y = warp.inverse_y(source_x, y)
    self.assertGreaterEqual(source_y.min(), 0)
    self.assertLessEqual(source_y.max(), 1)
    self.assertTrue(np.all(np.diff(source_y, axis=0) > 0))
    # Every point above the lower guide stays vertically in frame, including
    # the two corners. Foreground below that guide may be intentionally cropped.
    _, lower = warp.columns(source_x)
    for fraction in np.linspace(0, 1, 101):
      mapped = warp.forward_y(source_x, lower * fraction)
      self.assertGreaterEqual(mapped.min(), 0)
      self.assertLessEqual(mapped.max(), warp.bottom + 1e-14)
    self.assertGreater(warp.forward_y(.02, 1), 1)

  def test_render_preserves_verticals_and_does_not_invent_black_borders(self):
    image = np.zeros((151, 321, 3), dtype=np.uint8)
    image[:] = np.arange(321, dtype=np.uint16)[None, :, None] % 256
    warp = RinkWarp(specification())
    output = warp.render(image)
    np.testing.assert_array_equal(output, np.broadcast_to(output[:1], output.shape))
    np.testing.assert_array_equal(warp.render(np.full_like(image, 255)), 255)

  def test_mask_report_detects_each_cropping_direction(self):
    spec = specification()
    spec["horizontal_crop"] = [.1, .9]
    warp = RinkWarp(spec)
    mask = np.zeros((151, 321), np.uint8)
    mask[50, 160] = 1
    self.assertEqual(warp.mask_report(mask)["input_ice_pixels_mapped_outside_canvas"], 0)
    for point in ((50, 0), (50, 320), (150, 50)):
      mask[point] = 1
    self.assertEqual(warp.mask_report(mask)["input_ice_pixels_mapped_outside_canvas"], 3)
    self.assertLess(warp.mask_report(mask)["minimum_ice_clearance_pixels"], 0)

  def test_horizontal_expansion_matches_vertical_scale_between_rails(self):
    spec = specification()
    warp = RinkWarp(spec)
    x = (warp.source_x[1:] + warp.source_x[:-1]) / 2
    upper, lower = warp.columns(x)
    vertical = (warp.bottom - warp.top) / (lower - upper)
    horizontal = np.diff(warp.output_x) / np.diff(warp.source_x)
    horizontal *= (warp.output_size[0] - 1) / (warp.source_size[0] - 1)
    np.testing.assert_allclose(horizontal, vertical, rtol=.003)
    # A constant-scale grid must remain square in pixel coordinates, and the
    # output grows instead of compressing the horizontal axis back afterward.
    spec["guides"] = [[0, .3, .45], [1, .3, .45]]
    uniform = RinkWarp(spec)
    self.assertEqual(uniform.output_size, (641, 151))
    self.assertAlmostEqual(float(uniform.forward_y(.5, .35) - uniform.forward_y(.5, .3)), .1)

  def test_foreground_boundary_limit_and_output_width_limit(self):
    spec = specification()
    spec["target_rows"] = [.8, .9]
    warp = RinkWarp(spec)
    self.assertEqual(float(warp.inverse_y(.5, 1)), 1)
    x = np.linspace(0, 1, 301)
    y = np.linspace(0, 1, 501)[:, None]
    mapped = warp.inverse_y(x, y)
    self.assertGreaterEqual(mapped.min(), 0)
    self.assertLessEqual(mapped.max(), 1)
    self.assertTrue(np.all(np.diff(mapped, axis=0) > 0))
    np.testing.assert_allclose(warp.forward_y(x, mapped), np.broadcast_to(y, mapped.shape), atol=1e-12)
    spec["source_size"] = [2000, 151]
    spec["guides"] = [[0, .44, .45], [1, .44, .45]]
    spec["target_rows"] = [.3, .9]
    with self.assertRaisesRegex(ValueError, "output width"):
      RinkWarp(spec)

  def test_invalid_documents_are_rejected(self):
    cases = [("schema", True), ("schema", 1), ("source_size", [1, 151]),
             ("source_size", [321.0, 151]), ("source_size", [32767, 151]),
             ("target_rows", [.5, .8]), ("target_rows", [.9, .9]),
             ("target_rows", [.5, float("nan")]), ("target_rows", [True, .9]),
             ("target_rows", 0), ("target_rows", [.1, 1]),
             ("horizontal_crop", [1, 0]), ("horizontal_crop", [0, 1.1]),
             ("horizontal_crop", [0, 0]), ("horizontal_crop", [0, float("inf")]),
             ("guides", []), ("guides", [[0, .2, .5], [1, .6, .5]]),
             ("guides", [[0, .2, .5], [0, .2, .5]]),
             ("guides", [[0, .2, .5], [.9, .2, .5]]),
             ("guides", [[0, 0, .5], [1, .2, .5]]),
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

  def test_cli_writes_comparison_and_refuses_overwrite_or_clipped_ice(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      image = np.zeros((151, 321, 3), dtype=np.uint8)
      image[35:60, :, 1] = 255
      cv2.imwrite(str(root / "input.png"), image)
      cv2.imwrite(str(root / "mask.png"), image[:, :, 1])
      (root / "guides.json").write_text(json.dumps(specification()))
      args = [str(root / "input.png"), "--guides", str(root / "guides.json"),
              "--output-dir", str(root / "result"), "--comparison-width", "321",
              "--mask", str(root / "mask.png")]
      with contextlib.redirect_stdout(io.StringIO()):
        self.assertEqual(main(args), 0)
      report = json.loads((root / "result/report.json").read_text())
      self.assertEqual(report["input_ice_pixels_mapped_outside_canvas"], 0)
      self.assertEqual(cv2.imread(str(root / "result/warped.png")).shape[1::-1], RinkWarp(specification()).output_size)
      self.assertTrue((root / "result/comparison.jpg").is_file())
      with self.assertRaises(FileExistsError):
        main(args)
      np.testing.assert_array_equal(cv2.imread(str(root / "input.png")), image)
      args[args.index(str(root / "result"))] = str(root / "clipped")
      cv2.imwrite(str(root / "mask.png"), np.full((151, 321), 255, np.uint8))
      with self.assertRaisesRegex(ValueError, "marked ice outside"):
        main(args)
      self.assertFalse((root / "clipped").exists())

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


class EditorSchemaTest(unittest.TestCase):
  def test_zero_strength_is_identity(self):
    spec = {"schema": 3, "source_size": [321, 151],
            "guides": [[0, .2, .4], [.5, .5, .9], [1, .2, .4]],
            "target_rows": [.62, .96], "horizontal_crop": [0, 1],
            "strength": 0, "aspect_balance": 1}
    warp = RinkWarp(spec)
    self.assertEqual(warp.output_size, warp.source_size)
    x = np.linspace(0, 1, 23)
    y = np.linspace(0, 1, 37)[:, None]
    np.testing.assert_allclose(warp.inverse_y(x, y), np.broadcast_to(y, (37, 23)), atol=1e-12)
    np.testing.assert_allclose(warp.inverse_x(x), x, atol=1e-12)

  def test_partial_strength_round_trip_and_targets(self):
    spec = {"schema": 3, "source_size": [321, 151],
            "guides": [[0, .2, .4], [.5, .5, .9], [1, .2, .4]],
            "target_rows": [.3, .7], "horizontal_crop": [.05, .95],
            "strength": .47, "aspect_balance": .6}
    warp = RinkWarp(spec)
    x = np.linspace(0, 1, 29)
    y = np.linspace(0, 1, 101)[:, None]
    source_y = warp.inverse_y(x, y)
    self.assertTrue(np.all(np.diff(source_y, axis=0) > 0))
    np.testing.assert_allclose(warp.forward_y(x, source_y), np.broadcast_to(y, (101, 29)), atol=1e-12)
    up, low = warp.columns(x)
    top, bottom = warp.targets(x)
    np.testing.assert_allclose(warp.forward_y(x, up), top, atol=1e-12)
    np.testing.assert_allclose(warp.forward_y(x, low), bottom, atol=1e-12)


if __name__ == "__main__":
  unittest.main()
