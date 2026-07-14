#!/bin/bash
set -euo pipefail

if [ "$(uname -m)" != "x86_64" ]; then
  exit 0
fi
if ! command -v nvidia-smi >/dev/null 2>&1 || ! command -v nvcc >/dev/null 2>&1; then
  exit 0
fi

if ! nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | grep -Eq '^(12|12\.)'; then
  exit 0
fi
if ! nvcc --help 2>/dev/null | grep 'sm_120' >/dev/null; then
  exit 0
fi

printf '%s\n' '--config=blackwell'
