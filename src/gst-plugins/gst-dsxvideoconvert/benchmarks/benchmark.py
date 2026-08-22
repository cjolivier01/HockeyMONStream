#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Local black-box performance gate; benchmark results are not persisted."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from dataclasses import dataclass
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile


ORIGINAL = "nvvideoconvert"
REPLACEMENT = "dsxvideoconvert"


@dataclass(frozen=True)
class BenchmarkCase:
    candidate: Callable[[str], list[str]]
    baseline: Callable[[], list[str]] | None = None
    gst_launch: bool = True


def raw_source(frames: int) -> list[str]:
    return [
        "videotestsrc", f"num-buffers={frames}", "pattern=smpte", "!",
        "video/x-raw,format=RGBA,width=1920,height=1080,framerate=30/1", "!",
    ]


def nvmm_source(frames: int) -> list[str]:
    return [
        "nvvideotestsrc", f"num-buffers={frames}", "pattern=smpte", "!",
        "video/x-raw(memory:NVMM),format=RGBA,width=1920,height=1080,"
        "framerate=30/1", "!",
    ]


def sink() -> list[str]:
    return ["fakesink", "sync=false"]


def cases(frames: int) -> dict[str, BenchmarkCase]:
    def raw(plugin: str) -> list[str]:
        return [
            *raw_source(frames), plugin, "name=candidate",
            "interpolation-method=1", "!",
            "video/x-raw,format=NV12,width=1280,height=720", "!", *sink(),
        ]

    def raw_to_nvmm(plugin: str) -> list[str]:
        return [
            *raw_source(frames), plugin, "name=candidate",
            "interpolation-method=1", "!",
            "video/x-raw(memory:NVMM),format=NV12,width=1280,height=720", "!",
            *sink(),
        ]

    def nvmm(plugin: str) -> list[str]:
        return [
            *nvmm_source(frames), plugin, "name=candidate",
            "interpolation-method=1", "!",
            "video/x-raw(memory:NVMM),format=NV12,width=1280,height=720", "!",
            *sink(),
        ]

    def nvmm_to_raw(plugin: str) -> list[str]:
        return [
            *nvmm_source(frames), plugin, "name=candidate",
            "interpolation-method=1", "!",
            "video/x-raw,format=NV12,width=1280,height=720", "!", *sink(),
        ]

    def crop_flip(plugin: str) -> list[str]:
        return [
            *nvmm_source(frames), plugin, "name=candidate",
            "src-crop=101:51:1601:901",
            "flip-method=3", "interpolation-method=2", "!",
            "video/x-raw(memory:NVMM),format=RGBA,width=720,height=1280", "!",
            *sink(),
        ]

    batch_benchmark = Path(os.environ["DSX_BATCH_BENCH"])
    if not batch_benchmark.is_file():
        raise SystemExit(f"batch benchmark is not built: {batch_benchmark}")

    return {
        "raw-scale-convert": BenchmarkCase(raw),
        "raw-to-nvmm": BenchmarkCase(raw_to_nvmm),
        "nvmm-scale-convert": BenchmarkCase(nvmm),
        "nvmm-to-raw": BenchmarkCase(nvmm_to_raw),
        "nvmm-crop-flip": BenchmarkCase(crop_flip),
        "batched-nvmm-scale-convert": BenchmarkCase(
            lambda plugin: [str(batch_benchmark), plugin, str(frames)],
            lambda: [str(batch_benchmark), "identity", str(frames)],
            gst_launch=False,
        ),
    }


