#!/usr/bin/env python3

from pathlib import Path
import tempfile
import unittest

try:
  import compare_detection_accuracy as accuracy
  import benchmark_model_precision as benchmark
except ModuleNotFoundError:
  from scripts import compare_detection_accuracy as accuracy
  from scripts import benchmark_model_precision as benchmark


def det(label="person", left=0.0, top=0.0, right=100.0, bottom=100.0, confidence=0.9):
  return accuracy.Detection(label, left, top, right, bottom, confidence)


def kitti_line(label, left, top, right, bottom, confidence):
  fillers = " ".join(["0.0"] * 7)
  return f"{label} 0.0 0 0.0 {left} {top} {right} {bottom} {fillers} {confidence}"


class ReplaceYamlScalarTest(unittest.TestCase):
  """Regression: a numeric value used to be swallowed by the group reference."""

  def test_numeric_value_is_not_read_as_a_group_reference(self):
    # rf"\1{value}" with value="2" yields \12 -> "invalid group reference 12".
    self.assertEqual(
        benchmark.replace_yaml_scalar("  network-mode: 0\n", "network-mode", "2"),
        "  network-mode: 2\n",
    )

  def test_every_single_digit_value(self):
    for value in "0123456789":
      self.assertEqual(
          benchmark.replace_yaml_scalar("  network-mode: 0\n", "network-mode", value),
          f"  network-mode: {value}\n",
      )

  def test_path_values_still_work(self):
    self.assertEqual(
        benchmark.replace_yaml_scalar("  onnx-file: old\n", "onnx-file", "/tmp/new.onnx"),
        "  onnx-file: /tmp/new.onnx\n",
    )

  def test_missing_key_raises(self):
    with self.assertRaises(ValueError):
      benchmark.replace_yaml_scalar("  a: 1\n", "network-mode", "2")


class LoadDetectionsTest(unittest.TestCase):
  def _load(self, lines):
    with tempfile.TemporaryDirectory() as tmp:
      Path(tmp, "00_000_000001.txt").write_text("\n".join(lines) + "\n")
      return accuracy.load_detections(Path(tmp))[(0, 1)]

  def test_single_word_label(self):
    (parsed,) = self._load([kitti_line("person", 10, 20, 110, 220, 0.75)])
    self.assertEqual(parsed.label, "person")
    self.assertEqual((parsed.left, parsed.top, parsed.right, parsed.bottom), (10, 20, 110, 220))
    self.assertAlmostEqual(parsed.confidence, 0.75)

  def test_multi_word_labels_do_not_shift_columns(self):
    """Regression: COCO has 13 multi-word labels; front-indexing misreads them."""
    parsed = self._load([
        kitti_line("sports ball", 30, 40, 130, 240, 0.61),
        kitti_line("traffic light", 50, 60, 150, 260, 0.52),
    ])
    self.assertEqual([p.label for p in parsed], ["sports ball", "traffic light"])
    for p in parsed:
      self.assertEqual(p.right - p.left, 100)
      self.assertEqual(p.bottom - p.top, 200)
    self.assertAlmostEqual(parsed[0].confidence, 0.61)
    self.assertAlmostEqual(parsed[1].confidence, 0.52)

  def test_short_and_unlabeled_lines_are_skipped(self):
    self.assertEqual(self._load(["person 1 2 3", " 0.0 0.0", kitti_line("person", 0, 0, 1, 1, 0.5)]),
                     self._load([kitti_line("person", 0, 0, 1, 1, 0.5)]))

  def test_missing_directory_is_empty_not_an_error(self):
    self.assertEqual(accuracy.load_detections(Path("/nonexistent-xyz")), {})


class MatchFrameTest(unittest.TestCase):
  def test_exact_match(self):
    pairs, missed, extra = accuracy.match_frame([det()], [det()], 0.5)
    self.assertEqual((len(pairs), len(missed), len(extra)), (1, 0, 0))
    self.assertAlmostEqual(pairs[0][2], 1.0)

  def test_below_threshold_is_a_miss_plus_an_extra(self):
    pairs, missed, extra = accuracy.match_frame([det()], [det(left=90, right=190)], 0.5)
    self.assertEqual((len(pairs), len(missed), len(extra)), (0, 1, 1))

  def test_class_flip_is_not_a_match(self):
    """A label flip on an identical box must not score as IoU 1.0."""
    pairs, missed, extra = accuracy.match_frame([det("person")], [det("sports ball")], 0.5)
    self.assertEqual((len(pairs), len(missed), len(extra)), (0, 1, 1))

  def test_each_box_is_used_at_most_once(self):
    ref = [det(left=0, right=100), det(left=10, right=110)]
    pairs, missed, extra = accuracy.match_frame(ref, [det(left=0, right=100)], 0.5)
    self.assertEqual((len(pairs), len(missed), len(extra)), (1, 1, 0))


class CompareTest(unittest.TestCase):
  def test_identity_is_perfect(self):
    frames = {(0, 1): [det(), det(left=500, right=600)]}
    m = accuracy.compare(frames, frames, 0.5, 0.25)
    self.assertEqual(m["recall_vs_ref_pct"], 100.0)
    self.assertEqual(m["precision_vs_ref_pct"], 100.0)
    self.assertEqual(m["f1_vs_ref_pct"], 100.0)
    self.assertEqual(m["total_disagreements"], 0)
    self.assertEqual(m["frames_with_count_delta"], 0)

  def test_precision_falls_when_the_candidate_over_detects(self):
    ref = {(0, 1): [det()]}
    cand = {(0, 1): [det(), det(left=500, right=600)]}
    m = accuracy.compare(ref, cand, 0.5, 0.25)
    # Recall alone would read 100% and hide the spurious box.
    self.assertEqual(m["recall_vs_ref_pct"], 100.0)
    self.assertEqual(m["precision_vs_ref_pct"], 50.0)
    self.assertEqual(m["total_disagreements"], 1)
    self.assertEqual(m["frames_with_count_delta"], 1)

  def test_confidence_floor_filters_both_sides(self):
    ref = {(0, 1): [det(confidence=0.30)]}
    m = accuracy.compare(ref, ref, 0.5, 0.5)
    self.assertEqual(m["ref_detections"], 0)
    # Undefined, not 0.0 -- a 0% here would read as a total detection failure.
    self.assertIsNone(m["recall_vs_ref_pct"])
    self.assertEqual(accuracy._num(m["recall_vs_ref_pct"], 8, 2).strip(), "n/a")

  def test_only_the_frame_intersection_is_compared(self):
    m = accuracy.compare({(0, 1): [det()], (0, 2): [det()]}, {(0, 1): [det()]}, 0.5, 0.25)
    self.assertEqual(m["frames_compared"], 1)
    self.assertEqual(m["frames_ref_only"], 1)
    self.assertEqual(m["frames_cand_only"], 0)


class InferConfigTest(unittest.TestCase):
  def test_fp32_uses_the_pipeline_default(self):
    self.assertIsNone(accuracy.infer_config_for("fp32"))

  def test_each_precision_resolves_to_a_committed_config(self):
    for precision in ("fp16", "int8", "bf16"):
      path = accuracy.infer_config_for(precision)
      self.assertIsNotNone(path)
      self.assertTrue(path.exists(), f"{precision} config missing: {path}")

  def test_unsupported_precision_raises(self):
    with self.assertRaises(ValueError):
      accuracy.infer_config_for("fp8")

  def test_fp16_config_actually_selects_fp16(self):
    text = accuracy.FP16_CONFIG.read_text()
    self.assertIn("network-mode: 2", text)


if __name__ == "__main__":
  unittest.main()
