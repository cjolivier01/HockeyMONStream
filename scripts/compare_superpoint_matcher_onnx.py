#!/usr/bin/env python3
"""Check exported LightGlue against PyTorch with identical SuperPoint features.

Offline qualification only; requires torch, kornia, OpenCV, onnx and
onnxruntime, plus a cvg/LightGlue checkout and its cached public checkpoints.
The detector/preprocessing and attention precision are held constant so this
checks positional normalization and matcher arithmetic, not similar match counts.
Use the FP32 or CUDA production graph; its extractor is bypassed for this check.
"""

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
import torch
from onnx import TensorProto as T
from onnx import helper as h


def matcher_graph(path: Path, capacity: int) -> bytes:
    model = onnx.load(path)
    has_image_sizes = any(value.name == "image_sizes" for value in model.graph.input)
    boundaries = {
        "keypoints": "keypoints",
        "/extractor/Transpose_1_output_0": "descriptors",
    }
    producer = {
        output: index
        for index, node in enumerate(model.graph.node)
        for output in node.output
    }
    needed = set()

    def visit(value):
        if value in boundaries or value not in producer or producer[value] in needed:
            return
        index = producer[value]
        needed.add(index)
        for input_name in model.graph.node[index].input:
            visit(input_name)

    for value in ("matches", "mscores"):
        visit(value)
    nodes = []
    for index in sorted(needed):
        node = model.graph.node[index]
        for index, value in enumerate(node.input):
            node.input[index] = boundaries.get(value, value)
        nodes.append(node)
    del model.graph.node[:]
    model.graph.node.extend(nodes)
    used = {value for node in nodes for value in node.input}
    initializers = [value for value in model.graph.initializer if value.name in used]
    del model.graph.initializer[:]
    model.graph.initializer.extend(initializers)
    del model.graph.input[:]
    model.graph.input.extend(
        [
            h.make_tensor_value_info("images", T.FLOAT, [2, 1, "height", "width"]),
            h.make_tensor_value_info("keypoints", T.INT64, [2, capacity, 2]),
            h.make_tensor_value_info("descriptors", T.FLOAT, [2, capacity, 256]),
        ]
    )
    if has_image_sizes:
        model.graph.input.append(
            h.make_tensor_value_info("image_sizes", T.FLOAT, [2, 4])
        )
    outputs = [
        value for value in model.graph.output if value.name in ("matches", "mscores")
    ]
    del model.graph.output[:]
    model.graph.output.extend(outputs)
    del model.graph.value_info[:]
    onnx.checker.check_model(model, full_check=True)
    return model.SerializeToString()


def compare(args):
    sys.path.insert(0, str(args.lightglue_root))
    from lightglue import LightGlue, SuperPoint
    from lightglue.utils import ImagePreprocessor

    torch.set_grad_enabled(False)
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    extractor = SuperPoint(max_num_keypoints=args.keypoints).eval().to(args.device)
    # The reference defaults to half-precision flash attention even with mp=False.
    # Disable it to compare the production graph's float32 matcher arithmetic.
    matcher = (
        LightGlue(
            features="superpoint",
            depth_confidence=-1,
            width_confidence=-1,
            filter_threshold=0.2,
            flash=False,
        )
        .eval()
        .to(args.device)
    )
    features = []
    detector_points = []
    image_sizes = []
    sizes = []
    for path in (args.left, args.right):
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError(f"Unable to read {path}")
        rgb = torch.from_numpy(image[..., ::-1].copy()).permute(2, 0, 1)
        rgb = rgb[None].to(args.device, torch.float32) / 255
        resized, scale = ImagePreprocessor(**extractor.preprocess_conf)(rgb)
        result = extractor({"image": resized})
        detector_points.append(result["keypoints"])
        # The ONNX graph receives raw detector points plus the exact sizes;
        # the reference extractor restores these source coordinates itself.
        result["keypoints"] = (result["keypoints"] + 0.5) / scale[None] - 0.5
        result["image_size"] = rgb.new_tensor(rgb.shape[-2:][::-1])[None]
        if result["keypoints"].shape[1] != args.keypoints:
            raise ValueError(
                "Reference image has fewer detections than the fixed graph capacity"
            )
        features.append(result)
        sizes.append(tuple(resized.shape[-2:]))
        image_sizes.append([*rgb.shape[-2:][::-1], *resized.shape[-2:][::-1]])
    expected = matcher({"image0": features[0], "image1": features[1]})
    expected_matches = expected["matches"][0].cpu().numpy()
    expected_scores = expected["scores"][0].cpu().numpy()
    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    session = ort.InferenceSession(
        matcher_graph(args.model, args.keypoints),
        options,
        providers=["CPUExecutionProvider"],
    )
    inputs = {
        "images": np.zeros(
            (2, 1, max(s[0] for s in sizes), max(s[1] for s in sizes)), np.float32
        ),
        "keypoints": torch.cat(detector_points).cpu().numpy().astype(np.int64),
        "descriptors": torch.cat([f["descriptors"] for f in features]).cpu().numpy(),
    }
    if any(value.name == "image_sizes" for value in session.get_inputs()):
        inputs["image_sizes"] = np.asarray(image_sizes, dtype=np.float32)
    matches, scores = session.run(["matches", "mscores"], inputs)
    accepted = scores > 0.2
    actual = {
        tuple(pair[1:]): float(score)
        for pair, score in zip(matches[accepted], scores[accepted])
    }
    reference = {
        tuple(pair): float(score)
        for pair, score in zip(expected_matches, expected_scores)
    }
    common = actual.keys() & reference.keys()
    error = max(
        (abs(actual[pair] - reference[pair]) for pair in common), default=float("inf")
    )
    record = {
        "model": str(args.model),
        "reference_matches": len(reference),
        "onnx_matches": len(actual),
        "common_matches": len(common),
        "identical_match_pairs": actual.keys() == reference.keys(),
        "maximum_score_error": error,
        "score_tolerance": args.score_tolerance,
    }
    print(json.dumps(record, indent=2))
    return (
        bool(common)
        and record["identical_match_pairs"]
        and error <= args.score_tolerance
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--lightglue-root", type=Path, required=True)
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--keypoints", type=int, default=2048)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--score-tolerance", type=float, default=0.001)
    sys.exit(0 if compare(parser.parse_args()) else 1)
