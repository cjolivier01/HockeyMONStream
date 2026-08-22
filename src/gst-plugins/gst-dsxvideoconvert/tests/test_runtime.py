#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Black-box pixel and negotiation parity tests for dsxvideoconvert."""

from __future__ import annotations

import os
from pathlib import Path
import platform
import subprocess
import tempfile


ORIGINAL = "nvvideoconvert"
REPLACEMENT = "dsxvideoconvert"
IS_JETSON = platform.machine() == "aarch64" and Path("/etc/nv_tegra_release").exists()
if IS_JETSON:
    RAW_FORMATS = (
        "I420", "NV12", "P010_10LE", "BGRx", "RGBA", "GRAY8",
        "GRAY16_LE", "RGB", "BGR", "BGR10A2_LE", "UYVP", "UYVY",
    )
    NVMM_ONLY_FORMATS = ("I420_12LE",)
    MEMORY_TYPES = (0, 1, 2, 3, 4)
    YUV_DEST_FORMATS = (
        "I420", "NV12", "P010_10LE", "UYVP", "UYVY",
    )
else:
    RAW_FORMATS = (
        "I420", "NV12", "P010_10LE", "BGRx", "RGBA", "Y444", "GRAY8",
        "GRAY16_LE", "GBR", "RGB", "BGR", "BGR10A2_LE", "RGB10A2_LE",
        "UYVP", "UYVY",
    )
    NVMM_ONLY_FORMATS = ("I420_12LE", "Y444_10LE", "Y444_12LE")
    MEMORY_TYPES = (0, 1, 2, 3)
    YUV_DEST_FORMATS = ("I420", "NV12", "P010_10LE", "Y444", "UYVP", "UYVY")

JETSON_GPU_ONLY_FORMATS = {
    "GRAY16_LE", "RGB", "BGR", "BGR10A2_LE", "RGB10A2_LE", "UYVP",
    "I420_12LE", "Y444", "Y444_10LE", "Y444_12LE", "BGRA64_LE",
}
JETSON_GPU_PROPERTIES = ("compute-hw=1", "copy-hw=1", "nvbuf-memory-type=2")


def element(name: str, *properties: str) -> list[str]:
    return [name, *properties]


def caps(value: str) -> list[str]:
    return ["!", value, "!"]


def format_properties(format_name: str) -> tuple[str, ...]:
    if IS_JETSON and format_name in JETSON_GPU_ONLY_FORMATS:
        return JETSON_GPU_PROPERTIES
    return ()


