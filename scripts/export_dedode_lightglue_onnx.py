#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2018 Kornia Team
# SPDX-FileCopyrightText: 2023 ETH Zurich
# SPDX-FileCopyrightText: 2023 Johan Edstedt
# SPDX-FileCopyrightText: 2026 HStream contributors
# SPDX-License-Identifier: Apache-2.0
#
# This exporter adapts Kornia 0.8.3's Apache-2.0 DeDoDe and LightGlue
# implementations, which in turn derive from Johan Edstedt's MIT-licensed
# DeDoDe and ETH Zurich's Apache-2.0 LightGlue projects. HStream modified the
# upstream routines for deterministic fixed-shape ONNX export. The applicable
# license texts and source revision notices are retained in
# third_party/native_model_licenses/.
"""Export the fixed-shape DeDoDe-B + LightGlue calibration pipeline.

The generated model is a build/release artifact, not a Python runtime
dependency. This exporter is pinned to Kornia 0.8.3 and PyTorch 2.11.0 and
verifies the three downloaded checkpoint digests before writing the graph.
"""

import argparse
import hashlib
import io
from pathlib import Path
import types

import torch
import torch.nn.functional as F
from torch import nn

import kornia
import kornia.feature as KF
import kornia.feature.lightglue as lightglue_module


