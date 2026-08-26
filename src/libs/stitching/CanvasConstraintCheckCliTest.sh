#!/usr/bin/env bash
set -euo pipefail

checker=$1
root=$(mktemp -d)
trap 'rm -rf "${root}"' EXIT

if "${checker}" "${root}" -1 >"${root}/negative.out" 2>&1; then
  echo "negative canvas width was accepted" >&2
  exit 1
fi

output=$("${checker}" "${root}" 0)
expected='HSTREAM_STITCHING_CANVAS_CHECK artifacts-compatible=0 requires-regeneration=0 lock-held=1'
if [[ "${output}" != "${expected}" ]]; then
  echo "unexpected empty-canvas result: ${output}" >&2
  exit 1
fi

coproc holder { "${checker}" "${root}" 0 --hold-lock; }
read -r held_output <&"${holder[0]}"
if [[ "${held_output}" != "${expected}" ]]; then
  echo "checker did not retain the artifact lock: ${held_output}" >&2
  exit 1
fi
contended=$("${checker}" "${root}" 0)
contended_expected='HSTREAM_STITCHING_CANVAS_CHECK artifacts-compatible=0 requires-regeneration=1 lock-held=0'
if [[ "${contended}" != "${contended_expected}" ]]; then
  echo "contended checker did not fail closed: ${contended}" >&2
  exit 1
fi
exec {holder[1]}>&-
wait "${holder_PID}"
