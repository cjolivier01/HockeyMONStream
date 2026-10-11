#!/usr/bin/env bash
# Prepare a persistent DeepStream amd64 package without building HStream.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/deepstream_artifact.sh
source "${TOPDIR}/scripts/deepstream_artifact.sh"
TARGET_UBUNTU=""
DEEPSTREAM_DEB="${DEEPSTREAM_DEB:-}"
OUTPUT_DIR=""

usage() {
  echo "Usage: $0 --target-ubuntu=24.04|26.04 [--deepstream-deb=FILE] [--output-dir=DIR]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-ubuntu) TARGET_UBUNTU="${2:?Missing Ubuntu release}"; shift ;;
    --target-ubuntu=*) TARGET_UBUNTU="${1#*=}" ;;
    --deepstream-deb) DEEPSTREAM_DEB="${2:?Missing DeepStream package}"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
    --output-dir) OUTPUT_DIR="${2:?Missing output directory}"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

case "${TARGET_UBUNTU}" in
  24.04|26.04) ;;
  *) echo "ERROR: --target-ubuntu must be 24.04 or 26.04." >&2; exit 1 ;;
esac

OUTPUT_DIR="${OUTPUT_DIR:-${TOPDIR}/dist/ubuntu${TARGET_UBUNTU}}"
deepstream_artifact_resolve make_deepstream_deb "${DEEPSTREAM_DEB}" \
  "make deepstream-deb TARGET_UBUNTU=${TARGET_UBUNTU} DEEPSTREAM_DEB=/path/to/deepstream.deb" \
  "${OUTPUT_DIR}"
version="$(dpkg-deb -f "${DEEPSTREAM_ARTIFACT}" Version)"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(readlink -f "${OUTPUT_DIR}")"
# Keep target-specific outputs outside the resolver's original-download glob.
output_deb="${OUTPUT_DIR}/deepstream-9.1_${version}_amd64.ubuntu${TARGET_UBUNTU}.deb"
if [[ "${output_deb}" -ef "${DEEPSTREAM_ARTIFACT}" ]]; then
  echo "ERROR: output would overwrite the input package: ${output_deb}" >&2
  exit 1
fi

# Publish only a complete package, retaining any previous output on failure.
stage_dir="$(mktemp -d "${OUTPUT_DIR}/.deepstream-deb.XXXXXX")"
trap 'rm -rf -- "${stage_dir}"' EXIT
if [[ "${TARGET_UBUNTU}" == "26.04" ]]; then
  "${TOPDIR}/scripts/remove_deb_dependencies.py" \
    --output "${stage_dir}/deepstream.deb" "${DEEPSTREAM_ARTIFACT}"
else
  cp --reflink=auto "${DEEPSTREAM_ARTIFACT}" "${stage_dir}/deepstream.deb"
fi
mv -fT "${stage_dir}/deepstream.deb" "${output_deb}"

printf '\nDeepStream package: %s\n' "${output_deb}"
printf 'Install on Ubuntu %s with:\n  sudo apt-get install %q\n' "${TARGET_UBUNTU}" "${output_deb}"
