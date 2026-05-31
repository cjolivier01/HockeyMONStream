#!/usr/bin/env python3
"""Export HockeyMOM's MMYOLO YOLOv8 checkpoint to a DeepStream-friendly ONNX."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


DEFAULT_CHECKPOINT_URL = (
    "https://github.com/cjolivier01/HockeyMOM/releases/download/v0.0.1/"
    "hm_crowdhuman_e85_yolov8_m_1984_736.pth"
)
DEFAULT_ONNX_NAME = "hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx"


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _default_hm_root() -> Path:
    return (_repo_root() / "../hm").resolve()


def _default_config() -> Path:
    return _default_hm_root() / "openmm/configs/hm/hm_crowdhuman_yolov8_m_1984_736.py"


def _default_output() -> Path:
    return _repo_root() / "pretrained/deepstream/yolov8" / DEFAULT_ONNX_NAME


def _prepend_openmm_paths(hm_root: Path) -> None:
    openmm = hm_root / "openmm"
    for path in ("mmyolo", "mmdetection", "mmcv", "mmengine", "mmeval"):
        candidate = openmm / path
        if candidate.exists():
            sys.path.insert(0, str(candidate))


def _download(url: str, path: Path, timeout: float) -> None:
    _ensure_parent_dir(path)
    request = urllib.request.Request(url, headers={"User-Agent": "hstream-yolov8-export"})
    tmp_fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response, tmp_path.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
        tmp_path.replace(path)
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def _ensure_parent_dir(path: Path) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        return
    except OSError:
        pass

    real_parent = path.parent.resolve(strict=False)
    uid = os.getuid()
    gid = os.getgid()
    subprocess.run(["sudo", "mkdir", "-p", str(real_parent)], check=True)
    subprocess.run(["sudo", "chown", "-R", f"{uid}:{gid}", str(real_parent)], check=True)
    path.parent.mkdir(parents=True, exist_ok=True)


def _resolve_checkpoint(raw: str, timeout: float) -> Path:
    if raw.startswith(("http://", "https://")):
        target = Path.home() / ".cache/torch/hub/checkpoints" / Path(raw).name
        if not target.exists() or target.stat().st_size == 0:
            print(f"Downloading checkpoint: {target}")
            _download(raw, target, timeout)
        return target
    return Path(os.path.expanduser(os.path.expandvars(raw))).resolve()


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hm-root", type=Path, default=_default_hm_root(), help="Sibling hm checkout root")
    parser.add_argument("--config", type=Path, default=_default_config(), help="MMYOLO detector config")
    parser.add_argument("--checkpoint", default=DEFAULT_CHECKPOINT_URL, help="Checkpoint path or URL")
    parser.add_argument("--output", type=Path, default=_default_output(), help="Output ONNX path")
    parser.add_argument("--size", type=int, nargs=2, metavar=("HEIGHT", "WIDTH"), default=(736, 1984))
    parser.add_argument("--batch-size", type=int, default=2)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--dynamic-batch", action="store_true")
    parser.add_argument("--timeout", type=float, default=120.0)
    return parser


def main() -> int:
    args = _build_arg_parser().parse_args()

    hm_root = args.hm_root.resolve()
    _prepend_openmm_paths(hm_root)

    import torch
    import onnx
    from mmdet.apis import init_detector

    class DeepStreamYOLOv8(torch.nn.Module):
        def __init__(self, detector: torch.nn.Module):
            super().__init__()
            self.detector = detector
            self.head = detector.bbox_head

        def forward(self, images: torch.Tensor) -> torch.Tensor:
            cls_scores, bbox_preds = self.detector(images, data_samples=None, mode="tensor")
            featmap_sizes = [cls_score.shape[2:] for cls_score in cls_scores]
            priors = self.head.prior_generator.grid_priors(
                featmap_sizes, dtype=bbox_preds[0].dtype, device=bbox_preds[0].device)

            flatten_priors = torch.cat(priors)
            flatten_strides = []
            for stride, prior in zip(self.head.featmap_strides, priors):
                flatten_strides.append(flatten_priors.new_full((prior.shape[0],), stride))
            flatten_stride = torch.cat(flatten_strides)

            flatten_cls_scores = [
                cls_score.permute(0, 2, 3, 1).reshape(images.shape[0], -1, self.head.num_classes)
                for cls_score in cls_scores
            ]
            flatten_bbox_preds = [
                bbox_pred.permute(0, 2, 3, 1).reshape(images.shape[0], -1, 4)
                for bbox_pred in bbox_preds
            ]

            scores = torch.cat(flatten_cls_scores, dim=1).sigmoid()
            bboxes = torch.cat(flatten_bbox_preds, dim=1)
            bboxes = self.head.bbox_coder.decode(flatten_priors[None], bboxes, flatten_stride)
            max_scores, labels = torch.max(scores, dim=-1, keepdim=True)
            return torch.cat((bboxes, max_scores, labels.to(bboxes.dtype)), dim=-1)

    checkpoint = _resolve_checkpoint(args.checkpoint, args.timeout)
    config = args.config.resolve()
    output = args.output.resolve()
    _ensure_parent_dir(output)

    detector = init_detector(str(config), str(checkpoint), device=args.device)
    detector.eval()
    for parameter in detector.parameters():
        parameter.requires_grad_(False)

    wrapper = DeepStreamYOLOv8(detector).eval()
    height, width = args.size
    dummy = torch.zeros(args.batch_size, 3, height, width, device=args.device)
    dynamic_axes = None
    if args.dynamic_batch:
        dynamic_axes = {"input": {0: "batch"}, "output": {0: "batch"}}

    print(f"Exporting {checkpoint} -> {output}")
    torch.onnx.export(
        wrapper,
        dummy,
        str(output),
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=dynamic_axes,
        opset_version=args.opset,
        do_constant_folding=True,
        dynamo=False)

    model = onnx.load(output)
    onnx.checker.check_model(model)
    print(f"Wrote ONNX: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
