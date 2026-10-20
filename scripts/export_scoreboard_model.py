#!/usr/bin/env python3
"""Export the scoreboard checkpoint for native ONNX and HockeyMON TorchScript.

Developer-only: run in the training environment. Neither runtime imports
MMDetection. Both graphs accept normalized RGB [1,3,768,2048] and return the
final class/mask logits; mask-to-polygon processing stays bounded to this canvas.
"""

import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from mmdet.apis import init_detector

    torch.set_num_threads(8)
    torch.manual_seed(17)
    model = init_detector(str(args.config), str(args.checkpoint), device="cpu").eval()
    if tuple(model.dataset_meta["classes"]) != ("Scoreboard",):
        raise ValueError("Expected the single-class scoreboard checkpoint")

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
    exemplar = torch.randn(1, 3, 768, 2048)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = args.output_dir / "scoreboard-mask2former-r50.onnx"
    torch_path = args.output_dir / "scoreboard-mask2former-r50.pt"
    with torch.inference_mode():
        reference = [value.numpy() for value in wrapper(exemplar)]
        # CPU tracing selects the portable grid_sample implementation of
        # deformable attention, which also runs on CUDA without MMCV extensions.
        traced = torch.jit.trace(wrapper, exemplar, check_trace=False)
        traced = torch.jit.freeze(traced)
        # Tracing records factory-device arguments as CPU literals. Bind them
        # to the input instead so this one artifact can run on either device.
        graph = traced.graph
        device = graph.create("prim::device", [list(graph.inputs())[1]], 1)
        device.output().setType(torch._C.DeviceObjType.get())
        device.insertBefore(next(graph.nodes()))

        def portable_devices(block):
            for node in list(block.nodes()):
                for nested in node.blocks():
                    portable_devices(nested)
                if (
                    node.kind() == "prim::Constant"
                    and node.output().type().kind() == "DeviceObjType"
                ):
                    node.output().replaceAllUsesWith(device.output())
                    node.destroy()

        portable_devices(graph)
        torch._C._jit_pass_lint(graph)
        traced.save(str(torch_path))
        torch.onnx.export(
            wrapper,
            exemplar,
            str(onnx_path),
            opset_version=17,
            dynamo=False,
            input_names=["images"],
            output_names=["class_logits", "mask_logits"],
        )
    onnx.checker.check_model(str(onnx_path))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 8
    session = ort.InferenceSession(
        str(onnx_path), options, providers=["CPUExecutionProvider"]
    )
    actual = session.run(None, {"images": exemplar.numpy()})
    for output, expected in zip(actual, reference):
        np.testing.assert_allclose(output, expected, atol=3e-3, rtol=2e-3)
    del session
    if torch.cuda.is_available():
        with torch.inference_mode():
            gpu = torch.jit.load(str(torch_path), map_location="cuda")
            for output, expected in zip(gpu(exemplar.cuda()), reference):
                np.testing.assert_allclose(
                    output.cpu().numpy(), expected, atol=1e-2, rtol=3e-3
                )
        del gpu
    manifest = {
        "schema": "scoreboard-mask2former-r50-v1",
        "checkpoint_sha256": hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
        "input": [1, 3, 768, 2048],
        "outputs": [list(value.shape) for value in actual],
        "opset": 17,
        "torch": torch.__version__,
        "maximum_absolute_parity_error": [
            float(np.max(np.abs(a - b))) for a, b in zip(actual, reference)
        ],
        "artifacts": {},
    }
    for path in (onnx_path, torch_path):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        named = path.with_name(f"{path.stem}-{digest[:16]}{path.suffix}")
        path.rename(named)
        manifest["artifacts"][named.name] = digest
    (args.output_dir / "scoreboard-model.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
