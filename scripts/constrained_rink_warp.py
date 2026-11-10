#!/usr/bin/env python3
"""Offline line-constrained panorama straightening; leaves game calibration alone.

Two measured front rails map to horizontal lines. Monotone rational-quadratic
segments preserve upright posts. Horizontal spacing follows the vertical
scale between the rails, allowing a wider canvas instead of tall, thin people.
An explicit horizontal crop centers the rink; foreground below it may leave
frame. A supplied ice mask must remain entirely inside the output canvas.
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


def interval(z, x0, x1, y0, y1, d0, d1, *, inverse=False):
  """Monotone rational quadratic, or its analytic inverse.

  Durkan et al., Neural Spline Flows (2019), eq. 4 and appendix A.3.
  Positive interval lengths and derivatives guarantee strict monotonicity.
  Clamping only selects the endpoint outside this bin.
  """
  slope = (y1 - y0) / (x1 - x0)
  if inverse:
    q = np.clip((z - y0) / (y1 - y0), 0, 1)
    reflected = q > .5
    q = np.minimum(q, 1 - q)
    d0, d1 = np.where(reflected, d1, d0), np.where(reflected, d0, d1)
    a = q * (2 * slope - d0 - d1) + d0 - slope
    b = q * (d0 + d1 - 2 * slope) - d0
    c = q * slope
    root = np.sqrt(np.maximum(0, b * b - 4 * a * c))
    # Select the equivalent root formula without subtracting similar values.
    t = np.where(b > 0, -b - root, 2 * c) / np.where(b > 0, 2 * a, -b + root)
    return x0 + (x1 - x0) * np.where(reflected, 1 - t, t)
  t = np.clip((z - x0) / (x1 - x0), 0, 1)
  product = t * (1 - t)
  return y0 + (y1 - y0) * (slope * t * t + d0 * product) / (slope + (d0 + d1 - 2 * slope) * product)


class RinkWarp:
  def __init__(self, spec):
    keys = {"schema", "source_size", "guides", "target_rows", "horizontal_crop"}
    if not isinstance(spec, dict) or set(spec) != keys:
      raise ValueError(f"guide document must contain exactly: {', '.join(sorted(keys))}")
    if type(spec["schema"]) is not int or spec["schema"] != 2:
      raise ValueError("unsupported guide schema; redraw schema-1 far/near guides as two front rails")
    size = spec["source_size"]
    if (not isinstance(size, list) or len(size) != 2
        or any(type(v) is not int or not 2 <= v < 32767 for v in size)):
      raise ValueError("source_size must contain width and height in [2, 32766]")
    self.source_size = tuple(size)
    guides = spec["guides"]
    if not isinstance(guides, list) or not 2 <= len(guides) <= 1024:
      raise ValueError("guides must contain 2 to 1024 [x, upper_y, lower_y] rows")
    rows = []
    for row in guides:
      if not isinstance(row, list) or len(row) != 3:
        raise ValueError("each guide must be [x, upper_y, lower_y]")
      x, upper, lower = [number(v, "guide coordinate") for v in row]
      # Minimum separations protect numerical conditioning, not aesthetics.
      if not (0 <= x <= 1 and 1e-4 <= upper <= lower - 1e-4 and lower <= 1 - 1e-4):
        raise ValueError("guides must be ordered inside the frame, separated by at least 0.0001")
      rows.append((x, upper, lower))
    x, upper, lower = np.asarray(rows).T
    if x[0] != 0 or x[-1] != 1 or np.any(np.diff(x) < 1e-4):
      raise ValueError("guide x coordinates must span 0 to 1, separated by at least 0.0001")
    targets, crop = spec["target_rows"], spec["horizontal_crop"]
    if not isinstance(targets, list) or len(targets) != 2 or not isinstance(crop, list) or len(crop) != 2:
      raise ValueError("target_rows and horizontal_crop must each contain two numbers")
    self.top, self.bottom = [number(v, "target row") for v in targets]
    self.left, self.right = [number(v, "crop bound") for v in crop]
    if not (1e-4 <= self.top <= self.bottom - 1e-4 and max(lower) <= self.bottom <= 1 - 1e-4):
      raise ValueError("target rows must be ordered inside the frame; lower target must be at/below all lower marks")
    if not (0 <= self.left <= self.right - 1e-4 and self.right <= 1):
      raise ValueError("horizontal_crop must be an increasing interval within [0, 1]")
    self.lower = Curve(x, lower)
    # Interpolating the ratio prevents guide crossings between marks.
    self.upper_ratio = Curve(x, upper / lower)
    # Match horizontal expansion to mean vertical expansion between the rails.
    # Integrating positive scales gives an ordered map and the required canvas
    # width; keeping the original width would squash that geometry afterward.
    self.source_x = np.linspace(self.left, self.right, size[0])
    up, low = self.columns(self.source_x)
    scales = (self.bottom - self.top) / (low - up)
    integral = np.r_[0, np.cumsum((scales[1:] + scales[:-1]) * np.diff(self.source_x) / 2)]
    output_width = 1 + round(float(integral[-1]) * (size[0] - 1))
    if not 2 <= output_width < 32767:
      raise ValueError("shape-preserving output width is outside OpenCV's [2, 32766] range; relax the constraints")
    self.output_x = integral / integral[-1]
    self.output_size = (output_width, size[1])

  def columns(self, source_x):
    x = np.asarray(source_x, dtype=float)
    if not np.all(np.isfinite(x)) or np.any((x < 0) | (x > 1)):
      raise ValueError("source x coordinates must be finite and in [0, 1]")
    lower = self.lower(x)
    return lower * self.upper_ratio(x), lower

  def forward_x(self, source_x):
    x = np.asarray(source_x, dtype=float)
    if not np.all(np.isfinite(x)):
      raise ValueError("x coordinates must be finite")
    result = np.interp(x, self.source_x, self.output_x)
    # Preserve out-of-crop classification rather than clamping it in-frame.
    return np.where(x < self.left, (x - self.left) / (self.right - self.left),
                    np.where(x > self.right, 1 + (x - self.right) / (self.right - self.left), result))

  def inverse_x(self, output_x):
    x = np.asarray(output_x, dtype=float)
    if not np.all(np.isfinite(x)) or np.any((x < 0) | (x > 1)):
      raise ValueError("output x coordinates must be finite and in [0, 1]")
    return np.interp(x, self.output_x, self.source_x)

  def segments(self, source_x):
    upper, lower = self.columns(source_x)
    first = upper / self.top
    middle = (lower - upper) / (self.bottom - self.top)
    shared = 2 * first * middle / (first + middle)
    # Below the boards continue the local scale. If that would sample beyond
    # the input, a final monotone segment reaches the image boundary instead.
    end = np.minimum(1, lower + (1 - self.bottom) * middle)
    return upper, lower, first, middle, shared, end

  def inverse_y(self, source_x, output_y):
    """Source y at output_y; x is ALWAYS in source-image coordinates."""
    upper, lower, first, middle, shared, end = self.segments(source_x)
    y = np.asarray(output_y, dtype=float)
    if not np.all(np.isfinite(y)):
      raise ValueError("y coordinates must be finite")
    return np.where(y <= 0, y * first,
        np.where(y < self.top, interval(y, 0, self.top, 0, upper, first, shared),
        np.where(y < self.bottom, interval(y, self.top, self.bottom, upper, lower, shared, middle),
        np.where(y <= 1, interval(y, self.bottom, 1, lower, end, middle, middle),
                 end + (y - 1) * middle))))

  def forward_y(self, source_x, source_y):
    upper, lower, first, middle, shared, end = self.segments(source_x)
    y = np.asarray(source_y, dtype=float)
    if not np.all(np.isfinite(y)):
      raise ValueError("y coordinates must be finite")
    return np.where(y <= 0, y / first,
        np.where(y < upper, interval(y, 0, self.top, 0, upper, first, shared, inverse=True),
        np.where(y < lower, interval(y, self.top, self.bottom, upper, lower, shared, middle, inverse=True),
        np.where(y <= end, interval(y, self.bottom, 1, lower, end, middle, middle, inverse=True),
                 1 + (y - end) / middle))))

  def render(self, image, *, nearest=False):
    height, width = image.shape[:2]
    if (width, height) != self.source_size:
      raise ValueError("image dimensions do not match the guide document")
    width, height = self.output_size
    output = np.empty((height, width) + image.shape[2:], dtype=image.dtype)
    source_x = self.inverse_x(np.linspace(0, 1, width))
    # Offline still remap only; never reads GPU playback surfaces. Tiling
    # bounds temporary memory instead of allocating two full-size maps.
    for start in range(0, height, 64):
      end = min(start + 64, height)
      y = np.arange(start, end, dtype=float)[:, None] / (height - 1)
      map_y = (self.inverse_y(source_x, y) * (height - 1)).astype(np.float32)
      map_x = np.broadcast_to((source_x * (self.source_size[0] - 1)).astype(np.float32), map_y.shape).copy()
      output[start:end] = cv2.remap(image, map_x, map_y,
                                    cv2.INTER_NEAREST if nearest else cv2.INTER_LINEAR)
    return output

  def mask_report(self, mask):
    height, width = mask.shape
    if (width, height) != self.source_size:
      raise ValueError("mask dimensions do not match the guide document")
    outside, clearance = 0, float("inf")
    x = np.linspace(0, 1, width)
    out_x = self.forward_x(x)
    for start in range(0, height, 64):
      end = min(start + 64, height)
      y = np.arange(start, end)[:, None] / (height - 1)
      out_y = self.forward_y(x, y)
      selected = mask[start:end] > 0
      outside += int(np.count_nonzero(selected & ((out_x < 0) | (out_x > 1) | (out_y < 0) | (out_y > 1))))
      if np.any(selected):
        margin = np.minimum(np.minimum(out_x, 1 - out_x) * (self.output_size[0] - 1),
                            np.minimum(out_y, 1 - out_y) * (height - 1))
        clearance = min(clearance, float(margin[selected].min()))
    return {"input_ice_pixels_mapped_outside_canvas": outside,
            "minimum_ice_clearance_pixels": clearance if np.isfinite(clearance) else None}

  def report(self):
    width, height = self.output_size
    x = self.inverse_x(np.linspace(0, 1, width))
    upper, lower = self.columns(x)
    # Report sampled scales honestly; positivity follows from the spline's
    # positive lengths/derivatives, not from this diagnostic grid.
    sx = self.inverse_x(np.linspace(0, 1, 1025))
    y = np.linspace(0, 1, 2049)[:, None]
    scale = np.diff(self.forward_y(sx, y), axis=0) * 2048
    horizontal = np.diff(self.output_x) / np.diff(self.source_x) * (width - 1) / (self.source_size[0] - 1)
    return {"algorithm": "two-rail-aspect-warp-v2", "source_size": list(self.source_size),
            "output_size": list(self.output_size),
            "horizontal_crop": [self.left, self.right], "target_rows": [self.top, self.bottom],
            "front_margin_pixels": (1 - self.bottom) * (height - 1),
            "max_guide_error_pixels": float(max(np.max(np.abs(self.forward_y(x, upper) - self.top)),
                                                np.max(np.abs(self.forward_y(x, lower) - self.bottom))) * (height - 1)),
            "sampled_minimum_vertical_scale": float(scale.min()),
            "sampled_maximum_vertical_scale": float(scale.max()),
            "minimum_horizontal_scale": float(horizontal.min()),
            "maximum_horizontal_scale": float(horizontal.max()),
            "foreground_policy": "continue local scale below boards; crop outside canvas"}


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
  report = warp.report()
  if mask is not None:
    report.update(warp.mask_report(mask))
    if report["input_ice_pixels_mapped_outside_canvas"]:
      raise ValueError("warp/crop would put marked ice outside the canvas; adjust the guides or crop")
  # Reserve the destination before any writes, including when it is a symlink.
  args.output_dir.mkdir()
  output = warp.render(image)
  report["input_sha256"] = hashlib.sha256(image_bytes).hexdigest()
  write_image(args.output_dir / "warped.png", output)
  width = min(args.comparison_width, image.shape[1])
  panes = []
  for label, frame in (("Before", image), ("After: level front rails; width expanded to preserve proportions", output)):
    height = max(1, round(frame.shape[0] * width / frame.shape[1]))
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
