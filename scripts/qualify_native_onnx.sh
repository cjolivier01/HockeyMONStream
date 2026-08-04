#!/bin/bash
# Non-skippable native-model and deterministic Python parity release gate.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${TOPDIR}"

export HM_REQUIRE_ONNX_MODEL_TESTS=1
export HM_REQUIRE_ONNX_PARITY=1

bazel_args=(test --config=opt --test_output=errors)
host_cuda_flags="$(scripts/bazel_cuda_host_config.sh 2>/dev/null || true)"
if [[ -n "${host_cuda_flags}" ]]; then
  read -r -a parsed_cuda_flags <<< "${host_cuda_flags}"
  bazel_args+=("${parsed_cuda_flags[@]}")
fi
bazel_args+=(
  //src/libs/stitching:native_model_smoke_test
  //src/libs/stitching:python_parity_test
)

exec bazelisk "${bazel_args[@]}"
