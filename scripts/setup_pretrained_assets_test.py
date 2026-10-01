import argparse
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    import setup_pretrained_assets as setup
except ModuleNotFoundError:
    from scripts import setup_pretrained_assets as setup


CONFIG = """
pretrained-assets:
  - name: packaged
    on-demand: true
    redistributable: true
    url: https://example.invalid/packaged.onnx
    path: $HOME/.cache/hstream/models/packaged.onnx
  - name: private
    redistributable: false
    url: https://example.invalid/private.onnx
    path: $HOME/.cache/hstream/models/private.onnx
  - name: unmarked
    url: https://example.invalid/unmarked.onnx
    path: $HOME/.cache/hstream/models/unmarked.onnx
  - name: in-tree
    redistributable: true
    url: https://example.invalid/in-tree.onnx
    path: ../pretrained/in-tree.onnx
"""

APP_YAML = Path("app.yaml")


def selected(model_cache_dir=""):
    """Return {name: target} for the assets a packaging prefetch would act on.

    HOME is pinned so the declared `$HOME/.cache/...` targets resolve the same
    way under a bazel sandbox, which does not export one.
    """
    args = argparse.Namespace(model_cache_dir=model_cache_dir)
    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp) / "home"
        home.mkdir()
        config_path = Path(tmp) / "configs" / "app.yaml"
        config_path.parent.mkdir(parents=True)
        config_path.write_text(CONFIG, encoding="utf-8")
        with mock.patch.dict(os.environ, {"HOME": str(home)}):
            config = setup._load_yaml(config_path)
            return home, {
                spec["name"]: setup._effective_target(spec, config, config_path, args)
                for spec in setup._iter_asset_specs(config)
                if setup._asset_flag(spec, "redistributable", config_path)
            }


class RedistributableSelectionTest(unittest.TestCase):
    def test_only_explicitly_redistributable_assets_are_selected(self):
        _, targets = selected()
        self.assertEqual(sorted(targets), ["in-tree", "packaged"])

    def test_an_absent_flag_is_not_redistributable(self):
        # Fail-closed: AssetManager defaults redistributable to false, so an
        # asset with no recorded grant must never reach a binary package.
        self.assertFalse(setup._asset_flag({"name": "unmarked"}, "redistributable", APP_YAML))

    def test_on_demand_does_not_exclude_a_package_asset(self):
        # The flags are orthogonal. An on-demand asset is still package-owned
        # when it is redistributable, which is exactly the case the host
        # prefetch exists to cover.
        _, targets = selected()
        self.assertIn("packaged", targets)

    def test_yaml_cpp_boolean_spellings_are_accepted(self):
        for token in ("yes", "on", 1, True, "true"):
            self.assertTrue(setup._asset_flag({"redistributable": token}, "redistributable", APP_YAML), token)
        for token in ("no", "off", 0, False, "false"):
            self.assertFalse(setup._asset_flag({"redistributable": token}, "redistributable", APP_YAML), token)

    def test_a_non_boolean_flag_is_rejected(self):
        with self.assertRaises(ValueError):
            setup._asset_flag({"redistributable": "maybe"}, "redistributable", APP_YAML)


class ModelCacheOverrideTest(unittest.TestCase):
    def test_model_cache_assets_follow_the_override(self):
        _, targets = selected(model_cache_dir="/mnt/cache")
        self.assertEqual(targets["packaged"], Path("/mnt/cache/packaged.onnx"))

    def test_assets_outside_the_model_cache_are_left_alone(self):
        # Only the per-user model cache moves. A repo-relative declaration is
        # mounted at its own host path and must keep it.
        _, targets = selected(model_cache_dir="/mnt/cache")
        self.assertEqual(targets["in-tree"].name, "in-tree.onnx")
        self.assertNotEqual(targets["in-tree"].parent, Path("/mnt/cache"))

    def test_no_override_keeps_the_declared_cache_path(self):
        home, targets = selected()
        self.assertEqual(targets["packaged"], home / ".cache/hstream/models/packaged.onnx")


if __name__ == "__main__":
    unittest.main()
