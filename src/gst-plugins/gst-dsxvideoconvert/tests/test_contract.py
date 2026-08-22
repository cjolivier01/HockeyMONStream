#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Compare the public GStreamer and ELF contracts without implementation inspection."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile


ORIGINAL = Path(os.environ["NVVIDEO_PLUGIN_SO"])
REPLACEMENT = Path(os.environ["DSX_PLUGIN_SO"])


def run(*command: str, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        timeout=30,
    )
    return result.stdout


def inspect(element: str, registry: Path) -> str:
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(registry)
    return run("gst-inspect-1.0", element, env=environment)


def pad_contract(text: str) -> str:
    start = text.index("Pad Templates:")
    end = text.index("Element has no clocking capabilities.")
    contract = "\n".join(line.rstrip() for line in text[start:end].splitlines())

    def canonicalize_formats(match: re.Match[str]) -> str:
        formats = sorted(re.findall(r"\(string\)([A-Za-z0-9_]+)", match.group(2)))
        values = ", ".join(f"(string){value}" for value in formats)
        return f"{match.group(1)}{{ {values} }}{match.group(3)}"

    return re.sub(
        r"(?m)^(\s+format: )\{([^}]*)\}(.*)$",
        canonicalize_formats,
        contract,
    )


def property_contract(text: str) -> dict[str, tuple[str, tuple[tuple[str, str], ...]]]:
    properties = text.split("Element Properties:\n", 1)[1]
    starts = list(re.finditer(r"^  ([a-z][a-z0-9-]*)\s+:", properties, re.MULTILINE))
    result: dict[str, tuple[str, tuple[tuple[str, str], ...]]] = {}
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(properties)
        block = properties[match.start():end]
        flags = re.search(r"^\s+flags: (.+)$", block, re.MULTILINE)
        value = re.search(
            r'^\s+(Boolean|String|Unsigned Integer|Integer|Enum "[^"]+"|'
            r'Object of type "[^"]+")'
            r'(\..+| .+)?$',
            block,
            re.MULTILINE,
        )
        if flags is None or value is None:
            raise AssertionError(f"could not parse property {match.group(1)!r}\n{block}")
        value_contract = re.sub(r'Enum "[^"]+"', "Enum", value.group(0).strip())
        if match.group(1) == "name":
            value_contract = re.sub(r'Default: "[^"]+"', 'Default: "<element>"',
                                    value_contract)
        enum_values = tuple(
            re.findall(r"^\s+\((-?\d+)\):\s+(\S+)", block, re.MULTILINE)
        )
        result[match.group(1)] = (
            f"{flags.group(1)} | {value_contract}",
            enum_values,
        )
    return result


def dynamic_symbols(path: Path, defined: bool) -> set[str]:
    flag = "--defined-only" if defined else "--undefined-only"
    output = run("nm", "-D", flag, str(path))
    return {line.split()[-1].split("@")[0] for line in output.splitlines() if line.split()}


def negotiated_src_caps(element: str, nvmm_input: bool, nvmm_output: bool) -> dict[str, str]:
    output_media = "video/x-raw(memory:NVMM)" if nvmm_output else "video/x-raw"
    input_path = [
        "videotestsrc", "num-buffers=1", "!",
        "video/x-raw,format=RGBA,width=320,height=240,framerate=30/1", "!",
    ]
    if nvmm_input:
        input_path.extend([
            "nvvideoconvert", "!",
            "video/x-raw(memory:NVMM),format=RGBA,width=320,height=240,"
            "framerate=30/1,batch-size=1,num-surfaces-per-frame=1", "!",
        ])
    command = [
        "gst-launch-1.0", "-v", *input_path, element, "name=candidate", "!",
        f"{output_media},format=NV12,width=160,height=120", "!",
        "fakesink", "sync=false",
    ]
    output = run(*command)
    marker = "candidate.GstPad:src: caps = "
    caps_line = next((line for line in output.splitlines() if marker in line), None)
    if caps_line is None:
        raise AssertionError(f"could not find negotiated source caps for {element}")
    caps_text = caps_line.split(marker, 1)[1]
    result = {"media": caps_text.split(",", 1)[0]}
    for name, value in re.findall(r"([a-z][a-z0-9-]*)=\([^)]*\)([^,]+)", caps_text):
        result[name] = value.strip()
    return result


def main() -> None:
    if not ORIGINAL.is_file():
        raise SystemExit(f"original plugin is required for parity checks: {ORIGINAL}")
    if not REPLACEMENT.is_file():
        raise SystemExit(f"replacement plugin has not been built: {REPLACEMENT}")

    with tempfile.TemporaryDirectory(prefix="dsx-contract-") as temporary:
        registry = Path(temporary) / "registry.bin"
        original = inspect("nvvideoconvert", registry)
        replacement = inspect("dsxvideoconvert", registry)

    if pad_contract(original) != pad_contract(replacement):
        raise AssertionError("sink/source pad caps differ from nvvideoconvert")
    if property_contract(original) != property_contract(replacement):
        raise AssertionError("property names, flags, defaults, ranges, or enum values differ")

    for input_nvmm, output_nvmm in ((False, False), (False, True), (True, False), (True, True)):
        original_caps = negotiated_src_caps(ORIGINAL.stem.removeprefix("libgst"), input_nvmm,
                                            output_nvmm)
        replacement_caps = negotiated_src_caps("dsxvideoconvert", input_nvmm, output_nvmm)
        if original_caps != replacement_caps:
            raise AssertionError(
                f"negotiated caps differ for NVMM {input_nvmm}->{output_nvmm}:\n"
                f"original={original_caps}\nreplacement={replacement_caps}"
            )

    defined = dynamic_symbols(REPLACEMENT, True)
    required_identity = {
        "gst_dsx_video_convert_get_type",
        "gst_plugin_dsxvideoconvert_get_desc",
        "gst_plugin_dsxvideoconvert_register",
    }
    missing = required_identity - defined
    if missing:
        raise AssertionError(f"missing replacement identity symbols: {sorted(missing)}")
    unexpected = defined - required_identity
    if unexpected:
        raise AssertionError(f"unexpected exported symbols: {sorted(unexpected)}")

    collision_symbols = {
        "gst_nvvideoconvert_get_type",
        "gst_plugin_nvvideoconvert_get_desc",
        "gst_plugin_nvvideoconvert_register",
        "ScaleArrowParams",
        "ScaleCircleParams",
        "ScaleLineParams",
        "ScaleRectParams",
        "ScaleTextParams",
        "find_peer_element",
        "inspect_caps",
        "remove_format_from_caps",
    }
    collisions = collision_symbols & defined
    if collisions:
        raise AssertionError(f"ELF symbols collide with the installed plugin: {sorted(collisions)}")

    undefined = dynamic_symbols(REPLACEMENT, False)
    required_backend = {
        "NvBufSurfTransform",
        "NvBufSurfTransformSetSessionParams",
        "NvBufSurfaceCreate",
        "NvBufSurfaceDestroy",
        "gst_nvds_buffer_pool_new",
    }
    missing_backend = required_backend - undefined
    if missing_backend:
        raise AssertionError(f"expected public backend imports are absent: {sorted(missing_backend)}")

    linkage = run("ldd", str(REPLACEMENT))
    if "not found" in linkage:
        raise AssertionError(f"replacement has unresolved shared libraries:\n{linkage}")

    print("PASS: caps, properties, namespaced ABI, and runtime linkage")


if __name__ == "__main__":
    main()
