#!/usr/bin/env python3
"""Rendered interaction/export checks for the shared Qt/browser still editor.

Requires Playwright, system Chrome, NumPy, OpenCV and PyYAML. Set
RINK_WARP_BROWSER_GPU=1 to use native Vulkan instead of software WebGL.
"""

import base64
import importlib.util
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np
import yaml
from constrained_rink_warp import RinkWarp
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "rink_warp_bundle", ROOT / "src/apps/rink-warp-editor/bundle.py"
)
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


def png_data(image):
    ok, encoded = cv2.imencode(".png", image)
    assert ok
    return "data:image/png;base64," + base64.b64encode(encoded).decode()


class EditorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        chrome = shutil.which("google-chrome") or shutil.which("chromium")
        if not chrome:
            raise unittest.SkipTest("System Chrome is required")
        flags = ["--no-sandbox", "--use-gl=angle", "--ignore-gpu-blocklist"]
        flags += (
            ["--use-angle=vulkan"]
            if os.environ.get("RINK_WARP_BROWSER_GPU")
            else ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
        )
        cls.browser = cls.playwright.chromium.launch(
            executable_path=chrome, headless=True, args=flags
        )

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.context = self.browser.new_context(
            viewport={"width": 1450, "height": 950},
            device_scale_factor=1,
            accept_downloads=True,
        )
        self.page = self.context.new_page()
        self.errors = []
        self.page.on("pageerror", lambda error: self.errors.append(str(error)))
        y, x = np.indices((201, 401))
        self.image = np.stack(
            (x % 256, y % 256, (x // 7 + y // 7) % 2 * 180 + 40), axis=-1
        ).astype(np.uint8)
        self.mask = (
            (y / 200 > 0.3) & (y / 200 < 0.8 - 0.3 * (2 * x / 400 - 1) ** 2)
        ).astype(np.uint8) * 255
        # Occlusion outliers in both directions must not become fitted knots.
        self.mask[100:, 45:57] = 0
        self.mask[:195, 310:317] = 255
        self.spec = {
            "schema": 3,
            "source_size": [401, 201],
            "guides": [
                [0, 0.3, 0.5],
                [0.25, 0.4, 0.7],
                [0.5, 0.45, 0.8],
                [0.75, 0.4, 0.7],
                [1, 0.3, 0.5],
            ],
            "target_rows": [0.55, 0.9],
            "horizontal_crop": [0, 1],
            "strength": 1,
            "aspect_balance": 1,
        }
        defaults = yaml.safe_load((ROOT / "configs/baseline.yaml").read_text())[
            "hstream_ui"
        ]["rink_warp"]
        boot = {
            "defaults": defaults,
            "image": png_data(self.image),
            "mask": png_data(self.mask),
            "guides": self.spec,
            "title": "<still & guides>",
        }
        html = bundle.template(ROOT / "src/apps/rink-warp-editor").replace(
            "@@WARP_BOOTSTRAP@@", json.dumps(boot).replace("<", "\\u003c")
        )
        path = self.root / "editor.html"
        path.write_text(html)
        self.page.goto(path.as_uri())
        self.page.wait_for_selector('html[data-warp-ready="true"]')
        # Validate presented pixels too: an export can be correct even when a
        # browser compositor cannot present its accelerated canvas.
        screenshot = cv2.imdecode(
            np.frombuffer(self.page.screenshot(), dtype=np.uint8), cv2.IMREAD_COLOR
        )
        px, py = self.point(207 / 400, 123 / 200)
        displayed = screenshot[round(py), round(px)].astype(float)
        self.assertLess(float(np.max(np.abs(displayed - self.image[123, 207]))), 8)

        if os.environ.get("RINK_WARP_BROWSER_GPU"):
            device = self.page.locator("#warp-device").inner_text()
            print("Hardware renderer:", device, flush=True)
            self.assertFalse(
                any(
                    name in device.lower()
                    for name in ("swiftshader", "lavapipe", "llvmpipe")
                ),
                device,
            )

    def tearDown(self):
        try:
            self.assertEqual(self.errors, [])
        finally:
            self.context.close()
            self.directory.cleanup()

    def snapshot(self):
        return self.page.evaluate("rinkWarpEditor.snapshot()")

    def point(self, x, y):
        b = self.page.locator("[data-pane=source] .warp-overlay").bounding_box()
        w, h = 401, 201
        scale = min(b["width"] / w, b["height"] / h) * 0.96
        return (
            b["x"] + b["width"] / 2 + (x - 0.5) * w * scale,
            b["y"] + b["height"] / 2 + (y - 0.5) * h * scale,
        )

    def drag(self, start, end):
        self.page.mouse.move(*start)
        self.page.mouse.down()
        self.page.mouse.move(*end, steps=12)
        self.page.mouse.up()
        self.page.wait_for_timeout(100)

    def test_drag_undo_and_wheel_scroll(self):
        before = self.page.locator("[data-pane=result]").screenshot()
        self.drag(self.point(0.5, 0.8), self.point(0.5, 0.74))
        changed = self.snapshot()
        self.assertAlmostEqual(changed["guides"][2][2], 0.74, delta=0.002)
        self.assertNotEqual(
            before, self.page.locator("[data-pane=result]").screenshot()
        )
        self.page.get_by_role("button", name="Undo", exact=True).click()
        self.assertEqual(self.snapshot(), self.spec)
        # Selecting a point is not an edit and must preserve the redo branch.
        self.page.mouse.click(*self.point(0.5, 0.8))
        self.page.mouse.click(*self.point(0, 0.5))
        self.page.keyboard.press("ArrowLeft")
        self.assertEqual(self.snapshot(), self.spec)
        self.assertTrue(
            self.page.get_by_role("button", name="Undo", exact=True).is_disabled()
        )
        self.assertTrue(
            self.page.get_by_role("button", name="Redo", exact=True).is_enabled()
        )
        self.page.get_by_role("button", name="Redo", exact=True).click()
        self.assertEqual(self.snapshot(), changed)
        slider = self.page.get_by_role("slider", name="Flattening", exact=True)
        slider.hover()
        self.page.mouse.wheel(0, 500)
        self.page.wait_for_timeout(100)
        self.assertEqual(self.snapshot(), changed)
        self.assertGreater(
            self.page.locator(".warp-controls").evaluate("(e)=>e.scrollTop"), 50
        )

    def test_symmetry_drawing_and_point_edits(self):
        self.page.get_by_label("Mirror guide edits", exact=True).check()
        state = self.snapshot()
        row = state["guides"][3]
        self.drag(
            self.point(row[0], row[2]), self.point(row[0] + 0.015, row[2] - 0.025)
        )
        state = self.snapshot()
        for a, b in zip(state["guides"], reversed(state["guides"])):
            self.assertAlmostEqual(a[0] + b[0], 1, places=8)
            self.assertAlmostEqual(a[2], b[2], places=8)
        self.page.get_by_label("Mirror guide edits", exact=True).uncheck()
        self.page.get_by_label("Active guide", exact=True).select_option("1")
        self.page.get_by_role("button", name="Draw guide", exact=True).click()
        self.page.mouse.move(*self.point(0.02, 0.29))
        self.page.mouse.down()
        for x in np.linspace(0.02, 0.98, 55):
            self.page.mouse.move(
                *self.point(float(x), 0.29 + 0.15 * (1 - (2 * x - 1) ** 2))
            )
        self.page.mouse.up()
        self.page.wait_for_timeout(100)
        self.assertAlmostEqual(self.snapshot()["guides"][6][1], 0.44, delta=0.015)
        old_count = len(self.snapshot()["guides"])
        self.page.mouse.dblclick(*self.point(0.29, 0.4))
        self.assertEqual(len(self.snapshot()["guides"]), old_count + 1)
        self.page.get_by_role("button", name="Remove point", exact=True).click()
        self.assertEqual(len(self.snapshot()["guides"]), old_count)

    def test_broad_mask_fit_and_identity(self):
        self.page.get_by_role(
            "button", name="Fit lower guide to mask", exact=True
        ).click()
        for x, _, lower in self.snapshot()["guides"]:
            self.assertAlmostEqual(lower, 0.8 - 0.3 * (2 * x - 1) ** 2, delta=0.035)
        self.page.get_by_role("slider", name="Flattening", exact=True).focus()
        self.page.keyboard.press("Home")
        self.assertEqual(self.snapshot()["strength"], 0)
        self.assertEqual(self.page.evaluate("rinkWarpEditor.outputSize()"), [401, 201])

    def test_latest_mask_selection_wins_after_delayed_decode(self):
        self.page.evaluate("""() => {
          const NativeImage = window.Image;
          let count = 0;
          window.Image = function() {
            const image = new NativeImage(), held = ++count === 1;
            Object.defineProperty(image, 'onload', {set(callback) {
              image.addEventListener('load', () => {
                if (held) window.releaseDecodedImage = () => callback.call(image);
                else callback.call(image);
              });
            }});
            return image;
          };
        }""")
        for name, lower in (("first", 0.6), ("second", 0.9)):
            mask = np.zeros((201, 401), dtype=np.uint8)
            mask[40 : int(lower * 200)] = 255
            path = self.root / f"{name}.png"
            cv2.imwrite(str(path), mask)
            with self.page.expect_file_chooser() as chooser:
                self.page.get_by_role(
                    "button", name="Open ice mask", exact=True
                ).click()
            chooser.value.set_files(path)
            if name == "first":
                self.page.wait_for_function("typeof releaseDecodedImage === 'function'")
        fit = self.page.get_by_role(
            "button", name="Fit lower guide to mask", exact=True
        )
        fit.click()
        self.page.wait_for_function("rinkWarpEditor.snapshot().guides[6][2] > .89")
        self.page.evaluate("releaseDecodedImage()")
        fit.click()
        self.assertAlmostEqual(self.snapshot()["guides"][6][2], 0.895, delta=0.001)

    def test_selected_mask_supersedes_delayed_bootstrap_mask(self):
        self.page.add_init_script("""(() => {
          const NativeImage = window.Image;
          let count = 0;
          window.Image = function() {
            const image = new NativeImage(), held = ++count === 2;
            Object.defineProperty(image, 'onload', {set(callback) {
              image.addEventListener('load', () => {
                if (held) window.releaseBootstrapMask = () => callback.call(image);
                else { callback.call(image); window.lastImageDecoded = count; }
              });
            }});
            return image;
          };
        })();""")
        self.page.reload()
        self.page.wait_for_function("typeof releaseBootstrapMask === 'function'")
        mask = np.zeros((201, 401), dtype=np.uint8)
        mask[40:180] = 255
        path = self.root / "selected.png"
        cv2.imwrite(str(path), mask)
        with self.page.expect_file_chooser() as chooser:
            self.page.get_by_role("button", name="Open ice mask", exact=True).click()
        chooser.value.set_files(path)
        self.page.wait_for_function("lastImageDecoded === 3")
        self.page.evaluate("releaseBootstrapMask()")
        self.page.get_by_role(
            "button", name="Fit lower guide to mask", exact=True
        ).click()
        self.assertAlmostEqual(self.snapshot()["guides"][6][2], 0.895, delta=0.001)

    def test_save_reload_and_gpu_png_matches_cpu(self):
        with self.page.expect_download() as download:
            self.page.get_by_role("button", name="Save guides", exact=True).click()
        guides_path = self.root / "guides.json"
        download.value.save_as(guides_path)
        self.assertEqual(json.loads(guides_path.read_text()), self.spec)
        with self.page.expect_download(timeout=30000) as download:
            self.page.get_by_role("button", name="Export PNG", exact=True).click()
        png_path = self.root / "warped.png"
        download.value.save_as(png_path)
        actual = cv2.imread(str(png_path))
        expected = RinkWarp(self.spec).render(self.image)
        self.assertEqual(actual.shape, expected.shape)
        difference = np.abs(actual.astype(float) - expected.astype(float))
        self.assertLess(float(difference.mean()), 0.6)
        self.assertLess(float(np.quantile(difference, 0.99)), 3.1)
        self.page.get_by_role("slider", name="Flattening", exact=True).focus()
        self.page.keyboard.press("Home")
        with self.page.expect_file_chooser() as chooser:
            self.page.get_by_role("button", name="Load guides", exact=True).click()
        chooser.value.set_files(guides_path)
        self.page.wait_for_function("rinkWarpEditor.snapshot().strength === 1")
        self.assertEqual(self.snapshot(), self.spec)
        invalid = self.root / "invalid.json"
        invalid.write_text('{"schema":3}')
        with self.page.expect_file_chooser() as chooser:
            self.page.get_by_role("button", name="Load guides", exact=True).click()
        chooser.value.set_files(invalid)
        self.page.wait_for_timeout(150)
        self.assertEqual(self.snapshot(), self.spec)
        self.assertIn("schema", self.page.locator("#warp-status").inner_text())


if __name__ == "__main__":
    unittest.main()
