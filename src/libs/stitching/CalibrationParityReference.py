#!/usr/bin/env python3
"""Conditional deterministic Python oracle for native calibration parity.

This source-checkout-only helper deliberately uses the pinned LightGlue-ONNX
v3 RaCoALIKED pipeline that produced HStream's ONNX artifact. HockeyMOM rink
and legacy SuperPoint comparisons are reported when their Python dependencies
are installed; production RaCo-ALIKED qualification does not depend on the
legacy matcher.
"""

import argparse
import gc
import importlib.util
import inspect
import os
import pkgutil
import resource
import sys
from pathlib import Path


# MMDetection 3.x still calls pkgutil.find_loader(), removed in Python 3.14.
# Keep this source-only oracle compatible without altering the production
# environment or the installed third-party packages.
if not hasattr(pkgutil, "find_loader"):
    def _find_loader(name: str):
        spec = importlib.util.find_spec(name)
        return None if spec is None else spec.loader

    pkgutil.find_loader = _find_loader  # type: ignore[attr-defined]


def skip(message: str) -> None:
    print(f"SKIP: {message}", file=sys.stderr)
    raise SystemExit(77)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--max-control-points", type=int, default=128)
    parser.add_argument("--lightglue-source-init", type=Path, required=True)
    parser.add_argument("--raco-weights", type=Path, required=True)
    parser.add_argument("--aliked-weights", type=Path, required=True)
    parser.add_argument("--lightglue-weights", type=Path, required=True)
    parser.add_argument("--rink-config", type=Path)
    parser.add_argument("--rink-checkpoint", type=Path)
    parser.add_argument("--rink-inference-scale", type=float, required=True)
    parser.add_argument("--memory-limit-gib", type=float, required=True)
    args = parser.parse_args()
    memory_limit = int(args.memory_limit_gib * 1024**3)
    resource.setrlimit(resource.RLIMIT_AS, (memory_limit, memory_limit))
    os.environ["HM_GAME_DIR"] = str(args.game_dir.parent)
    os.environ.setdefault("MPLCONFIGDIR", str(args.output_dir / "matplotlib"))

    try:
        import cv2
        import hmlib  # parity runs only in a HockeyMOM environment
        import numpy as np
        import torch
        import yaml
    except Exception as exc:
        skip(f"HockeyMOM parity dependencies are unavailable: {exc}")

    # A source checkout intentionally does not install MMDetection or the
    # legacy LightGlue tree into the environment. Discover those pinned sibling
    # trees from the imported hmlib checkout so the opt-in parity test behaves
    # like HockeyMOM's own launchers without changing production PYTHONPATH.
    hockeymom_root = Path(hmlib.__file__).resolve().parent.parent
    for source_tree in (
        hockeymom_root / "openmm" / "mmdetection",
        hockeymom_root / "xmodels" / "LightGlue",
    ):
        if source_tree.is_dir() and str(source_tree) not in sys.path:
            sys.path.insert(0, str(source_tree))

    source_init = args.lightglue_source_init.resolve()
    if source_init.name != "__init__.py" or source_init.parent.name != "lightglue_dynamo":
        skip(f"pinned LightGlue-ONNX v3 source is unavailable: {source_init}")
    for weights in (args.raco_weights, args.aliked_weights, args.lightglue_weights):
        if not weights.is_file():
            skip(f"pinned RaCo-ALIKED weight is unavailable: {weights}")
    sys.path.insert(0, str(source_init.parent.parent))
    try:
        from lightglue_dynamo.config import Extractor
        from lightglue_dynamo.models import LightGlue, Pipeline, RaCoALIKED
    except Exception as exc:
        skip(f"pinned LightGlue-ONNX v3 Python source cannot be imported: {exc}")

    left = cv2.imread(str(args.game_dir / "left.png"), cv2.IMREAD_COLOR)
    right = cv2.imread(str(args.game_dir / "right.png"), cv2.IMREAD_COLOR)
    if left is None or right is None:
        skip("left.png and right.png parity fixtures are required")

    # Freeze the canonical eager oracle to deterministic CPU execution.
    torch.set_num_threads(1)
    try:
        torch.set_num_interop_threads(1)
    except RuntimeError:
        pass
    torch.manual_seed(0)
    torch.use_deterministic_algorithms(True)
    device = torch.device("cpu")

    def numpy_value(value):
        if isinstance(value, torch.Tensor):
            value = value.detach().cpu().numpy()
        return np.asarray(value)

    def prepare_matcher_image(image):
        height, width = image.shape[:2]
        scale = min(1024.0 / width, 576.0 / height)
        resized_width = max(32, int(round(width * scale)))
        resized_height = max(32, int(round(height * scale)))
        resized = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_AREA)
        canvas = np.zeros((576, 1024, 3), dtype=np.uint8)
        canvas[:resized_height, :resized_width] = resized
        return canvas, resized_width, resized_height

    def image_tensor(image):
        rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        return torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1))).float().div_(255.0)

    left_input, left_width, left_height = prepare_matcher_image(left)
    right_input, right_width, right_height = prepare_matcher_image(right)
    try:
        extractor = RaCoALIKED(
            num_keypoints=2048,
            raco_weights=args.raco_weights.resolve(),
            aliked_weights=args.aliked_weights.resolve(),
            portable_deform_conv=True,
        )
        lightglue_config = dict(Extractor.raco_aliked.lightglue_config)
        lightglue_config["url"] = args.lightglue_weights.resolve()
        matcher = LightGlue(**lightglue_config)
        pipeline = Pipeline(extractor, matcher).eval().to(device)
        pipeline.fuse_batch_norm()
        images = torch.stack((image_tensor(left_input), image_tensor(right_input))).to(device)
        with torch.inference_mode():
            keypoints, matches, match_scores = pipeline(images)
    except Exception as exc:
        skip(f"pinned Python RaCo-ALIKED/LightGlue oracle could not run: {exc}")

    keypoints = numpy_value(keypoints)
    matches = numpy_value(matches)
    match_scores = numpy_value(match_scores)
    if keypoints.shape != (2, 2048, 2) or matches.ndim != 2 or matches.shape[1] != 3:
        raise RuntimeError(
            f"Python RaCo-ALIKED export contract changed: keypoints={keypoints.shape}, matches={matches.shape}"
        )
    if match_scores.shape != (matches.shape[0],):
        raise RuntimeError("Python RaCo-ALIKED match scores violate the export contract")

    accepted = []
    for match, score in zip(matches, match_scores, strict=True):
        pair, left_index, right_index = (int(value) for value in match)
        if pair != 0 or not (0 <= left_index < 2048) or not (0 <= right_index < 2048):
            raise RuntimeError("Python RaCo-ALIKED returned an invalid keypoint index")
        if not float(score) > 0.2:
            continue
        left_point = keypoints[0, left_index].copy()
        right_point = keypoints[1, right_index].copy()
        if not (
            0 <= left_point[0] < left_width
            and 0 <= left_point[1] < left_height
            and 0 <= right_point[0] < right_width
            and 0 <= right_point[1] < right_height
        ):
            continue
        left_point[0] = (left_point[0] + 0.5) * left.shape[1] / left_width - 0.5
        left_point[1] = (left_point[1] + 0.5) * left.shape[0] / left_height - 0.5
        right_point[0] = (right_point[0] + 0.5) * right.shape[1] / right_width - 0.5
        right_point[1] = (right_point[1] + 0.5) * right.shape[0] / right_height - 0.5
        accepted.append((left_index, right_index, left_point, right_point, float(score)))
    if not accepted:
        raise RuntimeError("Python RaCo-ALIKED produced no accepted source-image matches")

    by_y = sorted(range(len(accepted)), key=lambda index: accepted[index][2][1])
    selection = torch.linspace(0, len(accepted) - 1, args.max_control_points, dtype=torch.float32).long().tolist()
    selected = [accepted[by_y[index]] for index in selection]

    output = {
        "raco_left_indices": [entry[0] for entry in accepted],
        "raco_right_indices": [entry[1] for entry in accepted],
        "raco_left_points": [[float(value) for value in entry[2]] for entry in accepted],
        "raco_right_points": [[float(value) for value in entry[3]] for entry in accepted],
        "raco_scores": [entry[4] for entry in accepted],
        "raco_selected_left_points": [[float(value) for value in entry[2]] for entry in selected],
        "raco_selected_right_points": [[float(value) for value in entry[3]] for entry in selected],
        "rink_available": False,
        "legacy_superpoint_available": False,
    }
    # The rink oracle has a large, short-lived Mask2Former output. Release the
    # independent RaCo graph before allocating those masks so qualification
    # does not retain both model stacks at once.
    del extractor, matcher, pipeline, images, keypoints, matches, match_scores
    gc.collect()

    # HockeyMOM rink parity is independent of feature matching. Keep its
    # heavier MMDetection import optional for ordinary developer runs; the
    # checked-in mandatory qualification entrypoint requires this section.
    stitched_path = args.game_dir / "s.png"
    stitched = cv2.imread(str(stitched_path), cv2.IMREAD_COLOR) if stitched_path.is_file() else None
    try:
        if stitched is None:
            raise RuntimeError("s.png fixture is unavailable")
        from mmdet.apis import init_detector  # noqa: F401
        import mmdet.apis.inference as mmdet_inference
        from hmlib.config import prepend_root_dir
        from hmlib.models.loader import get_model_config
        from hmlib.segm.ice_rink import find_ice_rink_masks

        if "weights_only" not in inspect.signature(mmdet_inference.load_checkpoint).parameters:
            legacy_load_checkpoint = mmdet_inference.load_checkpoint

            def compatible_load_checkpoint(*load_args, weights_only=False, **load_kwargs):
                del weights_only
                return legacy_load_checkpoint(*load_args, **load_kwargs)

            mmdet_inference.load_checkpoint = compatible_load_checkpoint

        if args.rink_config is not None or args.rink_checkpoint is not None:
            if args.rink_config is None or args.rink_checkpoint is None:
                raise RuntimeError("rink config and checkpoint overrides must be provided together")
            config_file = str(args.rink_config.resolve())
            checkpoint = str(args.rink_checkpoint.resolve())
        else:
            config_file, checkpoint = get_model_config(game_id=args.game_dir.name, model_name="ice_rink_segm")
            config_file = prepend_root_dir(config_file)
            checkpoint = prepend_root_dir(checkpoint)
        if not Path(config_file).is_file() or not Path(checkpoint).is_file():
            raise RuntimeError(f"HockeyMOM rink checkpoint is unavailable: {checkpoint}")
        original_torch_load = torch.load

        def compatible_torch_load(*load_args, **load_kwargs):
            # The checkpoint is an explicitly supplied, SHA-256-pinned oracle
            # input. Old MMEngine omits this argument and is incompatible with
            # PyTorch 2.6+'s changed default.
            load_kwargs.setdefault("weights_only", False)
            return original_torch_load(*load_args, **load_kwargs)

        torch.load = compatible_torch_load
        try:
            rink = find_ice_rink_masks(
                image=stitched,
                config_file=config_file,
                checkpoint=checkpoint,
                device=device,
                inference_scale=args.rink_inference_scale,
            )
        finally:
            torch.load = original_torch_load
        if rink is None or rink.get("combined_mask") is None:
            raise RuntimeError("HockeyMOM rink reference returned no combined mask")
        mask = numpy_value(rink["combined_mask"]).astype(np.uint8) * 255
        args.output_dir.mkdir(parents=True, exist_ok=True)
        if not cv2.imwrite(str(args.output_dir / "python_rink_mask.png"), mask):
            raise RuntimeError("unable to save Python parity rink mask")
        centroid = numpy_value(rink["centroid"]).reshape(-1).tolist()
        bbox = numpy_value(rink["combined_bbox"]).reshape(-1).tolist()
        output.update(
            {
                "rink_available": True,
                "centroid": [float(centroid[0]), float(centroid[1])],
                "bbox": [float(value) for value in bbox[:4]],
            }
        )
    except Exception as exc:
        output["rink_skip_reason"] = str(exc)

    # The mandatory C++ qualification gate compares the optimized Hugin result
    # against this legacy production matcher. Ordinary developer parity runs
    # keep the comparison optional when HockeyMOM dependencies are absent.
    try:
        from hmlib.stitching.control_points import calculate_control_points

        points = calculate_control_points(
            image_tensor(left_input),
            image_tensor(right_input),
            max_control_points=args.max_control_points,
            device=device,
            max_num_keypoints=2048,
        )
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
        output.update(
            {
                "legacy_superpoint_available": True,
                "legacy_left_points": [[float(x), float(y)] for x, y in points0],
                "legacy_right_points": [[float(x), float(y)] for x, y in points1],
            }
        )
    except Exception as exc:
        output["legacy_superpoint_skip_reason"] = str(exc)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "python_reference.yaml").open("w", encoding="utf-8") as stream:
        yaml.safe_dump(output, stream)


if __name__ == "__main__":
    main()
