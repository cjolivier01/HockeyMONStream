#!/usr/bin/env python3
"""Developer-only export of the trained rink landmark raw heads for native use.

Run in the HockeyMON training environment. Production never invokes Python.
The graph accepts RGB CHW floats normalized with ImageNet mean/std, with the
long edge resized to 1008 and zero padding to 1024. Outputs remain at 256x256;
native postprocessing never expands every query to full camera resolution.
"""

import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from mmdet.apis import init_detector

    torch.set_num_threads(8)
    torch.manual_seed(17)
    model = init_detector(str(args.config), str(args.checkpoint), device="cpu").eval()
    classes = tuple(model.dataset_meta["classes"])
    expected = (
        "Blue Line",
        "Center Ice Circle",
        "Center Line",
        "Crease",
        "Faceoff Dot",
        "Field",
        "Goal",
        "Goal Line",
        "Slot Box",
        "Trapezoid",
        "Zone Circle",
    )
    if classes != expected:
        raise ValueError(f"Unexpected checkpoint class order: {classes}")

    class RawHeads(torch.nn.Module):
        def __init__(self, detector):
            super().__init__()
            self.detector = detector

        def forward(self, images):
            logits, masks = self.detector.panoptic_head(
                self.detector.extract_feat(images), []
            )
            return logits[-1], masks[-1]

    wrapper = RawHeads(model).eval()
    exemplar = torch.randn(1, 3, 1024, 1024)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with torch.no_grad():
        reference = [value.numpy() for value in wrapper(exemplar)]
        torch.onnx.export(
            wrapper,
            exemplar,
            str(args.output),
            opset_version=17,
            dynamo=False,
            input_names=["images"],
            output_names=["class_logits", "mask_logits"],
        )
    onnx.checker.check_model(str(args.output))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 8
    session = ort.InferenceSession(
        str(args.output), options, providers=["CPUExecutionProvider"]
    )
    actual = session.run(None, {"images": exemplar.numpy()})
    errors = [float(np.max(np.abs(a - b))) for a, b in zip(actual, reference)]
    for a, b in zip(actual, reference):
        np.testing.assert_allclose(a, b, atol=3e-3, rtol=2e-3)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    manifest = {
        "schema": "hstream-rink-landmarks-onnx-v1",
        "sha256": digest,
        "checkpoint_sha256": hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
        "classes": classes,
        "input": [1, 3, 1024, 1024],
        "outputs": [list(value.shape) for value in actual],
        "opset": 17,
        "torch": torch.__version__,
        "onnx": onnx.__version__,
        "maximum_absolute_parity_error": errors,
    }
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
