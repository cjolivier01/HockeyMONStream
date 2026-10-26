#!/usr/bin/env python3
"""Exercise the actual CPU import boundary with private temporary bundles."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


def manifest():
    return {
        "schema_version": 1, "feature": "pose", "model_family": "rtmpose", "model_id": "reference-pose",
        "source": {"url": "https://example.invalid/model.pth", "license": "Apache-2.0", "sha256": "a" * 64, "config_sha256": "b" * 64},
        "onnx": {"file": "model.onnx", "opset": 17, "sha256": "a" * 64},
        "engine": {"file": "model.engine", "tensorrt_version": "10.3.0", "tensorrt_build_version": 1, "cuda_runtime_version": 12060, "gpu_name": "Example GPU", "compute_capability": "8.7", "precision": "fp16", "max_batch": 8, "sha256": "a" * 64},
        "inputs": [{"name": "image", "dtype": "float32", "shape": [-1, 3, 256, 192]}],
        "outputs": [{"name": "simcc_x", "dtype": "float32", "shape": [-1, 17, 384]}, {"name": "simcc_y", "dtype": "float32", "shape": [-1, 17, 512]}],
        "topology_id": "coco17",
        "preprocessing": {"id": "rtmpose-coco17-affine-v1", "color": "rgb", "mean": [123.675, 116.28, 103.53], "std": [58.395, 57.12, 57.375], "padding": 1.25, "simcc_split_ratio": 2, "flip_test": False},
    }


class ImportBoundary(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.bundle = self.root / "inputs"
        self.bundle.mkdir()
        (self.bundle / "manifest.json").write_text(json.dumps(manifest()))
        (self.bundle / "model.engine").write_bytes(b"prepared engine")
        (self.bundle / "model.onnx").write_bytes(b"prepared onnx")
        self.request = {"schema": 1, "id": "reference-id", "root": str(self.root), "feature": "pose", "bundle": "inputs"}

    def freeze(self):
        self.request["files"] = [{"path": item.name, "size": item.stat().st_size, "sha256": hashlib.sha256(item.read_bytes()).hexdigest()} for item in self.bundle.iterdir()]

    def run_import(self, success):
        (self.root / "model-request.json").write_text(json.dumps(self.request))
        result = subprocess.run([BINARY, "--request", str(self.root / "model-request.json")], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        self.assertEqual((self.root / "model-validation.yaml").exists(), success)
        return result

    def test_valid_pose(self):
        self.freeze()
        self.run_import(True)
        self.assertIn("topology: coco17", (self.root / "model-validation.yaml").read_text())

    def test_changed_checksum(self):
        self.freeze()
        (self.bundle / "model.engine").write_bytes(b"different engine")
        self.run_import(False)

    def test_missing_file(self):
        self.freeze()
        (self.bundle / "model.engine").unlink()
        self.run_import(False)

    def test_extra_file(self):
        self.freeze()
        (self.bundle / "unadmitted").write_bytes(b"extra")
        self.run_import(False)

    def test_wrong_feature(self):
        self.freeze()
        self.request["feature"] = "action"
        self.run_import(False)

    def test_unsafe_path(self):
        self.freeze()
        self.request["files"][0]["path"] = "../model-request.json"
        self.run_import(False)

    def test_symlink_file(self):
        (self.root / "outside.engine").write_bytes(b"prepared engine")
        (self.bundle / "model.engine").unlink()
        (self.bundle / "model.engine").symlink_to(self.root / "outside.engine")
        self.freeze()
        self.run_import(False)

    def reid(self, engine):
        (self.bundle / "manifest.json").unlink()
        (self.bundle / "model.onnx").unlink()
        (self.bundle / "config.yaml").write_text(json.dumps({"ReID": {"modelEngineFile": engine}}))
        self.request.update(feature="reid", entry="config.yaml")
        self.freeze()

    def test_valid_reid(self):
        self.reid("model.engine")
        self.run_import(True)

    def test_missing_reid_engine(self):
        self.reid("missing.engine")
        self.run_import(False)

    def test_escaping_reid_engine(self):
        (self.root / "outside.engine").write_bytes(b"engine")
        self.reid("../outside.engine")
        self.run_import(False)


if __name__ == "__main__":
    unittest.main()
