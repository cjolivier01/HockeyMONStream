#!/usr/bin/env bash
set -euo pipefail

inspector=$1
mode=$2
elf=$3

if listing="$("${inspector}" "${mode}" "${elf}" 2>&1)"; then
  # PTX-only binaries have no ELF payload. cuobjdump reports that on stderr
  # despite succeeding. CUDA 13 likewise reports host-only binaries as an
  # informational successful invocation. Keep both out of the architecture
  # listing so a host-only executable is not mistaken for a native cubin.
  printf '%s\n' "${listing}" |
    sed -E '/^cuobjdump info[[:space:]]*: (No (ELF|PTX) file found to extract|File .* does not contain device code)/d'
else
  status=$?
  # Host-only utilities/plugins are valid package members. CUDA 13 reports
  # this as fatal/255 for some listing modes, unlike the successful empty
  # listing above. Missing or damaged files and other inspector errors must
  # still fail packaging.
  if [[ ${listing} == *'does not contain device code'* ]]; then
    exit 0
  fi
  printf '%s\n' "${listing}" >&2
  exit "${status}"
fi
