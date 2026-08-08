#!/bin/bash
# Non-skippable native-model and deterministic Python parity release gate.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${TOPDIR}"

export HM_REQUIRE_ONNX_MODEL_TESTS=1
export HM_REQUIRE_ONNX_PARITY=1

for tool in pto_gen autooptimiser nona pano_modify realpath sha256sum; do
  if ! command -v "${tool}" >/dev/null; then
    echo "Required qualification tool is unavailable on PATH: ${tool}" >&2
    exit 2
  fi
done

if [[ -z "${HM_PARITY_PYTHON:-}" ]]; then
  echo "HM_PARITY_PYTHON must name the pinned HockeyMOM Python executable." >&2
  exit 2
fi
if [[ ! -x "${HM_PARITY_PYTHON}" ]]; then
  echo "HM_PARITY_PYTHON is not executable: ${HM_PARITY_PYTHON}" >&2
  exit 2
fi

hockeymom_root="$("${HM_PARITY_PYTHON}" -c \
  'from pathlib import Path; import hmlib; print(Path(hmlib.__file__).resolve().parent.parent)')"
if [[ "$(git -C "${hockeymom_root}" rev-parse --is-inside-work-tree 2>/dev/null || true)" != "true" ]]; then
  echo "HM_PARITY_PYTHON does not import hmlib from a HockeyMOM Git checkout: ${hockeymom_root}" >&2
  exit 2
fi
expected_hockeymom_revision="$(tr -d '[:space:]' < "${TOPDIR}/scripts/hmlib-runtime-revision")"
actual_hockeymom_revision="$(git -C "${hockeymom_root}" rev-parse HEAD)"
if [[ "${actual_hockeymom_revision}" != "${expected_hockeymom_revision}" ]]; then
  echo "HockeyMOM oracle revision mismatch: expected ${expected_hockeymom_revision}, got ${actual_hockeymom_revision}" >&2
  exit 2
fi
if [[ -n "$(git -C "${hockeymom_root}" status --porcelain --untracked-files=all -- \
    hmlib/config.py hmlib/models hmlib/segm hmlib/stitching)" ]]; then
  echo "HockeyMOM oracle sources contain uncommitted changes under ${hockeymom_root}" >&2
  exit 2
