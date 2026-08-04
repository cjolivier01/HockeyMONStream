#!/usr/bin/env python3
"""Conditional HockeyMOM reference used only by the repository parity test."""

import argparse
import os
import sys
from pathlib import Path


def skip(message: str) -> None:
    print(f"SKIP: {message}", file=sys.stderr)
    raise SystemExit(77)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--max-control-points", type=int, default=128)
    args = parser.parse_args()
    os.environ["HM_GAME_DIR"] = str(args.game_dir.parent)

    try:
        import cv2
        import numpy as np
        import torch
        import yaml
        from lightglue import ALIKED, LightGlue, SuperPoint
        from lightglue.utils import rbd
        from mmdet.apis import init_detector  # noqa: F401
        from hmlib.config import prepend_root_dir
        from hmlib.models.loader import get_model_config
        from hmlib.segm.ice_rink import find_ice_rink_masks
        from hmlib.stitching.control_points import calculate_control_points
    except Exception as exc:
        skip(f"HockeyMOM parity dependencies are unavailable: {exc}")

    stitched = cv2.imread(str(args.game_dir / "s.png"), cv2.IMREAD_COLOR)
    left = cv2.imread(str(args.game_dir / "left.png"), cv2.IMREAD_COLOR)
    right = cv2.imread(str(args.game_dir / "right.png"), cv2.IMREAD_COLOR)
    if stitched is None or left is None or right is None:
        skip("left.png, right.png, and s.png parity fixtures are required")

    try:
        config_file, checkpoint = get_model_config(game_id=args.game_dir.name, model_name="ice_rink_segm")
        config_file = prepend_root_dir(config_file)
        checkpoint = prepend_root_dir(checkpoint)
    except Exception as exc:
        skip(f"HockeyMOM rink model configuration is unavailable: {exc}")
    if not Path(config_file).is_file() or not Path(checkpoint).is_file():
        skip(f"HockeyMOM rink checkpoint is unavailable: {checkpoint}")

    try:
        rink = find_ice_rink_masks(
            image=stitched,
            config_file=config_file,
            checkpoint=checkpoint,
            device=torch.device("cpu"),
            inference_scale=1.0,
        )[0]
    except Exception as exc:
        skip(f"HockeyMOM rink reference could not run: {exc}")
    if rink is None or rink.get("combined_mask") is None:
        raise RuntimeError("HockeyMOM rink reference returned no combined mask")

    def numpy_value(value):
        if isinstance(value, torch.Tensor):
            value = value.detach().cpu().numpy()
        return np.asarray(value)

    mask = numpy_value(rink["combined_mask"]).astype(np.uint8) * 255
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(args.output_dir / "python_rink_mask.png"), mask):
        raise RuntimeError("unable to save Python parity rink mask")

    def prepare_matcher_image(image):
        height, width = image.shape[:2]
        scale = min(1024.0 / width, 576.0 / height)
        resized_width = max(32, int(round(width * scale)))
        resized_height = max(32, int(round(height * scale)))
        resized = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_AREA)
        canvas = np.zeros((576, 1024, 3), dtype=np.uint8)
        canvas[:resized_height, :resized_width] = resized
        return canvas, resized_width, resized_height

    left_input, left_width, left_height = prepare_matcher_image(left)
    right_input, right_width, right_height = prepare_matcher_image(right)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    try:
        points = calculate_control_points(
            left_input,
            right_input,
            max_control_points=args.max_control_points,
            device=device,
            max_num_keypoints=2048,
        )
    except Exception as exc:
        skip(f"HockeyMOM SuperPoint/LightGlue reference could not run: {exc}")
    points0 = numpy_value(points["m_kpts0"])
    points1 = numpy_value(points["m_kpts1"])
    valid = (
        (points0[:, 0] >= 0)
        & (points0[:, 0] < left_width)
        & (points0[:, 1] >= 0)
        & (points0[:, 1] < left_height)
        & (points1[:, 0] >= 0)
        & (points1[:, 0] < right_width)
        & (points1[:, 1] >= 0)
        & (points1[:, 1] < right_height)
    )
    points0 = points0[valid]
    points1 = points1[valid]
    points0[:, 0] = (points0[:, 0] + 0.5) * left.shape[1] / left_width - 0.5
    points0[:, 1] = (points0[:, 1] + 0.5) * left.shape[0] / left_height - 0.5
    points1[:, 0] = (points1[:, 0] + 0.5) * right.shape[1] / right_width - 0.5
    points1[:, 1] = (points1[:, 1] + 0.5) * right.shape[0] / right_height - 0.5

    # Compare the exported graph to the same redistributable ALIKED+LightGlue
    # pair, not only to HockeyMOM's legacy SuperPoint baseline. The frozen ONNX
    # contract takes two zero-padded RGB float images and applies a strict 0.2
    # LightGlue score threshold.
    def image_tensor(image):
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        return torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1))).float().div_(255.0)

    try:
        extractor = ALIKED(max_num_keypoints=2048).eval().to(device)
        aliked_matcher = LightGlue(features="aliked").eval().to(device)
        with torch.inference_mode():
            features0 = extractor.extract(image_tensor(left_input).to(device))
            features1 = extractor.extract(image_tensor(right_input).to(device))
            prediction = aliked_matcher({"image0": features0, "image1": features1})
            features0, features1, prediction = [rbd(item) for item in (features0, features1, prediction)]
        match_indices = prediction["matches"].detach().cpu().numpy()
        aliked_scores = prediction["scores"].detach().cpu().numpy()
        aliked0 = features0["keypoints"][prediction["matches"][:, 0]].detach().cpu().numpy()
        aliked1 = features1["keypoints"][prediction["matches"][:, 1]].detach().cpu().numpy()
    except Exception as exc:
        skip(f"Python ALIKED+LightGlue oracle could not run: {exc}")
    if match_indices.ndim != 2 or match_indices.shape[1] != 2:
        raise RuntimeError("Python ALIKED matcher returned an invalid match contract")
    aliked_valid = (
        (aliked_scores > 0.2)
        & (aliked0[:, 0] >= 0)
        & (aliked0[:, 0] < left_width)
        & (aliked0[:, 1] >= 0)
        & (aliked0[:, 1] < left_height)
        & (aliked1[:, 0] >= 0)
        & (aliked1[:, 0] < right_width)
        & (aliked1[:, 1] >= 0)
        & (aliked1[:, 1] < right_height)
    )
    aliked0 = aliked0[aliked_valid]
    aliked1 = aliked1[aliked_valid]
    aliked_scores = aliked_scores[aliked_valid]
    aliked0[:, 0] = (aliked0[:, 0] + 0.5) * left.shape[1] / left_width - 0.5
    aliked0[:, 1] = (aliked0[:, 1] + 0.5) * left.shape[0] / left_height - 0.5
    aliked1[:, 0] = (aliked1[:, 0] + 0.5) * right.shape[1] / right_width - 0.5
    aliked1[:, 1] = (aliked1[:, 1] + 0.5) * right.shape[0] / right_height - 0.5

    centroid = numpy_value(rink["centroid"]).reshape(-1).tolist()
    bbox = numpy_value(rink["combined_bbox"]).reshape(-1).tolist()
    output = {
        "centroid": [float(centroid[0]), float(centroid[1])],
        "bbox": [float(value) for value in bbox[:4]],
        "left_points": [[float(x), float(y)] for x, y in points0],
        "right_points": [[float(x), float(y)] for x, y in points1],
        "aliked_left_points": [[float(x), float(y)] for x, y in aliked0],
        "aliked_right_points": [[float(x), float(y)] for x, y in aliked1],
        "aliked_scores": [float(score) for score in aliked_scores],
    }
    with (args.output_dir / "python_reference.yaml").open("w", encoding="utf-8") as stream:
        yaml.safe_dump(output, stream)


if __name__ == "__main__":
    main()
