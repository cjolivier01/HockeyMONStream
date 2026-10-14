#!/usr/bin/env python3
"""Reproduce the bundled, approximate lens presets from pinned local Gyroflow data.

This developer tool does not read videos or download anything. The runtime Auto
importer deliberately retains its stricter accuracy and recording-mode checks.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from urllib.parse import quote

BEGIN = "    # BEGIN GENERATED CAMERA LENS CATALOG"
END = "    # END GENERATED CAMERA LENS CATALOG"
MAX_ERROR_AT_8K = 8.0
FIT_SAMPLES = 4096
ERROR_SAMPLES = 16384
GRID_SAMPLES = 80


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, name):
    require(
        isinstance(value, (int, float)) and not isinstance(value, bool),
        f"Invalid {name}",
    )
    require(math.isfinite(value), f"Nonfinite {name}")
    return float(value)


def dimensions(value):
    require(isinstance(value, dict), "Missing calibration dimensions")
    w, h = value.get("w"), value.get("h")
    require(
        all(
            isinstance(x, int) and not isinstance(x, bool) and 16 <= x <= 32768
            for x in (w, h)
        ),
        "Invalid calibration dimensions",
    )
    return w, h


def effective_camera(source, recipe):
    """Implement only the explicit, static modes used by the bundled manifest."""
    w, h = dimensions(source.get("calib_dimension"))
    require(
        dimensions(source.get("orig_dimension")) == (w, h),
        "Unsupported original image dimensions",
    )
    mode = dict(source)
    selected = recipe.get("compatible_identifier")
    if selected:
        matches = [
            x
            for x in source.get("compatible_settings", [])
            if x.get("identifier") == selected
        ]
        require(
            len(matches) == 1,
            "Compatible identifier must identify exactly one source setting",
        )
        override = matches[0]
        require(
            set(override)
            <= {"identifier", "width", "height", "fps", "crop", "lens_model"},
            "Unsupported compatible-setting transform",
        )
        require(
            (override.get("width"), override.get("height")) == (w, h),
            "Compatible-setting resize/crop requires a separately calibrated source",
        )
        mode.update(override)
    require(isinstance(mode.get("identifier", ""), str), "Invalid mode identifier")
    require(
        mode.get("distortion_model") in (None, "opencv_fisheye")
        and mode.get("distortion_model_id", 0) == 0,
        "Unsupported distortion model",
    )
    require(
        mode.get("is_superview", False) is False, "Unsupported legacy SuperView lens"
    )
    require(
        mode.get("digital_lens") in (None, "") and not mode.get("digital_lens_params"),
        "Unsupported digital lens",
    )
    require(
        not mode.get("interpolations"),
        "Variable lens interpolation cannot be a static preset",
    )
    require(
        all(
            number(mode.get(key, 1), key) == 1
            for key in ("input_horizontal_stretch", "input_vertical_stretch")
        ),
        "Unsupported image stretch",
    )
    crop = number(mode.get("crop") if mode.get("crop") is not None else 1, "crop")
    require(0 < crop <= 1, "Unsupported crop")
    params = mode.get("fisheye_params", {})
    require(
        params.get("radial_distortion_limit") is None,
        "Unsupported explicit radial distortion limit",
    )
    matrix = params.get("camera_matrix")
    require(
        isinstance(matrix, list)
        and len(matrix) == 3
        and all(isinstance(row, list) and len(row) == 3 for row in matrix),
        "Invalid camera matrix",
    )
    matrix = [[number(v, "camera matrix") for v in row] for row in matrix]
    require(
        matrix[0][1] == matrix[1][0] == 0 and matrix[2] == [0, 0, 1],
        "Unsupported skewed camera matrix",
    )
    fx, fy = matrix[0][0] / crop, matrix[1][1] / crop
    require(fx > 0 and fy > 0, "Invalid focal length")
    asymmetrical = mode.get("asymmetrical", False)
    require(isinstance(asymmetrical, bool), "Invalid asymmetrical flag")
    cx, cy = (matrix[0][2], matrix[1][2]) if asymmetrical else (w / 2, h / 2)
    require(0 < cx < w and 0 < cy < h, "Optical center must lie within the image")
    coefficients = params.get("distortion_coeffs")
    require(
        isinstance(coefficients, list) and len(coefficients) == 4,
        "Expected four OpenCV fisheye coefficients",
    )
    coefficients = [number(v, "distortion coefficient") for v in coefficients]
    rms = number(params.get("RMS_error"), "source RMS error")
    require(rms >= 0, "Invalid source RMS error")
    return {
        "width": w,
        "height": h,
        "fx": fx,
        "fy": fy,
        "cx": cx,
        "cy": cy,
        "coefficients": coefficients,
        "identifier": mode.get("identifier", ""),
        "crop": crop,
        "source_name": source.get("name", ""),
        "source_note": source.get("note", ""),
        "source_rms_px": rms,
        "calibrated_by": source.get("calibrated_by", ""),
        "fps": number(mode.get("fps"), "fps"),
    }


def polynomial(coefficients, x):
    value = 0.0
    for coefficient in reversed(coefficients):
        value = value * x + coefficient
    return value


def bisect(function, target, high):
    low = 0.0
    endpoint = function(high)
    require(
        function(0) <= target <= endpoint + 32 * math.ulp(endpoint),
        "Lens does not cover requested image radius",
    )
    if target == 0:
        return 0.0
    if target >= endpoint:
        return high  # Only endpoint rounding is tolerated; there is no extrapolation.
    for _ in range(52):
        middle = (low + high) / 2
        if function(middle) < target:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def solve(system):
    """Small pivoted system; fit arguments are normalized to keep it conditioned."""
    for col in range(4):
        pivot = max(range(col, 4), key=lambda row: abs(system[row][col]))
        system[col], system[pivot] = system[pivot], system[col]
        scale = system[col][col]
        require(abs(scale) > 1e-12, "Degenerate radial fit")
        system[col] = [value / scale for value in system[col]]
        for row in range(4):
            if row != col:
                scale = system[row][col]
                system[row] = [a - scale * b for a, b in zip(system[row], system[col])]
    return [row[4] for row in system]


def fit_radial(radius, theta_max, w, h, projection):
    require(
        projection in ("full-frame-fisheye", "rectilinear"),
        "Unsupported target projection",
    )
    require(
        projection != "rectilinear" or theta_max < math.pi / 2,
        "Rectilinear fit crosses 90 degrees",
    )
    coordinate = (lambda t: t) if projection == "full-frame-fisheye" else math.tan
    extent, normalizer = radius(theta_max), min(w, h) / 2
    maximum = coordinate(theta_max)
    system = [[0.0] * 5 for _ in range(4)]
    for i in range(FIT_SAMPLES + 1):
        r = extent * i / FIT_SAMPLES
        x = coordinate(bisect(radius, r, theta_max)) / maximum
        powers = [x**j for j in range(1, 9)]
        for a in range(4):
            for b in range(4):
                system[a][b] += powers[a + b + 1]
            system[a][4] += powers[a] * r / normalizer
    q = solve(system)
    scale = bisect(lambda x: polynomial([0] + q, x), 1, 1)
    # Hugin fixes d = 1-a-b-c. Solve the scale where the fitted polynomial is 1.
    a, b, c = q[3] * scale**4, q[2] * scale**3, q[1] * scale**2
    a, b, c = (0.0 if abs(v) < 1e-10 else v for v in (a, b, c))
    focal = normalizer / (scale * maximum)
    hfov = (
        w / focal
        if projection == "full-frame-fisheye"
        else 2 * math.atan(w / (2 * focal))
    )
    target = [0, 1 - a - b - c, c, b, a]

    def fitted(theta):
        return normalizer * polynomial(target, focal * coordinate(theta) / normalizer)

    # A global bound on |d² fitted_radius/dtheta²| bounds interpolation error.
    u = coordinate(theta_max)
    scaled = [
        value * normalizer * (focal / normalizer) ** i for i, value in enumerate(target)
    ]

    def fitted_derivative(theta):
        u = coordinate(theta)
        factor = 1 + u * u if projection == "rectilinear" else 1
        return polynomial([i * scaled[i] for i in range(1, 5)], u) * factor

    first = sum(i * abs(scaled[i]) * u ** (i - 1) for i in range(1, 5))
    second = sum(i * (i - 1) * abs(scaled[i]) * u ** (i - 2) for i in range(2, 5))
    if projection == "rectilinear":
        sec2 = 1 + u * u
        second = second * sec2 * sec2 + first * 2 * u * sec2
    return {
        "horizontal_fov": math.degrees(hfov),
        "distortion": [a, b, c],
        "focal": focal,
        "radius": fitted,
        "derivative": fitted_derivative,
        "second_derivative_bound": second,
    }


def convert(camera, projection):
    w, h = camera["width"], camera["height"]
    fx, fy, cx, cy = (camera[key] for key in ("fx", "fy", "cx", "cy"))
    focal = (fx + fy) / 2
    k = camera["coefficients"]
    source = [
        0,
        focal,
        0,
        focal * k[0],
        0,
        focal * k[1],
        0,
        focal * k[2],
        0,
        focal * k[3],
    ]
    derivative = [i * source[i] for i in range(1, len(source))]

    def radius(theta):
        return polynomial(source, theta)

    # Include every corner in normalized fx/fy space, even if the center is offset.
    extent = focal * math.hypot(max(cx, w - cx) / fx, max(cy, h - cy) / fy)
    high = 0.0
    for i in range(1, 16385):
        high = math.pi * i / 16384
        require(polynomial(derivative, high) > 0, "Nonmonotonic source lens model")
        if radius(high) >= extent:
            break
    require(radius(high) >= extent, "Source lens does not cover the image")
    theta_max = bisect(radius, extent, high)
    source_second = sum(
        i * (i - 1) * abs(source[i]) * theta_max ** (i - 2)
        for i in range(2, len(source))
    )
    step = theta_max / ERROR_SAMPLES
    min_derivative = min(
        polynomial(derivative, step * i) for i in range(ERROR_SAMPLES + 1)
    )
    require(
        min_derivative > source_second * step / 2,
        "Source lens monotonicity cannot be established",
    )
    fit = fit_radial(radius, theta_max, w, h, projection)
    target = fit["radius"]
    errors = [
        abs(target(step * i) - radius(step * i)) for i in range(ERROR_SAMPLES + 1)
    ]
    # Between grid samples, linear interpolation differs by at most M*step²/8.
    radial_bound = (
        max(errors) + (source_second + fit["second_derivative_bound"]) * step * step / 8
    )
    anisotropy_bound = max(abs(fx - focal), abs(fy - focal)) * extent / focal
    bound = radial_bound + anisotropy_bound
    require(
        bound * 7680 / w <= MAX_ERROR_AT_8K,
        f"Conversion bound {bound * 7680 / w:.3f}px at 8K exceeds {MAX_ERROR_AT_8K:g}px",
    )
    target_min_derivative = min(
        fit["derivative"](step * i) for i in range(ERROR_SAMPLES + 1)
    )
    require(
        target_min_derivative > fit["second_derivative_bound"] * step / 2,
        "Fitted lens monotonicity cannot be established",
    )
    pixel_errors = []
    for row in range(GRID_SAMPLES + 1):
        for col in range(GRID_SAMPLES + 1):
            dx, dy = w * col / GRID_SAMPLES - cx, h * row / GRID_SAMPLES - cy
            nx, ny = dx / fx, dy / fy
            norm = math.hypot(nx, ny)
            if norm == 0:
                pixel_errors.append(0.0)
                continue
            theta = bisect(radius, focal * norm, theta_max)
            out = target(theta) / norm
            pixel_errors.append(math.hypot(out * nx - dx, out * ny - dy))
    center = [(cx - w / 2) / w, (cy - h / 2) / h]
    vertical = math.degrees(
        bisect(radius, focal * cy / fy, theta_max)
        + bisect(radius, focal * (h - cy) / fy, theta_max)
    )
    require(
        all(abs(v) <= 1 for v in fit["distortion"]),
        "Hugin coefficient exceeds supported range",
    )
    require(
        0 < fit["horizontal_fov"] < 360 and 0 < vertical <= 180,
        "Unsupported field of view",
    )
    return {
        "horizontal_fov": fit["horizontal_fov"],
        "vertical_fov": vertical,
        "projection": projection,
        "distortion": fit["distortion"],
        "center_offset": center,
        "conversion_max_px": max(pixel_errors),
        "conversion_rms_px": math.sqrt(
            sum(v * v for v in pixel_errors) / len(pixel_errors)
        ),
        "conversion_bound_px": bound,
        "conversion_bound_px_at_8k": bound * 7680 / w,
        "radial_bound_px": radial_bound,
        "anisotropy_bound_px": anisotropy_bound,
    }


def read_catalog(manifest_path):
    manifest = json.loads(manifest_path.read_text())
    repository = manifest["upstream_repository"]
    revision = manifest["upstream_revision"]
    require(
        repository == "https://github.com/gyroflow/lens_profiles",
        "Unsupported source repository",
    )
    require(
        re.fullmatch(r"[0-9a-f]{40}", revision) is not None,
        "Expected immutable upstream revision",
    )
    records, seen = [], set()
    for recipe in manifest["profiles"]:
        identifier = recipe["id"]
        require(
            re.fullmatch(r"[a-z0-9][a-z0-9-]*", identifier) and identifier not in seen,
            "Invalid or duplicate profile id",
        )
        seen.add(identifier)
        filename = recipe["source_file"]
        require(
            isinstance(filename, str) and Path(filename).name == filename,
            "Source filename must be a basename",
        )
        raw = (manifest_path.parent / "sources" / filename).read_bytes()
        require(
            hashlib.sha256(raw).hexdigest() == recipe["source_sha256"],
            f"Source SHA256 mismatch: {filename}",
        )
        source = json.loads(raw)
        camera = effective_camera(source, recipe)
        result = convert(camera, recipe["projection"])
        source_path = recipe["source_path"]
        require(
            isinstance(source_path, str)
            and not source_path.startswith("/")
            and all(part not in (".", "..", "") for part in source_path.split("/")),
            "Invalid upstream source path",
        )
        records.append(
            {
                **recipe,
                "upstream_revision": revision,
                "source_url": f"{repository}/blob/{revision}/{quote(source_path, safe='/')}",
                "effective_camera": camera,
                "conversion": result,
            }
        )
    require(records, "Catalog must contain profiles")
    return records


def scalar(value):
    return format(value, ".12g")


def render(records):
    lines = [
        BEGIN,
        "    # Generated by scripts/convert_camera_lens_catalog.py; do not edit this block.",
        "    # Gyroflow measured approximations (CC0); mode-specific starting values for refinement.",
    ]
    for record in records:
        lens, camera = record["conversion"], record["effective_camera"]
        lines.extend(
            [
                f"    {record['id']}:",
                f"      display_name: {json.dumps(record['display_name'], ensure_ascii=False)}",
                f"      # Source: {record['source_file']}; {camera['identifier'] or camera['source_name']}",
            ]
        )
        for line in record["notes"].splitlines():
            lines.append(f"      # {line}")
        lines.extend(
            [
                f"      # Source RMS {camera['source_rms_px']:.3f}px; conversion max/RMS "
                f"{lens['conversion_max_px']:.3f}/{lens['conversion_rms_px']:.3f}px.",
                f"      # Conservative conversion bound {lens['conversion_bound_px_at_8k']:.3f}px at 7680px width.",
                f"      horizontal_fov: {scalar(lens['horizontal_fov'])}",
                f"      vertical_fov: {scalar(lens['vertical_fov'])}",
                "      input_lens:",
                f"        projection: {lens['projection']}",
                "        optimization: refine",
                f"        distortion: [{', '.join(map(scalar, lens['distortion']))}]",
                f"        center_offset: [{', '.join(map(scalar, lens['center_offset']))}]",
            ]
        )
    return "\n".join(lines + [END])


def replace_block(text, generated):
    require(
        text.count(BEGIN) == text.count(END) == 1,
        "Expected one generated catalog block in baseline",
    )
    start, end_marker = text.index(BEGIN), text.index(END)
    require(start < end_marker, "Invalid generated block markers")
    end = end_marker + len(END)
    return text[:start] + generated + text[end:]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument(
        "--manifest", type=Path, default=root / "configs/camera-lenses/catalog.json"
    )
    parser.add_argument("--baseline", type=Path, default=root / "configs/baseline.yaml")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--write", action="store_true", help="Replace only the generated baseline block"
    )
    mode.add_argument(
        "--check", action="store_true", help="Check generated baseline block (default)"
    )
    parser.add_argument(
        "--report",
        type=Path,
        help="Write source provenance and conversion measurements as JSON",
    )
    args = parser.parse_args(argv)
    try:
        records = read_catalog(args.manifest)
        original = args.baseline.read_bytes().decode("utf-8")
        generated = replace_block(original, render(records))
        if args.write:
            args.baseline.write_bytes(generated.encode("utf-8"))
        elif original != generated:
            raise ValueError("Bundled lens catalog is stale; run with --write")
        if args.report:
            args.report.write_text(
                json.dumps(records, indent=2, ensure_ascii=False) + "\n"
            )
        print(
            f"{'Wrote' if args.write else 'Verified'} {len(records)} camera lens presets"
        )
        return 0
    except (ValueError, KeyError, TypeError, OSError, OverflowError) as error:
        print(f"Lens catalog: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