def oracle_supports(format_name: str) -> bool:
    result = subprocess.run(
        ["gst-inspect-1.0", ORIGINAL],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    return format_name in result.stdout


def run_pipeline(
    tokens: list[str],
    output: Path,
    expect_success: bool = True,
    expected_error: str | None = None,
    environment_updates: dict[str, str] | None = None,
) -> None:
    command = ["timeout", "25s", "gst-launch-1.0", "-q", *tokens]
    separator = [] if tokens and tokens[-1] == "!" else ["!"]
    if expect_success:
        command.extend([*separator, "filesink", f"location={output}"])
    else:
        command.extend([*separator, "fakesink"])
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(output.parent / "registry.bin")
    if environment_updates is not None:
        environment.update(environment_updates)
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=30,
    )
    if expect_success and result.returncode != 0:
        raise AssertionError(
            f"pipeline failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    if not expect_success and result.returncode != 1:
        raise AssertionError(
            f"pipeline did not fail with the expected negotiation error "
            f"(status {result.returncode}): {' '.join(command)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    if not expect_success and expected_error is not None:
        stderr = result.stderr.decode(errors="replace")
        if expected_error not in stderr:
            raise AssertionError(
                f"pipeline failed for the wrong reason; expected {expected_error!r}: "
                f"{' '.join(command)}\n{stderr}"
            )


def compare_pair(directory: Path, label: str, make_tokens) -> None:
    outputs: dict[str, Path] = {}
    for candidate in (ORIGINAL, REPLACEMENT):
        output = directory / f"{label}.{candidate}.raw"
        run_pipeline(make_tokens(candidate), output)
        outputs[candidate] = output
    original = outputs[ORIGINAL].read_bytes()
    replacement = outputs[REPLACEMENT].read_bytes()
    if not original:
        raise AssertionError(f"{label}: original pipeline produced an empty buffer")
    if not replacement:
        raise AssertionError(f"{label}: replacement pipeline produced an empty buffer")
    if IS_JETSON:
        return
    if original != replacement:
        differences = sum(left != right for left, right in zip(original, replacement))
        differences += abs(len(original) - len(replacement))
        raise AssertionError(
            f"{label}: output differs in {differences} bytes "
            f"({len(original)} versus {len(replacement)} bytes)"
        )


def raw_source(format_name: str = "RGBA", width: int = 320, height: int = 240) -> list[str]:
    return [
        "videotestsrc",
        "num-buffers=1",
        "pattern=smpte",
        *caps(
            f"video/x-raw,format={format_name},width={width},height={height},"
            "framerate=30/1"
        ),
    ]


def test_raw_formats(directory: Path) -> int:
    count = 0
    for format_name in RAW_FORMATS:
        compare_pair(
            directory,
            f"raw-output-{format_name}",
            lambda candidate, fmt=format_name: [
                *raw_source(),
                *element(candidate, *format_properties(fmt)),
                *caps(f"video/x-raw,format={fmt},width=160,height=120"),
            ],
        )
        compare_pair(
            directory,
            f"raw-input-{format_name}",
            lambda candidate, fmt=format_name: [
                *raw_source(fmt),
                *element(candidate, *format_properties(fmt)),
                *caps("video/x-raw,format=RGBA,width=160,height=120"),
            ],
        )
        count += 2
    return count


def test_transforms(directory: Path) -> int:
    cases: list[tuple[str, tuple[str, ...], str]] = [
        ("odd-output", (), "video/x-raw,format=RGBA,width=161,height=121"),
        ("odd-input", (), "video/x-raw,format=RGBA,width=127,height=95"),
        ("src-crop", ("src-crop=23:17:201:155",), "video/x-raw,format=RGBA,width=160,height=120"),
        ("dest-crop", ("dest-crop=13:9:121:91",), "video/x-raw,format=RGBA,width=160,height=120"),
        ("even-crop", ("src-crop=23:17:201:155", "allow-odd-crop=false"), "video/x-raw,format=NV12,width=160,height=120"),
        ("forced-transform", ("disable-passthrough=true",), "video/x-raw,format=RGBA,width=320,height=240"),
        ("bt601-full", (), "video/x-raw,format=NV12,width=160,height=120,colorimetry=1:4:5:4"),
        ("bt601-limited", (), "video/x-raw,format=NV12,width=160,height=120,colorimetry=2:4:5:4"),
        ("bt709-full", (), "video/x-raw,format=NV12,width=160,height=120,colorimetry=1:3:5:1"),
        ("bt709-limited", (), "video/x-raw,format=NV12,width=160,height=120,colorimetry=2:3:5:1"),
    ]
    count = 0
    for label, properties, output_caps in cases:
        width, height = (161, 121) if label == "odd-input" else (320, 240)
        compare_pair(
            directory,
            label,
            lambda candidate, props=properties, target=output_caps, w=width, h=height: [
                *raw_source(width=w, height=h),
                *element(candidate, *props),
                *caps(target),
            ],
        )
        count += 1

    for method in range(8):
        rotated = method in (1, 3, 5, 7)
        output_caps = (
            "video/x-raw,format=RGBA,width=120,height=160"
            if rotated
            else "video/x-raw,format=RGBA,width=160,height=120"
        )
        compare_pair(
            directory,
            f"flip-{method}",
            lambda candidate, value=method, target=output_caps: [
                *raw_source(),
                *element(candidate, f"flip-method={value}"),
                *caps(target),
            ],
        )
        count += 1

    for method in range(7):
        compare_pair(
            directory,
            f"interpolation-{method}",
            lambda candidate, value=method: [
                *raw_source(),
                *element(candidate, f"interpolation-method={value}"),
                *caps("video/x-raw,format=RGBA,width=173,height=127"),
            ],
        )
        count += 1

    for format_name in YUV_DEST_FORMATS:
        for memory in ("video/x-raw", "video/x-raw(memory:NVMM)"):
            compare_pair(
                directory,
                f"dest-crop-{format_name}-{memory == 'video/x-raw(memory:NVMM)'}",
                lambda candidate, target=memory, fmt=format_name: [
                    *raw_source(),
                    *element(candidate, "dest-crop=13:9:121:91", *format_properties(fmt)),
                    *caps(f"{target},format={fmt},width=160,height=120"),
                    *element(
                        ORIGINAL if "NVMM" in target else "videoconvert",
                        *(format_properties(fmt) if "NVMM" in target else ()),
                    ),
                    *caps("video/x-raw,format=RGBA"),
                ],
            )
            count += 1

    for format_name in RAW_FORMATS:
        compare_pair(
            directory,
            f"odd-raw-output-{format_name}",
            lambda candidate, fmt=format_name: [
                *raw_source(width=319, height=239),
                *element(candidate, *format_properties(fmt)),
                *caps(f"video/x-raw,format={fmt},width=161,height=121"),
                *element("videoconvert"),
                *caps("video/x-raw,format=RGBA,width=161,height=121"),
            ],
        )
        count += 1
    return count


def test_nvmm(directory: Path) -> int:
    count = 0
    for format_name in (*RAW_FORMATS, *NVMM_ONLY_FORMATS):
        compare_pair(
            directory,
            f"nvmm-output-{format_name}",
            lambda candidate, fmt=format_name: [
                *raw_source(),
                *element(candidate, *format_properties(fmt)),
                *caps(f"video/x-raw(memory:NVMM),format={fmt},width=160,height=120"),
                *element(ORIGINAL, *format_properties(fmt)),
                *caps("video/x-raw,format=RGBA"),
            ],
        )
        compare_pair(
            directory,
            f"nvmm-input-{format_name}",
            lambda candidate, fmt=format_name: [
                *raw_source(),
                *element(ORIGINAL, *format_properties(fmt)),
                *caps(f"video/x-raw(memory:NVMM),format={fmt}"),
                *element(candidate, *format_properties(fmt)),
                *caps("video/x-raw,format=RGBA,width=160,height=120"),
            ],
        )
        count += 2

    for memory_type in MEMORY_TYPES:
        gpu_properties = (
            JETSON_GPU_PROPERTIES if IS_JETSON and memory_type in (1, 2, 3) else ()
        )
        compare_pair(
            directory,
            f"memory-type-{memory_type}",
            lambda candidate, value=memory_type: [
                *raw_source(),
                *element(candidate, f"nvbuf-memory-type={value}", *gpu_properties),
                *caps("video/x-raw(memory:NVMM),format=NV12,width=160,height=120"),
                *element(ORIGINAL, *gpu_properties),
                *caps("video/x-raw,format=RGBA"),
            ],
        )
        count += 1
    return count


def test_bgra64(directory: Path) -> int:
    if not oracle_supports("BGRA64_LE"):
        return 0
    compare_pair(
        directory,
        "bgra64-raw",
        lambda candidate: [
            *raw_source(),
            *element(ORIGINAL, *format_properties("UYVP")),
            *caps("video/x-raw,format=UYVP"),
            *element(candidate, *format_properties("BGRA64_LE")),
            *caps("video/x-raw,format=BGRA64_LE,width=160,height=120"),
        ],
    )
    for candidate in (ORIGINAL, REPLACEMENT):
        output = directory / f"bgra64-negative.{candidate}.raw"
        run_pipeline(
            [
                *raw_source(),
                *element(candidate, *format_properties("BGRA64_LE")),
                *caps("video/x-raw,format=BGRA64_LE"),
            ],
            output,
            expect_success=False,
            expected_error=(
                "buffer transform failed"
                if candidate == ORIGINAL
                else "not-negotiated"
            ),
        )
    return 2


def test_hardware_controls(directory: Path) -> int:
    count = 0
    copy_methods = (1, 2) if IS_JETSON else (1,)
    for method in copy_methods:
        compare_pair(
            directory,
            f"copy-hw-{method}-raw-to-nvmm",
            lambda candidate, value=method: [
                *raw_source(),
                *element(candidate, f"copy-hw={value}"),
                *caps(
                    "video/x-raw(memory:NVMM),format=NV12,width=160,height=120"
                ),
                *element(ORIGINAL),
                *caps("video/x-raw,format=RGBA"),
            ],
        )
        compare_pair(
            directory,
            f"copy-hw-{method}-nvmm-to-raw",
            lambda candidate, value=method: [
                *raw_source(),
                *element(ORIGINAL),
                *caps("video/x-raw(memory:NVMM),format=RGBA"),
                *element(candidate, f"copy-hw={value}"),
                *caps("video/x-raw,format=NV12,width=160,height=120"),
            ],
        )
        count += 2

    compute_methods = (1, 2) if IS_JETSON else (1,)
    for method in compute_methods:
        compare_pair(
            directory,
            f"compute-hw-{method}",
            lambda candidate, value=method: [
                *raw_source(),
                *element(ORIGINAL),
                *caps("video/x-raw(memory:NVMM),format=RGBA"),
                *element(
                    candidate,
                    f"compute-hw={value}",
                    *(("copy-hw=2",) if value == 2 else ()),
                ),
                *caps("video/x-raw,format=NV12,width=160,height=120"),
            ],
        )
        count += 1

    if IS_JETSON:
        trace_library = Path(os.environ["DSX_COPY_TRACE_SO"])
        for method, expected_events in (
            (1, ("transform=1",)),
            (2, ("raw-to-surface", "surface-to-raw")),
        ):
            trace_file = directory / f"copy-hw-{method}.trace"
            output = directory / f"copy-hw-{method}.raw"
            run_pipeline(
                [
                    *raw_source(),
                    *element(REPLACEMENT, "compute-hw=0", f"copy-hw={method}"),
                    *caps("video/x-raw(memory:NVMM),format=NV12,width=160,height=120"),
                    *element(REPLACEMENT, "compute-hw=0", f"copy-hw={method}"),
                    *caps("video/x-raw,format=RGBA,width=80,height=60"),
                ],
                output,
                environment_updates={
                    "LD_PRELOAD": str(trace_library),
                    "DSX_COPY_TRACE_FILE": str(trace_file),
                },
            )
            events = trace_file.read_text().splitlines()
            for expected in expected_events:
                if expected not in events:
                    raise AssertionError(
                        f"copy-hw={method} did not use {expected}: {events}"
                    )
            count += 1
    return count


def main() -> None:
    if not Path(os.environ["NVVIDEO_PLUGIN_SO"]).is_file():
        raise SystemExit("installed nvvideoconvert is required for black-box parity")
    with tempfile.TemporaryDirectory(prefix="dsx-runtime-") as temporary:
        directory = Path(temporary)
        checks = 0
        checks += test_raw_formats(directory)
        checks += test_transforms(directory)
        checks += test_nvmm(directory)
        checks += test_bgra64(directory)
        checks += test_hardware_controls(directory)
    qualifier = (
        "Jetson runtime compatibility cases; headless oracle bytes are not stable"
        if IS_JETSON
        else "black-box runtime parity cases"
    )
    print(f"PASS: {checks} {qualifier}")


if __name__ == "__main__":
    main()