def measure(
    command: list[str], registry: Path, *, gst_launch: bool,
    expected_outputs: int,
) -> float:
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(registry)
    if gst_launch:
        environment["GST_TRACERS"] = "latency(flags=element)"
        environment["GST_DEBUG"] = "GST_TRACER:7"
        environment["GST_DEBUG_NO_COLOR"] = "1"
    full_command = (
        ["timeout", "120s", "gst-launch-1.0", "-q", *command]
        if gst_launch
        else ["timeout", "120s", *command]
    )
    result = subprocess.run(
        full_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=125,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"benchmark pipeline failed ({result.returncode}): "
            f"{' '.join(full_command)}\n{result.stderr.decode(errors='replace')}"
        )
    if gst_launch:
        latencies = [
            int(value)
            for value in re.findall(
                rb"element=\(string\)candidate, src=\(string\)[^,]+, "
                rb"time=\(guint64\)(\d+)",
                result.stderr,
            )
        ]
        if not latencies:
            raise RuntimeError(
                "latency tracer did not report the candidate element\n"
                + result.stderr.decode(errors="replace")
            )
        if len(latencies) != expected_outputs:
            raise RuntimeError(
                f"candidate produced {len(latencies)} latency records; "
                f"expected {expected_outputs}"
            )
        warmup = min(5, max(1, len(latencies) // 10))
        measured = latencies[warmup:]
        if not measured:
            measured = latencies
        return statistics.fmean(measured) / 1_000_000_000
    try:
        return float(result.stdout.decode().strip())
    except ValueError as error:
        raise RuntimeError(
            f"batch benchmark returned invalid timing: {result.stdout!r}"
        ) from error


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--max-slowdown", type=float, default=1.20)
    arguments = parser.parse_args()
    if arguments.frames < 1 or arguments.trials < 1 or arguments.max_slowdown < 1:
        parser.error("frames/trials must be positive and max-slowdown must be at least 1")
    if not Path(os.environ["NVVIDEO_PLUGIN_SO"]).is_file():
        raise SystemExit("installed nvvideoconvert is required for performance parity")

    failed: list[str] = []
    with tempfile.TemporaryDirectory(prefix="dsx-benchmark-") as temporary:
        registry = Path(temporary) / "registry.bin"
        for name, benchmark_case in cases(arguments.frames).items():
            if benchmark_case.baseline is not None:
                measure(
                    benchmark_case.baseline(), registry,
                    gst_launch=benchmark_case.gst_launch,
                    expected_outputs=arguments.frames,
                )
            measure(
                benchmark_case.candidate(ORIGINAL), registry,
                gst_launch=benchmark_case.gst_launch,
                expected_outputs=arguments.frames,
            )
            measure(
                benchmark_case.candidate(REPLACEMENT), registry,
                gst_launch=benchmark_case.gst_launch,
                expected_outputs=arguments.frames,
            )
            timings = {"baseline": [], ORIGINAL: [], REPLACEMENT: []}
            for _trial in range(arguments.trials):
                if benchmark_case.baseline is not None:
                    timings["baseline"].append(
                        measure(
                            benchmark_case.baseline(), registry,
                            gst_launch=benchmark_case.gst_launch,
                            expected_outputs=arguments.frames,
                        )
                    )
                for order in ((ORIGINAL, REPLACEMENT), (REPLACEMENT, ORIGINAL)):
                    for candidate in order:
                        timings[candidate].append(
                            measure(
                                benchmark_case.candidate(candidate), registry,
                                gst_launch=benchmark_case.gst_launch,
                                expected_outputs=arguments.frames,
                            )
                        )
            baseline = (
                statistics.median(timings["baseline"])
                if timings["baseline"]
                else 0.0
            )
            original_total = statistics.median(timings[ORIGINAL])
            replacement_total = statistics.median(timings[REPLACEMENT])
            original = max(original_total - baseline, 1e-6)
            replacement = max(replacement_total - baseline, 1e-6)
            ratio = replacement / original
            status = "PASS" if ratio <= arguments.max_slowdown else "FAIL"
            print(
                f"{status} {name}: adjusted-original={original:.3f}s "
                f"adjusted-replacement={replacement:.3f}s "
                f"ratio={ratio:.3f} limit={arguments.max_slowdown:.3f}"
            )
            if ratio > arguments.max_slowdown:
                failed.append(name)
    if failed:
        raise SystemExit(f"performance parity failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