fi
oracle_revision_manifest="${TOPDIR}/scripts/hockeymom-oracle-revisions"
while read -r expected_revision relative_checkout; do
  [[ -z "${expected_revision}" || "${expected_revision}" == \#* ]] && continue
  checkout="${hockeymom_root}/${relative_checkout}"
  if [[ "$(git -C "${checkout}" rev-parse --is-inside-work-tree 2>/dev/null || true)" != "true" ]]; then
    echo "Pinned HockeyMOM oracle checkout is missing: ${checkout}" >&2
    exit 2
  fi
  actual_revision="$(git -C "${checkout}" rev-parse HEAD)"
  if [[ "${actual_revision}" != "${expected_revision}" ]]; then
    echo "HockeyMOM oracle checkout mismatch for ${relative_checkout}: expected ${expected_revision}, got ${actual_revision}" >&2
    exit 2
  fi
  if [[ -n "$(git -C "${checkout}" status --porcelain --untracked-files=all)" ]]; then
    echo "HockeyMOM oracle checkout contains uncommitted tracked changes: ${checkout}" >&2
    exit 2
  fi
done < "${oracle_revision_manifest}"

if [[ ! -f "${HM_PARITY_RINK_CONFIG:-}" ]]; then
  echo "HM_PARITY_RINK_CONFIG must name the pinned HockeyMOM rink config." >&2
  exit 2
fi
if [[ ! -f "${HM_PARITY_RINK_CHECKPOINT:-}" ]]; then
  echo "HM_PARITY_RINK_CHECKPOINT must name the pinned HockeyMOM rink checkpoint." >&2
  exit 2
fi
model_manifest="${HM_ONNX_PARITY_MODEL_MANIFEST:-${TOPDIR}/src/libs/stitching/testdata/native-onnx-python-models.sha256}"
if [[ ! -f "${model_manifest}" ]]; then
  echo "Native ONNX Python model manifest is missing: ${model_manifest}" >&2
  exit 2
fi
rink_model="${HM_RINK_ONNX_MODEL:-${HOME}/.cache/hstream/models/ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx}"
matcher_model="${HM_FEATURE_MATCHER_ONNX_MODEL:-${HOME}/.cache/hstream/models/aliked-lightglue-k2048-ea4a4ab2cb556958.onnx}"
for model_entry in \
    "rink-config:${HM_PARITY_RINK_CONFIG}" \
    "rink-checkpoint:${HM_PARITY_RINK_CHECKPOINT}" \
    "native-rink-onnx:${rink_model}" \
    "native-feature-matcher-onnx:${matcher_model}"; do
  model_name="${model_entry%%:*}"
  model_path="${model_entry#*:}"
  if [[ ! -f "${model_path}" ]]; then
    echo "Pinned model is missing for ${model_name}: ${model_path}" >&2
    exit 2
  fi
  expected_hash="$(awk -v name="${model_name}" '$2 == name { print $1 }' "${model_manifest}")"
  actual_hash="$(sha256sum "${model_path}")"
  actual_hash="${actual_hash%% *}"
  if [[ -z "${expected_hash}" || "${actual_hash}" != "${expected_hash}" ]]; then
    echo "Pinned Python model checksum mismatch for ${model_name}: ${model_path}" >&2
    exit 2
  fi
done
export HM_RINK_ONNX_MODEL
HM_RINK_ONNX_MODEL="$(realpath "${rink_model}")"
export HM_FEATURE_MATCHER_ONNX_MODEL
HM_FEATURE_MATCHER_ONNX_MODEL="$(realpath "${matcher_model}")"
if [[ -z "${HM_ONNX_PARITY_GAME_DIRS:-}" ]]; then
  echo "HM_ONNX_PARITY_GAME_DIRS must contain at least two colon-separated fixture directories." >&2
  exit 2
fi

fixture_manifest="${HM_ONNX_PARITY_FIXTURE_MANIFEST:-${TOPDIR}/src/libs/stitching/testdata/native-onnx-fixtures.sha256}"
if [[ ! -f "${fixture_manifest}" ]]; then
  echo "Native ONNX fixture manifest is missing: ${fixture_manifest}" >&2
  exit 2
fi
IFS=: read -r -a fixture_dirs <<< "${HM_ONNX_PARITY_GAME_DIRS}"
if (( ${#fixture_dirs[@]} < 2 )); then
  echo "Release qualification requires at least two pinned fixture directories." >&2
  exit 2
fi

declare -A seen_fixture_paths=()
declare -A seen_fixture_names=()
for fixture_dir in "${fixture_dirs[@]}"; do
  fixture_dir="$(realpath "${fixture_dir}")"
  fixture_name="$(basename "${fixture_dir}")"
  if [[ -n "${seen_fixture_paths[${fixture_dir}]:-}" ]]; then
    echo "Release qualification fixture path is duplicated: ${fixture_dir}" >&2
    exit 2
  fi
  if [[ -n "${seen_fixture_names[${fixture_name}]:-}" ]]; then
    echo "Release qualification fixture identity is duplicated: ${fixture_name}" >&2
    exit 2
  fi
  seen_fixture_paths["${fixture_dir}"]=1
  seen_fixture_names["${fixture_name}"]=1
  for fixture_file in left.png right.png s.png; do
    fixture_path="${fixture_dir}/${fixture_file}"
    if [[ ! -f "${fixture_path}" ]]; then
      echo "Pinned fixture is missing ${fixture_path}" >&2
      exit 2
    fi
    expected_hash="$(awk -v path="${fixture_name}/${fixture_file}" '$2 == path { print $1 }' "${fixture_manifest}")"
    if [[ -z "${expected_hash}" ]]; then
      echo "No pinned checksum for ${fixture_name}/${fixture_file} in ${fixture_manifest}" >&2
      exit 2
    fi
    actual_hash="$(sha256sum "${fixture_path}")"
    actual_hash="${actual_hash%% *}"
    if [[ "${actual_hash}" != "${expected_hash}" ]]; then
      echo "Fixture checksum mismatch for ${fixture_name}/${fixture_file}" >&2
      exit 2
    fi
  done
done

bazel_args=(test --config=opt --test_output=errors --nocache_test_results)
host_cuda_flags="$(scripts/bazel_cuda_host_config.sh 2>/dev/null || true)"
if [[ -n "${host_cuda_flags}" ]]; then
  read -r -a parsed_cuda_flags <<< "${host_cuda_flags}"
  bazel_args+=("${parsed_cuda_flags[@]}")
fi
bazelisk "${bazel_args[@]}" //src/libs/stitching:native_model_smoke_test

for fixture_dir in "${fixture_dirs[@]}"; do
  export HM_ONNX_PARITY_GAME_DIR
  HM_ONNX_PARITY_GAME_DIR="$(realpath "${fixture_dir}")"
  bazelisk "${bazel_args[@]}" //src/libs/stitching:python_parity_test
done
