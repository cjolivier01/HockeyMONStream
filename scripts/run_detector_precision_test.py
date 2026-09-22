#!/usr/bin/env python3
"""Exercise wrapper precision arguments without starting a GPU pipeline."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DetectorWrapperTest(unittest.TestCase):
  def test_precision_replaces_saved_engine_and_preserves_explicit_override(self):
    source = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory() as temporary:
      root = Path(temporary)
      shutil.copy(source / "run.sh", root / "run.sh")
      (root / "configs").mkdir()
      for precision in ("fp16", "bf16", "int8"):
        name = f"config_infer_yolov8_hockey_{precision}.yaml"
        contents = (source / "configs" / name).read_text()
        (root / "configs" / name).write_text(contents.replace("$HOME", str(root)))
        models = root / ".cache/hstream/models"
        models.mkdir(parents=True, exist_ok=True)
        engine = models / f"hm_crowdhuman_e85_yolov8_m_1984_736_dynamic_b1-b2_1984x736.onnx_b2_gpu0_NVIDIA_Test_GPU_{precision}.engine"
        engine.write_text("prepared test engine")
      runner = root / "bazel-bin/src/apps/hstream-cli/hstream-cli"
      runner.parent.mkdir(parents=True)
      runner.write_text('#!/bin/sh\nif [ "$1" = "--resolve-engine-path" ]; then printf "%s\\n" "$2" | sed "s/{gpu}/NVIDIA_Test_GPU/g"; exit 0; fi\nprintf "%s\\n" "$@" > "$TEST_ARGUMENTS"\n')
      runner.chmod(0o755)
      capture = root / "arguments.txt"
      environment = dict(os.environ, TEST_ARGUMENTS=str(capture))
      for precision in ("fp16", "bf16", "int8"):
        for override in (None, str(root / "custom.engine")):
          with self.subTest(precision=precision, override=override):
            command = ["bash", str(root / "run.sh"), f"--models-{precision}", "--enable-sinks=FAKE"]
            if override:
              command.append(f"--options=pipeline.primary-gie.model-engine-file={override}")
            subprocess.run(command, cwd=root, env=environment, check=True, capture_output=True, text=True)
            # Apply CLI layers over a conflicting saved UI engine selection.
            effective = {"pipeline.primary-gie.model-engine-file": "saved-different-precision.engine"}
            for argument in capture.read_text().splitlines():
              if argument.startswith("--options="):
                key, value = argument[len("--options="):].split("=", 1)
                effective[key] = value
            self.assertEqual(effective["pipeline.primary-gie.config-file"],
                             f"config_infer_yolov8_hockey_{precision}.yaml")
            selected = effective["pipeline.primary-gie.model-engine-file"]
            if override:
              self.assertEqual(selected, override)
            else:
              self.assertTrue(selected.endswith(f"_b2_gpu0_NVIDIA_Test_GPU_{precision}.engine"), selected)


if __name__ == "__main__":
  unittest.main()
