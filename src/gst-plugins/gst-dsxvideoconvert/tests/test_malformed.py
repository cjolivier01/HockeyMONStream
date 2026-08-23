#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Ensure malformed NVMM descriptors fail without a crash or hang."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


def run_case(registry: Path, size: int, diagnostic: str) -> None:
    command = [
        "gst-launch-1.0", "-q",
        "fakesrc", "sizetype=fixed", f"sizemax={size}", "filltype=zero",
        "num-buffers=1", "!",
        "video/x-raw(memory:NVMM),format=RGBA,width=64,height=48,"
        "framerate=1/1,batch-size=(int)1", "!",
        "dsxvideoconvert", "!",
        "video/x-raw,format=RGBA,width=32,height=24", "!", "fakesink",
    ]
    environment = os.environ.copy()
    environment["GST_REGISTRY"] = str(registry)
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=10,
        )
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"malformed {size}-byte NVMM buffer hung") from error
    stderr = result.stderr.decode(errors="replace")
    if result.returncode != 1:
        raise AssertionError(
            f"malformed {size}-byte NVMM buffer returned {result.returncode}\n{stderr}"
        )
    if "video conversion failed" not in stderr or diagnostic not in stderr:
        raise AssertionError(
            f"malformed {size}-byte NVMM buffer lacked a useful diagnostic\n{stderr}"
        )


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="dsx-malformed-") as temporary:
        registry = Path(temporary) / "registry.bin"
        run_case(registry, 1, "need at least")
        run_case(registry, 4096, "has no surface list")
    print("PASS: malformed NVMM descriptors fail safely")


if __name__ == "__main__":
    main()
