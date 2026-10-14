"""Checks mathematical conversion and reproducibility of the bundled lens data."""

import copy
import hashlib
import json
import math
from pathlib import Path
import tempfile
import unittest

import convert_camera_lens_catalog as catalog

ROOT = Path(__file__).resolve().parent.parent


def source_profile():
    return {
        "name": "Synthetic camera",
        "identifier": "synthetic-off",
        "fps": 30,
        "calib_dimension": {"w": 1920, "h": 1080},
        "orig_dimension": {"w": 1920, "h": 1080},
        "fisheye_params": {
            "camera_matrix": [[1000, 0, 910], [0, 1000, 520], [0, 0, 1]],
            "distortion_coeffs": [0, 0, 0, 0],
            "RMS_error": 0,
        },
    }


def model_pixel(camera, conversion, x, y):
    """Independent ray inversion and Hugin projection, including actual fx/fy."""
    nx, ny = (x - camera["cx"]) / camera["fx"], (y - camera["cy"]) / camera["fy"]
    radius = math.hypot(nx, ny)
    if radius == 0:
        return camera["cx"], camera["cy"]
    # Newton's method is independent of the converter's bisection.
    theta = radius
    k = camera["coefficients"]
    for _ in range(30):
        distorted = theta + sum(k[i] * theta ** (2 * i + 3) for i in range(4))
        derivative = 1 + sum(
            (2 * i + 3) * k[i] * theta ** (2 * i + 2) for i in range(4)
        )
        step = (distorted - radius) / derivative
        theta -= step
        if abs(step) < 1e-14:
            break
    width, height = camera["width"], camera["height"]
    horizontal = math.radians(conversion["horizontal_fov"])
    if conversion["projection"] == "full-frame-fisheye":
        focal = width / horizontal
        ideal = focal * theta
    else:
        focal = width / (2 * math.tan(horizontal / 2))
        ideal = focal * math.tan(theta)
    normalizer = min(width, height) / 2
    r = ideal / normalizer
    a, b, c = conversion["distortion"]
    mapped = normalizer * r * ((1 - a - b - c) + r * (c + r * (b + r * a)))
    center_x = width * (0.5 + conversion["center_offset"][0])
    center_y = height * (0.5 + conversion["center_offset"][1])
    return center_x + mapped * nx / radius, center_y + mapped * ny / radius


