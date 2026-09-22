import argparse
import json
import os
import signal
import subprocess
import time
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

try:
  import prepare_recording_int8 as preparation
except ModuleNotFoundError:
  from scripts import prepare_recording_int8 as preparation


class PreparationTest(unittest.TestCase):
  def fixture(self, root):
    source = root / "detector.onnx"
    source.write_bytes(b"source model")
    config = root / "detector.yaml"
    config.write_text(f"property:\n  onnx-file: {source}\n  net-scale-factor: 0.00392156979\n"
                      "  model-color-format: 0\n  maintain-aspect-ratio: 1\n  symmetric-padding: 1\n")
    return argparse.Namespace(cli=root / "cli", builder=root / "builder", assets=root / "assets",
        app_config=root / "app.yaml", detector_config=config, game_id="game", samples=16, cache=root / "cache")

  def samples(self, directory, count=16):
    directory.mkdir()
    records = []
    for index in range(count):
      name = f"frame-{index:04}.png"
      (directory / name).write_bytes(bytes([index]))
      records.append({"image": name, "timeline_ns": index + 1})
    (directory / "samples.json").write_text(json.dumps({"schema": 1, "width": 32, "height": 16, "gpu_id": 0, "samples": records}))
    (directory / "images.txt").write_text("\n".join(x["image"] for x in records))

  def test_sampler_has_no_program_outputs_or_detector_and_spans_full_recording(self):
    with tempfile.TemporaryDirectory() as temporary:
      args = self.fixture(Path(temporary))
      command = preparation.sampling_command(args, Path(temporary) / "samples", 32, 16)
      self.assertIn("--int8-sample-count=16", command)
      self.assertIn("--enable-sinks=FAKE", command)
      self.assertFalse(any(arg.startswith(("--start-time", "--time-limit", "--models", "-t")) for arg in command))

  def test_manifest_rejects_partial_duplicate_and_escaping_samples(self):
    with tempfile.TemporaryDirectory() as temporary:
      directory = Path(temporary) / "samples"
      self.samples(directory)
      manifest, hashes = preparation.validate_samples(directory, 16, 32, 16)
      self.assertEqual(len(hashes), 16)
      with self.assertRaises(ValueError): preparation.validate_samples(directory, 17, 32, 16)
      manifest["samples"][1]["timeline_ns"] = 1
      (directory / "samples.json").write_text(json.dumps(manifest))
      with self.assertRaises(ValueError): preparation.validate_samples(directory, 16, 32, 16)
      manifest["samples"][1]["timeline_ns"] = 2
      manifest["samples"][0]["image"] = "../escape.png"
      (directory / "samples.json").write_text(json.dumps(manifest))
      with self.assertRaises(ValueError): preparation.validate_samples(directory, 16, 32, 16)

  def test_cancellation_kills_grandchildren_when_child_exits_on_term(self):
    with tempfile.TemporaryDirectory() as temporary:
      root = Path(temporary)
      pid_file = root / "grandchild.pid"
      descendant = root / "descendant.py"
      descendant.write_text("import os, signal, time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                            f"open({str(pid_file)!r}, 'w').write(str(os.getpid()))\ntime.sleep(60)\n")
      child = root / "child.py"
      child.write_text("import subprocess, sys, time, signal\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                       f"subprocess.Popen([sys.executable, {str(descendant)!r}])\ntime.sleep(60)\n")
      helper = root / "helper.py"
      helper.write_text("import sys, signal\n"
                        f"sys.path.insert(0, {str(Path(preparation.__file__).parent)!r})\n"
                        "from prepare_recording_int8 import run, cancel\n"
                        "signal.signal(signal.SIGTERM, cancel)\n"
                        f"run([sys.executable, {str(child)!r}])\n")
      process = subprocess.Popen([sys.executable, str(helper)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
      grandchild = None
      try:
        deadline = time.monotonic() + 5
        while not pid_file.exists() and time.monotonic() < deadline:
          time.sleep(0.01)
        self.assertTrue(pid_file.exists())
        grandchild = int(pid_file.read_text())
        process.terminate()
        time.sleep(0.2)
        process.terminate()
        process.wait(timeout=5)
        status = Path(f"/proc/{grandchild}/stat")
        deadline = time.monotonic() + 2
        while status.exists() and status.read_text().split()[2] != "Z" and time.monotonic() < deadline:
          time.sleep(0.01)
        self.assertTrue(not status.exists() or status.read_text().split()[2] == "Z")
      finally:
        if process.poll() is None:
          process.kill()
          process.wait()
        if grandchild:
          try: os.kill(grandchild, signal.SIGKILL)
          except ProcessLookupError: pass

  def test_publication_requires_successful_build_and_preserves_previous_engine(self):
    dims = [SimpleNamespace(dim_value=value) for value in (0, 3, 16, 32)]
    model = SimpleNamespace(graph=SimpleNamespace(initializer=[], input=[SimpleNamespace(
      type=SimpleNamespace(tensor_type=SimpleNamespace(shape=SimpleNamespace(dim=dims))))]))
    fake_onnx = SimpleNamespace(load=lambda *args, **kwargs: model, TensorProto=SimpleNamespace(EXTERNAL=1))
    for fail in (False, "build", "runtime"):
      with self.subTest(fail=fail), tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        args = self.fixture(root)
        args.cache.mkdir()
        previous = args.cache / "previous.engine"
        previous.write_bytes(b"previous")
        def command(argv, capture=False):
          if fail == "runtime" and "--tensorrt-runtime-info" in argv:
            return "HSTREAM_TENSORRT_RUNTIME version=113000 gpu_uuid=test gpu_name=NVIDIA_Test_GPU"
          if "--runtime-info" in argv or "--tensorrt-runtime-info" in argv:
            return "HSTREAM_TENSORRT_RUNTIME version=101601 gpu_uuid=test gpu_name=NVIDIA_Test_GPU"
          sample = next((x.split("=", 1)[1] for x in argv if x.startswith("--int8-sample-output=")), None)
          if sample: self.samples(Path(sample))
          engine = next((x.split("=", 1)[1] for x in argv if x.startswith("--engine=")), None)
          if engine:
            if fail: raise RuntimeError("build failed")
            Path(engine).write_bytes(b"int8 engine")
            Path(engine + ".layers.json").write_text("{}")
        with patch.dict(sys.modules, {"onnx": fake_onnx, "onnxruntime": SimpleNamespace(),
                                     "cv2": SimpleNamespace(), "numpy": SimpleNamespace()}), \
             patch.object(preparation, "run", side_effect=command), \
             patch.object(preparation, "quantize", side_effect=lambda args: args.output.write_bytes(b"qdq")):
          if fail:
            with self.assertRaises((RuntimeError, ValueError)): preparation.prepare(args)
            self.assertEqual(list(args.cache.iterdir()), [previous])
          else:
            result = preparation.prepare(args)
            self.assertTrue(Path(result["engine"]).is_file())
            document = json.loads(Path(result["manifest"]).read_text())
            self.assertEqual(document["engine_sha256"], preparation.digest(Path(result["engine"])))
          self.assertEqual(previous.read_bytes(), b"previous")
          self.assertFalse(list(args.cache.glob(".preparing-*")))


if __name__ == "__main__":
  unittest.main()
