#!/usr/bin/env python3
"""Offline, bounded panorama straightening. Does not modify game calibration.

The horizontal coordinate is fixed. Above the far guide the transform is the
identity; below the near guide it is a translation. Between the guides its
vertical derivative increases smoothly, stays constant, then returns to one.
The integral of that derivative places the near guide at the requested row,
unless the explicit stretch limit prevents it. No pixels are invented.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import cv2
import numpy as np


def number(value, name):
  if isinstance(value, bool) or not isinstance(value, (int, float)) or not np.isfinite(value):
    raise ValueError(f"{name} must be a finite number")
  return float(value)


class Curve:
  """Shape-preserving cubic Hermite interpolation (Fritsch/Butland slopes)."""

  def __init__(self, x, y):
    self.x, self.y = np.asarray(x, dtype=float), np.asarray(y, dtype=float)
    h = np.diff(self.x)
    secant = np.diff(self.y) / h
    self.slope = np.zeros_like(self.y)
    if len(x) == 2:
      self.slope[:] = secant[0]
      return
    for i in range(1, len(x) - 1):
      if secant[i - 1] * secant[i] > 0:
        w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
        self.slope[i] = (w1 + w2) / (w1 / secant[i - 1] + w2 / secant[i])
    for i, j, k in ((0, 0, 1), (-1, -1, -2)):
      slope = ((2 * h[j] + h[k]) * secant[j] - h[j] * secant[k]) / (h[j] + h[k])
      if slope * secant[j] <= 0:
        slope = 0.0
      elif secant[j] * secant[k] <= 0 and abs(slope) > 3 * abs(secant[j]):
        slope = 3 * secant[j]
      self.slope[i] = slope

  def __call__(self, x):
    index = np.clip(np.searchsorted(self.x, x, side="right") - 1, 0, len(self.x) - 2)
    h = self.x[index + 1] - self.x[index]
    t = (x - self.x[index]) / h
    return ((2 * t**3 - 3 * t**2 + 1) * self.y[index]
            + (t**3 - 2 * t**2 + t) * h * self.slope[index]
            + (-2 * t**3 + 3 * t**2) * self.y[index + 1]
            + (t**3 - t**2) * h * self.slope[index + 1])


class RinkWarp:
  def __init__(self, spec):
    keys = {"schema", "source_size", "guides", "target_y", "max_vertical_stretch",
            "transition_fraction", "limit_transition_fraction"}
    if not isinstance(spec, dict) or set(spec) != keys:
      raise ValueError(f"guide document must contain exactly: {', '.join(sorted(keys))}")
    if type(spec["schema"]) is not int or spec["schema"] != 1:
      raise ValueError("unsupported guide schema")
    size = spec["source_size"]
    if (not isinstance(size, list) or len(size) != 2
        or any(type(v) is not int or not 2 <= v < 32767 for v in size)):
      raise ValueError("source_size must contain width and height in [2, 32766]")
    self.source_size = tuple(size)
    guides = spec["guides"]
    if not isinstance(guides, list) or not 2 <= len(guides) <= 1024:
      raise ValueError("guides must contain 2 to 1024 [x, far_y, near_y] rows")
    rows = []
    for row in guides:
      if not isinstance(row, list) or len(row) != 3:
        raise ValueError("each guide must be [x, far_y, near_y]")
      x, far, near = [number(v, "guide coordinate") for v in row]
      if not (0 <= x <= 1 and 0 <= far < near < 1):
        raise ValueError("guides require 0 <= x <= 1 and 0 <= far_y < near_y < 1")
      rows.append((x, far, near))
    x, far, near = np.asarray(rows).T
    if x[0] != 0 or x[-1] != 1 or np.any(np.diff(x) <= 0):
      raise ValueError("guide x coordinates must strictly increase from 0 to 1")
    self.target = number(spec["target_y"], "target_y")
    self.limit = number(spec["max_vertical_stretch"], "max_vertical_stretch")
    self.transition = number(spec["transition_fraction"], "transition_fraction")
    self.limit_transition = number(spec["limit_transition_fraction"], "limit_transition_fraction")
    if not max(near) <= self.target <= 1:
      raise ValueError("target_y must be at or below every near guide, and at most 1")
    if not 1 <= self.limit <= 100:
      raise ValueError("max_vertical_stretch must be in [1, 100]")
    if not 0 < self.transition <= 0.5 or not 0 < self.limit_transition < 1:
      raise ValueError("transition_fraction must be in (0, .5]; limit_transition_fraction in (0, 1)")
    self.near = Curve(x, near)
    # Interpolating a ratio guarantees that the two guides never cross,
    # including between user marks, without clipping or a derivative break.
    self.far_ratio = Curve(x, far / near)

  def columns(self, x):
    x = np.asarray(x, dtype=float)
    if not np.all(np.isfinite(x)) or np.any((x < 0) | (x > 1)):
      raise ValueError("x coordinates must be finite and in [0, 1]")
    near = self.near(x)
    far = near * self.far_ratio(x)
    requested = np.maximum(0, self.target - near)
    capacity = (self.limit - 1) * (near - far) * (1 - self.transition)
    # C1 soft minimum: exact below the transition, capped above it. The
    # transition remains below both constraints, so it cannot exceed the
    # stretch budget or push the near boundary past the requested row.
    e = self.limit_transition * capacity
    blend = np.clip(requested - capacity + e, 0, 2 * e)
    denominator = np.where(e > 0, 4 * e, 1)
    displacement = np.minimum(requested, capacity) - np.where(
        requested <= capacity, blend**2 / denominator,
        (2 * e - blend)**2 / denominator)
    return far, near, np.maximum(0, displacement)

  def _integral(self, u):
    r = self.transition
    lower = np.minimum(u, 1 - u)
    t = np.minimum(lower / r, 1)
    value = np.where(lower < r, r * (t**3 - t**4 / 2) / (1 - r),
                     (lower - r / 2) / (1 - r))
    return np.where(u <= .5, value, 1 - value)

  def forward_y(self, x, y):
    far, near, displacement = self.columns(x)
    y = np.asarray(y, dtype=float)
    if not np.all(np.isfinite(y)):
      raise ValueError("y coordinates must be finite")
    u = np.clip((y - far) / (near - far), 0, 1)
    return y + displacement * self._integral(u)

  def inverse_y(self, x, y):
    far, near, displacement = self.columns(x)
    y, far, near, displacement = np.broadcast_arrays(np.asarray(y, dtype=float), far, near, displacement)
    if not np.all(np.isfinite(y)):
      raise ValueError("y coordinates must be finite")
    h, r = near - far, self.transition
    # The large central region has an exact linear inverse. Only the two
    # smooth shoulders need a numerical solve; their quartic is monotone.
    source = np.array(far + (y - far + displacement * r / (2 * (1 - r))) / (1 + displacement / (h * (1 - r))))
    distance = np.minimum(y - far, near + displacement - y)
    shoulder = (distance > 0) & (distance < h * r + displacement * r / (2 * (1 - r)))
    if np.any(shoulder):
      z, hr = distance[shoulder], h[shoulder] * r
      dr = displacement[shoulder] * r / (1 - r)
      low, high = np.zeros_like(z), np.ones_like(z)
      for _ in range(32):
        t = (low + high) / 2
        above = hr * t + dr * (t**3 - t**4 / 2) > z
        high = np.where(above, t, high)
        low = np.where(above, low, t)
      offset = hr * (low + high) / 2
      source[shoulder] = np.where(y[shoulder] - far[shoulder] <= (h[shoulder] + displacement[shoulder]) / 2,
                                 far[shoulder] + offset, near[shoulder] - offset)
    return np.where(y <= far, y, np.where(y >= near + displacement, y - displacement, source))

  def render(self, image, *, nearest=False):
    height, width = image.shape[:2]
    if (width, height) != self.source_size:
      raise ValueError("image dimensions do not match the guide document")
    output = np.empty_like(image)
    x = np.arange(width, dtype=float) / (width - 1)
    # Tile the coordinate maps. This is an offline still-image operation, not
    # a steady-state video path; no GPU video surfaces are read back.
    for start in range(0, height, 64):
      end = min(start + 64, height)
      y = np.arange(start, end, dtype=float)[:, None] / (height - 1)
      map_y = (self.inverse_y(x, y) * (height - 1)).astype(np.float32)
      map_x = np.broadcast_to(np.arange(width, dtype=np.float32), map_y.shape).copy()
      output[start:end] = cv2.remap(image, map_x, map_y,
                                    cv2.INTER_NEAREST if nearest else cv2.INTER_LINEAR)
    return output

  def report(self):
    width, height = self.source_size
    x = np.linspace(0, 1, width)
    far, near, displacement = self.columns(x)
    residual = (self.target - near - displacement) * (height - 1)
    peak_scale = 1 + displacement / ((near - far) * (1 - self.transition))
    return {"algorithm": "bounded-column-warp-v1", "source_size": [width, height],
            "max_vertical_stretch": float(peak_scale.max()),
            "max_near_guide_error_pixels": float(residual.max()),
            "columns_within_one_pixel_of_target_fraction": float(np.mean(residual <= 1)),
            "protected_region_max_displacement_pixels": 0.0,
            "horizontal_displacement_pixels": 0.0,
            "minimum_forward_jacobian_determinant": 1.0}


def read_image(path, flags):
  image = cv2.imread(str(path), flags)
  if image is None:
    raise ValueError(f"cannot decode {path}")
  return image


def write_image(path, image):
  if not cv2.imwrite(str(path), image):
    raise ValueError(f"cannot write {path}")


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("image", type=Path)
  parser.add_argument("--guides", type=Path, required=True, help="explicit normalized constraints (JSON)")
  parser.add_argument("--output-dir", type=Path, required=True, help="new directory; existing paths are refused")
  parser.add_argument("--comparison-width", type=int, required=True)
  parser.add_argument("--mask", type=Path, help="optional same-size ice mask for coverage measurements")
  args = parser.parse_args(argv)
  if not 320 <= args.comparison_width < 32767:
    parser.error("comparison-width must be in [320, 32766]")
  spec = json.loads(args.guides.read_text())
  warp = RinkWarp(spec)
  image_bytes = args.image.read_bytes()
  image = cv2.imdecode(np.frombuffer(image_bytes, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
  if image is None:
    raise ValueError(f"cannot decode {args.image}")
  if image.dtype != np.uint8 or image.ndim != 3 or image.shape[2] != 3:
    raise ValueError("the preview input must be an 8-bit, three-channel image")
  if image.shape[1::-1] != warp.source_size:
    raise ValueError("image dimensions do not match the guide document")
  mask = None
  if args.mask:
    mask = read_image(args.mask, cv2.IMREAD_UNCHANGED)
    if mask.shape != image.shape[:2]:
      raise ValueError("mask dimensions do not match the image")
  # Reserve the destination before any writes, including when it is a symlink.
  args.output_dir.mkdir()
  output = warp.render(image)
  report = warp.report()
  report["input_sha256"] = hashlib.sha256(image_bytes).hexdigest()
  write_image(args.output_dir / "warped.png", output)
  width = min(args.comparison_width, image.shape[1])
  height = max(1, round(image.shape[0] * width / image.shape[1]))
  panes = []
  for label, frame in (("Before", image), ("Constrained warp (same framing)", output)):
    pane = cv2.resize(frame, (width, height), interpolation=cv2.INTER_AREA)
    title = np.full((40, width, 3), (38, 30, 24), dtype=np.uint8)
    cv2.putText(title, label, (12, 28), cv2.FONT_HERSHEY_SIMPLEX, .7, (255, 255, 255), 1, cv2.LINE_AA)
    panes.extend((title, pane))
  write_image(args.output_dir / "comparison.jpg", np.concatenate(panes))
  if mask is not None:
    warped_mask = warp.render(mask, nearest=True)
    write_image(args.output_dir / "warped-mask.png", warped_mask)
    report["ice_fraction_before"] = float(np.mean(mask > 0))
    report["ice_fraction_after"] = float(np.mean(warped_mask > 0))
    outside = 0
    x = np.linspace(0, 1, image.shape[1])
    for start in range(0, image.shape[0], 64):
      end = min(start + 64, image.shape[0])
      y = np.arange(start, end)[:, None] / (image.shape[0] - 1)
      outside += int(np.count_nonzero((mask[start:end] > 0) & (warp.forward_y(x, y) > 1)))
    report["input_ice_pixels_mapped_outside_canvas"] = outside
  (args.output_dir / "guides.json").write_text(json.dumps(spec, indent=2) + "\n")
  (args.output_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
  print(json.dumps(report, indent=2))
  return 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except (ValueError, OSError, cv2.error) as error:
    print(f"constrained_rink_warp: {error}", file=sys.stderr)
    sys.exit(1)