class LensConversionTest(unittest.TestCase):
    def test_metadata_modes_have_identical_geometry_and_qualified_rates(self):
        records = catalog.read_catalog(ROOT / "configs/camera-lenses/catalog.json")
        modes = [r for r in records if r.get("metadata_match")]
        self.assertEqual(sum(len(r["metadata_match"]["frame_rate"]) for r in modes), 20)
        for record in modes:
            self.assertEqual(record["metadata_match"]["horizon"], "Off")
            self.assertEqual(record["metadata_match"]["zoom"], "1")
            self.assertTrue(record["equivalent_identifiers"])

    def test_ideal_fisheye_and_source_center_semantics(self):
        camera = catalog.effective_camera(source_profile(), {})
        self.assertEqual((camera["cx"], camera["cy"]), (960, 540))
        result = catalog.convert(camera, "full-frame-fisheye")
        self.assertAlmostEqual(result["horizontal_fov"], math.degrees(1.92), places=6)
        self.assertAlmostEqual(result["vertical_fov"], math.degrees(1.08), places=6)
        self.assertLess(max(abs(x) for x in result["distortion"]), 1e-8)
        self.assertLess(result["conversion_bound_px"], 1e-6)
        for x, y in [(0, 0), (1920, 1080), (230, 870)]:
            ox, oy = model_pixel(camera, result, x, y)
            self.assertAlmostEqual(ox, x, places=6)
            self.assertAlmostEqual(oy, y, places=6)

    def test_ideal_rectilinear_projection(self):
        focal, width, height = 800, 1920, 1080
        theta_max = math.atan(math.hypot(width, height) / (2 * focal))
        fit = catalog.fit_radial(
            lambda t: focal * math.tan(t), theta_max, width, height, "rectilinear"
        )
        self.assertAlmostEqual(
            fit["horizontal_fov"],
            math.degrees(2 * math.atan(width / (2 * focal))),
            places=6,
        )
        self.assertLess(max(abs(x) for x in fit["distortion"]), 1e-8)
        for theta in (0, 0.1, 0.6, theta_max):
            self.assertAlmostEqual(
                fit["radius"](theta), focal * math.tan(theta), places=6
            )

    def test_offset_anisotropic_corners_and_rectilinear_conversion(self):
        source = source_profile()
        source["asymmetrical"] = True
        source["fisheye_params"]["camera_matrix"] = [
            [10000, 0, 980],
            [0, 10000.5, 540],
            [0, 0, 1],
        ]
        # First four terms of tan(theta)/theta produce an almost pinhole model at this narrow FOV.
        source["fisheye_params"]["distortion_coeffs"] = [
            1 / 3,
            2 / 15,
            17 / 315,
            62 / 2835,
        ]
        camera = catalog.effective_camera(source, {})
        result = catalog.convert(camera, "rectilinear")
        self.assertAlmostEqual(result["center_offset"][0], 20 / 1920)
        for x, y in [(0, 0), (1920, 0), (0, 1080), (1920, 1080), (400, 700)]:
            ox, oy = model_pixel(camera, result, x, y)
            self.assertLessEqual(
                math.hypot(ox - x, oy - y), result["conversion_bound_px"] + 1e-6
            )

    def test_compatible_crop_requires_explicit_mode(self):
        source = source_profile()
        source["compatible_settings"] = [
            {
                "identifier": "synthetic-eis",
                "width": 1920,
                "height": 1080,
                "fps": 60,
                "crop": 0.8,
            }
        ]
        result = catalog.effective_camera(
            source, {"compatible_identifier": "synthetic-eis"}
        )
        self.assertEqual(
            (result["fx"], result["fy"], result["cx"], result["cy"]),
            (1250, 1250, 960, 540),
        )
        self.assertEqual(result["fps"], 60)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            catalog.effective_camera(source, {"compatible_identifier": "missing"})
        source["compatible_settings"][0]["width"] = 3840
        with self.assertRaisesRegex(ValueError, "resize/crop"):
            catalog.effective_camera(source, {"compatible_identifier": "synthetic-eis"})

    def test_unsupported_models_and_transforms(self):
        for key, value in [
            ("distortion_model", "opencv_standard"),
            ("distortion_model_id", 2),
            ("digital_lens", "gopro_hyperview"),
            ("is_superview", True),
            ("interpolations", {"0": {}}),
            ("input_horizontal_stretch", 1.33),
            ("crop", -1),
        ]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                source = source_profile()
                source[key] = value
                catalog.effective_camera(source, {})
        source = source_profile()
        source["distortion_model_id"] = 0
        source["is_superview"] = False
        catalog.effective_camera(source, {})
        source["fisheye_params"]["camera_matrix"][0][0] = float("nan")
        with self.assertRaisesRegex(ValueError, "Nonfinite"):
            catalog.effective_camera(source, {})

    def test_fold_and_projection_limits_are_rejected(self):
        source = source_profile()
        source["fisheye_params"]["distortion_coeffs"][0] = -1
        with self.assertRaisesRegex(ValueError, "Nonmonotonic"):
            catalog.convert(catalog.effective_camera(source, {}), "full-frame-fisheye")
        with self.assertRaisesRegex(ValueError, "90 degrees"):
            catalog.fit_radial(lambda t: 1000 * t, 1.7, 1920, 1080, "rectilinear")
        with self.assertRaisesRegex(ValueError, "does not cover"):
            catalog.bisect(lambda t: t, 1.01, 1)

    def test_source_hash_and_catalog_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "sources").mkdir()
            raw = json.dumps(source_profile()).encode()
            (root / "sources/synthetic.json").write_bytes(raw)
            recipe = {
                "id": "synthetic",
                "display_name": "Synthetic",
                "source_file": "synthetic.json",
                "source_path": "Synthetic.json",
                "source_sha256": "0" * 64,
                "projection": "full-frame-fisheye",
                "notes": "Test",
            }
            manifest = {
                "upstream_repository": "https://github.com/gyroflow/lens_profiles",
                "upstream_revision": "a" * 40,
                "profiles": [recipe],
            }
            path = root / "catalog.json"
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "SHA256"):
                catalog.read_catalog(path)
            recipe["source_sha256"] = hashlib.sha256(raw).hexdigest()
            for invalid in ("synthetic-", "synthetic--camera", "Synthetic"):
                recipe["id"] = invalid
                path.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, "Invalid.*id"):
                    catalog.read_catalog(path)
            recipe["id"] = "synthetic"
            manifest["profiles"].append(copy.deepcopy(recipe))
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "duplicate"):
                catalog.read_catalog(path)

    def test_block_update_preserves_surroundings(self):
        text = f"before\n{catalog.BEGIN}\nold\n{catalog.END}\nafter\n"
        replacement = f"{catalog.BEGIN}\nnew\n{catalog.END}"
        self.assertEqual(
            catalog.replace_block(text, replacement), text.replace("old", "new")
        )
        for malformed in (
            "missing",
            text + catalog.BEGIN,
            f"{catalog.END}\n{catalog.BEGIN}",
        ):
            with self.subTest(text=malformed), self.assertRaises(ValueError):
                catalog.replace_block(malformed, replacement)


class BundledCatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.records = catalog.read_catalog(ROOT / "configs/camera-lenses/catalog.json")

    def test_bundled_baseline_reproduces(self):
        baseline = (ROOT / "configs/baseline.yaml").read_text()
        self.assertEqual(
            catalog.replace_block(baseline, catalog.render(self.records)), baseline
        )

    def test_independent_rays_obey_declared_bounds(self):
        for record in self.records:
            camera, result = record["effective_camera"], copy.deepcopy(
                record["conversion"]
            )
            # Validate the exact 12-significant-digit values written to YAML.
            result["horizontal_fov"] = float(catalog.scalar(result["horizontal_fov"]))
            for key in ("distortion", "center_offset"):
                result[key] = [float(catalog.scalar(v)) for v in result[key]]
            # Asymmetric sampling includes boundaries and is different from the conversion grid.
            for nx in (0, 0.13, 0.49, 0.79, 1):
                for ny in (0, 0.23, 0.57, 0.93, 1):
                    x, y = nx * camera["width"], ny * camera["height"]
                    with self.subTest(profile=record["id"], x=x, y=y):
                        px, py = model_pixel(camera, result, x, y)
                        self.assertLessEqual(
                            math.hypot(px - x, py - y),
                            result["conversion_bound_px"] + 1e-6,
                        )
            self.assertLessEqual(
                result["conversion_bound_px_at_8k"], catalog.MAX_ERROR_AT_8K
            )


if __name__ == "__main__":
    unittest.main()
