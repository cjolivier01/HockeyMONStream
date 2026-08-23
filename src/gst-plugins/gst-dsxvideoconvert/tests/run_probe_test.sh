#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

mode="$1"
replacement="$(readlink -f "$2")"
original="$(readlink -f "$3")"
probe="$(readlink -f "$4")"
deepstream_root="$(dirname "$(dirname "$(dirname "${original}")")")"
results="${TEST_TMPDIR:-$(mktemp -d)}"

export GST_PLUGIN_PATH="$(dirname "${replacement}"):$(dirname "${original}")${GST_PLUGIN_PATH:+:${GST_PLUGIN_PATH}}"
export LD_LIBRARY_PATH="${deepstream_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

case "${mode}" in
  metadata)
    for method in 0 1 2 3 4 5 6 7; do
      timeout 15s "${probe}" nvvideoconvert "${method}" >"${results}/original-${method}.txt"
      timeout 15s "${probe}" dsxvideoconvert "${method}" >"${results}/replacement-${method}.txt"
      diff -u "${results}/original-${method}.txt" "${results}/replacement-${method}.txt"
    done
    echo "PASS: DeepStream metadata propagation and geometry parity"
    ;;
  batch)
    timeout 15s "${probe}" nvvideoconvert nvmm >"${results}/original-nvmm.txt"
    timeout 15s "${probe}" dsxvideoconvert nvmm >"${results}/replacement-nvmm.txt"
    diff -u "${results}/original-nvmm.txt" "${results}/replacement-nvmm.txt"
    timeout 15s "${probe}" nvvideoconvert raw >"${results}/original-raw.txt"
    grep -qx 'raw-batch-error' "${results}/original-raw.txt"
    timeout 15s "${probe}" dsxvideoconvert raw >"${results}/replacement-raw.txt"
    timeout 15s "${probe}" dsxvideoconvert raw caps-only >"${results}/replacement-caps-only.txt"
    diff -u "${results}/replacement-raw.txt" "${results}/replacement-caps-only.txt"
    sed -n 's/.*checksum=//p' "${results}/original-nvmm.txt" >"${results}/original-checksums.txt"
    sed -n 's/.*checksum=//p' "${results}/replacement-raw.txt" >"${results}/replacement-checksums.txt"
    diff -u "${results}/original-checksums.txt" "${results}/replacement-checksums.txt"
    echo "PASS: two-frame NVMM and RAW batch coverage"
    ;;
  stride)
    timeout 15s "${probe}" nvvideoconvert compact >"${results}/original-compact.txt"
    timeout 15s "${probe}" dsxvideoconvert compact >"${results}/replacement-compact.txt"
    for layout in padded custom; do
      timeout 15s "${probe}" dsxvideoconvert "${layout}" >"${results}/replacement-${layout}.txt"
      diff -u "${results}/original-compact.txt" "${results}/replacement-${layout}.txt"
    done
    diff -u "${results}/original-compact.txt" "${results}/replacement-compact.txt"
    for sign in positive negative; do
      timeout 15s "${probe}" dsxvideoconvert "undersized-${sign}" >"${results}/undersized-${sign}.txt"
      grep -qx rejected "${results}/undersized-${sign}.txt"
    done
    echo "PASS: compact, padded/custom GstVideoMeta, stale-meta filtering, and stride rejection"
    ;;
  controls)
    pool_probe="$(readlink -f "$5")"
    for option in contiguous block-linear compute-gpu copy-gpu; do
      timeout 15s "${probe}" nvvideoconvert nvmm "${option}" >"${results}/original-${option}.txt"
      timeout 15s "${probe}" dsxvideoconvert nvmm "${option}" >"${results}/replacement-${option}.txt"
      diff -u "${results}/original-${option}.txt" "${results}/replacement-${option}.txt"
    done
    grep -q '^batch=2,filled=2,.*contiguous=1$' "${results}/replacement-contiguous.txt"
    timeout 15s "${pool_probe}" nvvideoconvert >"${results}/original-pool.txt"
    timeout 15s "${pool_probe}" dsxvideoconvert >"${results}/replacement-pool.txt"
    diff -u "${results}/original-pool.txt" "${results}/replacement-pool.txt"
    echo "PASS: allocation and hardware-control behavior"
    ;;
  *)
    echo "unknown probe mode: ${mode}" >&2
    exit 2
    ;;
esac