CHECKPOINTS = {
    "dedode_detector_L_v2.pth": "4113809dd9e0367af013a45fc2255a6b243ff241cd06520d17a65d9e231bdc17",
    "dedode_descriptor_B.pth": "8eccfc270ec990ced60cd54a434411a47c4c504de13586f596d042e005b3022b",
    "dedodeb_lightglue.pth": "c11800f3faf106e6c44423975a688aee8be79c9b2181bb8ac85e1698238bf766",
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def checkpoint_path(filename: str) -> Path:
    return Path(torch.hub.get_dir()) / "checkpoints" / filename


def verify_checkpoints() -> None:
    checkpoint_dir = Path(torch.hub.get_dir()) / "checkpoints"
    for filename, expected in CHECKPOINTS.items():
        path = checkpoint_dir / filename
        if not path.is_file():
            raise RuntimeError(f"expected downloaded checkpoint {path} is missing")
        actual = digest(path)
        if actual != expected:
            raise RuntimeError(
                f"checkpoint {path} has SHA-256 {actual}, expected {expected}"
            )


def load_verified_state_dict(filename: str):
    path = checkpoint_path(filename)
    payload = path.read_bytes()
    actual = hashlib.sha256(payload).hexdigest()
    expected = CHECKPOINTS[filename]
    if actual != expected:
        raise RuntimeError(
            f"checkpoint {path} changed before loading: SHA-256 {actual}, expected {expected}"
        )
    return torch.load(io.BytesIO(payload), map_location="cpu", weights_only=True)


def patched_cross_forward(self, x0, x1, mask=None):
    del mask
    qk0, qk1 = self.map_(self.to_qk, x0, x1)
    v0, v1 = self.map_(self.to_v, x0, x1)
    batch, count, inner_dim = qk0.shape
    head_dim = inner_dim // self.heads
    qk0, qk1, v0, v1 = (
        tensor.reshape(batch, count, self.heads, head_dim).permute(0, 2, 1, 3)
        for tensor in (qk0, qk1, v0, v1)
    )
    qk0, qk1 = qk0 * self.scale**0.5, qk1 * self.scale**0.5
    similarity = torch.einsum("bhid,bhjd->bhij", qk0, qk1)
    attention01 = F.softmax(similarity, dim=3)
    attention10 = F.softmax(similarity.transpose(2, 3).contiguous(), dim=3)
    message0 = torch.einsum("bhij,bhjd->bhid", attention01, v1)
    message1 = torch.einsum("bhji,bhjd->bhid", attention10.transpose(2, 3), v0)
    message0, message1 = self.map_(
        lambda tensor: tensor.permute(0, 2, 1, 3).reshape(batch, count, inner_dim),
        message0,
        message1,
    )
    message0, message1 = self.map_(self.to_out, message0, message1)
    x0 = x0 + self.ffn(torch.cat([x0, message0], dim=2))
    x1 = x1 + self.ffn(torch.cat([x1, message1], dim=2))
    return x0, x1


def patched_positional_forward(self, points):
    projected = self.Wr(points)
    embedding = torch.stack(
        (torch.cos(projected), torch.sin(projected)), dim=0
    ).unsqueeze(2)
    return torch.repeat_interleave(embedding, 2, dim=4)


def patched_self_forward(self, values, encoding, mask=None):
    del mask
    batch, count, _ = values.shape
    qkv = (
        self.Wqkv(values)
        .reshape(batch, count, self.num_heads, self.head_dim, 3)
        .permute(0, 2, 1, 3, 4)
    )
    query, key, value = qkv[..., 0], qkv[..., 1], qkv[..., 2]

    def apply_rotary(tensor):
        first = tensor[..., ::2]
        second = tensor[..., 1::2]
        rotated = torch.stack((-second, first), dim=4).reshape(
            batch, self.num_heads, count, self.head_dim
        )
        return tensor * encoding[0] + rotated * encoding[1]

    query = apply_rotary(query)
    key = apply_rotary(key)
    similarity = torch.einsum("bhid,bhjd->bhij", query, key) * self.head_dim**-0.5
    context = torch.einsum("bhij,bhjd->bhid", F.softmax(similarity, dim=3), value)
    message = self.out_proj(
        context.permute(0, 2, 1, 3).reshape(batch, count, self.embed_dim)
    )
    return values + self.ffn(torch.cat((values, message), dim=2))


def patched_assignment(similarity, certainty0, certainty1):
    batch, left_count, right_count = similarity.shape
    del batch, left_count, right_count
    certainties = F.logsigmoid(certainty0) + F.logsigmoid(certainty1).transpose(1, 2)
    interior = (
        F.log_softmax(similarity, dim=2)
        + F.log_softmax(similarity.transpose(1, 2).contiguous(), dim=2).transpose(1, 2)
        + certainties
    )
    left_dustbin = F.logsigmoid(-certainty0)
    upper = torch.cat([interior, left_dustbin], dim=2)
    right_dustbin = F.logsigmoid(-certainty1.squeeze(2)).unsqueeze(1)
    corner = torch.zeros_like(right_dustbin[:, :, :1])
    lower = torch.cat([right_dustbin, corner], dim=2)
    return torch.cat([upper, lower], dim=1)


class DeDoDeLightGluePipeline(nn.Module):
    def __init__(self, keypoint_count: int, height: int, width: int) -> None:
        super().__init__()
        self.keypoint_count = keypoint_count
        self.height = height
        self.width = width
        self.dedode = KF.DeDoDe(
            detector_model="L", descriptor_model="B", amp_dtype=torch.float32
        )
        self.dedode.detector.load_state_dict(
            load_verified_state_dict("dedode_detector_L_v2.pth")
        )
        self.dedode.descriptor.load_state_dict(
            load_verified_state_dict("dedode_descriptor_B.pth")
        )
        lightglue_config = dict(KF.LightGlue.features["dedodeb"])
        lightglue_config.update(
            weights=None,
            depth_confidence=-1,
            width_confidence=-1,
            flash=False,
            mp=False,
        )
        self.lightglue = KF.LightGlue(None, **lightglue_config)
        incompatible = self.lightglue.load_state_dict(
            load_verified_state_dict("dedodeb_lightglue.pth"), strict=False
        )
        if incompatible.missing_keys != ["confidence_thresholds"] or incompatible.unexpected_keys:
            raise RuntimeError(f"unexpected DeDoDe-B LightGlue checkpoint contract: {incompatible}")
        self.lightglue.posenc.forward = types.MethodType(
            patched_positional_forward, self.lightglue.posenc
        )
        for transformer in self.lightglue.transformers:
            transformer.self_attn.forward = types.MethodType(
                patched_self_forward, transformer.self_attn
            )
            transformer.cross_attn.forward = types.MethodType(
                patched_cross_forward, transformer.cross_attn
            )
        self.register_buffer(
            "mean",
            torch.tensor([0.485, 0.456, 0.406], dtype=torch.float32).view(1, 3, 1, 1),
        )
        self.register_buffer(
            "std",
            torch.tensor([0.229, 0.224, 0.225], dtype=torch.float32).view(1, 3, 1, 1),
        )
        xs = (torch.arange(width, dtype=torch.float32) + 0.5) / width * 2.0 - 1.0
        ys = (torch.arange(height, dtype=torch.float32) + 0.5) / height * 2.0 - 1.0
        yy, xx = torch.meshgrid(ys, xs, indexing="ij")
        self.register_buffer(
            "sampling_grid",
            torch.stack((xx, yy), dim=2).reshape(1, height * width, 2).repeat(2, 1, 1),
        )
        coverage = (-(torch.linspace(-2, 2, steps=51, dtype=torch.float32) ** 2)).exp()
        self.register_buffer("coverage_x", coverage.reshape(1, 1, 1, 51))
        self.register_buffer("coverage_y", coverage.reshape(1, 1, 51, 1))

    def forward(self, images):
        normalized = (images - self.mean) / self.std
        logits = self.dedode.detector(normalized)[..., : self.height, : self.width]
        scoremap = (
            logits.reshape(2, self.height * self.width)
            .softmax(dim=1)
            .reshape(2, self.height, self.width)
        )
        density_x = F.conv2d(
            (scoremap[:, None] + 1e-6) * 10000, self.coverage_x, padding=(0, 25)
        )
        density = F.conv2d(density_x, self.coverage_y, padding=(25, 0))[:, 0]
        covered_scoremap = scoremap * (density + 1e-8) ** (-0.5)
        indices = torch.topk(
            covered_scoremap.reshape(2, self.height * self.width), k=self.keypoint_count
        ).indices
        gather_indices = torch.stack((indices, indices), dim=2)
        normalized_keypoints = torch.gather(
            self.sampling_grid, dim=1, index=gather_indices
        )
        dense_descriptors = self.dedode.descriptor(normalized)[
            ..., : self.height, : self.width
        ]
        descriptors = F.grid_sample(
            dense_descriptors.float(),
            normalized_keypoints[:, None],
            mode="bilinear",
            align_corners=False,
        )[:, :, 0].permute(0, 2, 1)
        keypoints = torch.stack(
            (
                self.width * (normalized_keypoints[..., 0] + 1.0) / 2.0,
                self.height * (normalized_keypoints[..., 1] + 1.0) / 2.0,
            ),
            dim=2,
        ).float()
        image_size = torch.tensor(
            [[self.width, self.height]], dtype=torch.float32, device=images.device
        )
        prediction = self.lightglue(
            {
                "image0": {
                    "keypoints": keypoints[0:1],
                    "descriptors": descriptors[0:1],
                    "image_size": image_size,
                },
                "image1": {
                    "keypoints": keypoints[1:2],
                    "descriptors": descriptors[1:2],
                    "image_size": image_size,
                },
            }
        )
        return keypoints, prediction["matches0"], prediction["matching_scores0"]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=576)
    parser.add_argument("--keypoints", type=int, default=1024)
    args = parser.parse_args()
    expected_torch = "2.11.0a0+gita03d450"
    if kornia.__version__ != "0.8.3" or torch.__version__ != expected_torch:
        raise RuntimeError(
            f"expected Kornia 0.8.3 and PyTorch {expected_torch}, got {kornia.__version__} and {torch.__version__}"
        )

    verify_checkpoints()
    model = DeDoDeLightGluePipeline(args.keypoints, args.height, args.width).eval()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    original_assignment = lightglue_module.sigmoid_log_double_softmax
    lightglue_module.sigmoid_log_double_softmax = patched_assignment
    try:
        with torch.inference_mode():
            torch.onnx.export(
                model,
                torch.zeros(2, 3, args.height, args.width, dtype=torch.float32),
                args.output,
                input_names=["images"],
                output_names=["keypoints", "matches0", "matching_scores0"],
                opset_version=17,
                dynamo=False,
            )
    finally:
        lightglue_module.sigmoid_log_double_softmax = original_assignment
    print(f"{digest(args.output)}  {args.output}")


if __name__ == "__main__":
    main()
