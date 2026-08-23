#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

benchmark_script="$(readlink -f "$1")"
replacement="$(readlink -f "$2")"
original="$(readlink -f "$3")"
batch_benchmark="$(readlink -f "$4")"
deepstream_root="$(dirname "$(dirname "$(dirname "${original}")")")"
shift 4

export GST_PLUGIN_PATH="$(dirname "${replacement}"):$(dirname "${original}")${GST_PLUGIN_PATH:+:${GST_PLUGIN_PATH}}"
export LD_LIBRARY_PATH="${deepstream_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export NVVIDEO_PLUGIN_SO="${original}"
export DSX_BATCH_BENCH="${batch_benchmark}"

exec python3 "${benchmark_script}" "$@"
