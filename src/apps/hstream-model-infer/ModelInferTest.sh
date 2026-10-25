#!/usr/bin/env bash
set -euo pipefail
: "${HSTREAM_REID_ENGINE:?Set HSTREAM_REID_ENGINE to an already prepared native ReID engine}"
exec python3 "$(dirname "$0")/ModelInferTest.py" "$1" "${HSTREAM_REID_ENGINE}"
