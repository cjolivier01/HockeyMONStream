#!/bin/bash
# Build HMStream against an explicit Ubuntu ABI baseline in Docker.
#
# Usage:
#   scripts/make_deb_docker.sh --target-ubuntu=24.04 [--deepstream-deb=FILE]
#   scripts/make_deb_docker.sh --target-ubuntu=26.04 [--deepstream-deb=FILE]
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_UBUNTU=""
DEEPSTREAM_DEB="${DEEPSTREAM_DEB:-}"
HMLIB_SOURCE="${HMLIB_SOURCE:-}"
OUTPUT_DIR=""
PACKAGE_VERSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-ubuntu) TARGET_UBUNTU="$2"; shift ;;
    --target-ubuntu=*) TARGET_UBUNTU="${1#*=}" ;;
    --deepstream-deb) DEEPSTREAM_DEB="$2"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
    --hmlib-source) HMLIB_SOURCE="$2"; shift ;;
    --hmlib-source=*) HMLIB_SOURCE="${1#*=}" ;;
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
    --version) PACKAGE_VERSION="$2"; shift ;;
    --version=*) PACKAGE_VERSION="${1#*=}" ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

case "${TARGET_UBUNTU}" in
  24.04|26.04) ;;
  *) echo "ERROR: --target-ubuntu must be 24.04 or 26.04." >&2; exit 1 ;;
esac

if [[ -z "${DEEPSTREAM_DEB}" ]]; then
  shopt -s nullglob
  candidates=("${TOPDIR}/../DeepStream/artifacts/"deepstream-9.1_*_amd64.deb)
  shopt -u nullglob
  if [[ "${#candidates[@]}" -eq 0 ]]; then
    echo "ERROR: no sibling DeepStream 9.1 artifact found; pass --deepstream-deb=FILE." >&2
    exit 1
  fi
  for candidate in "${candidates[@]}"; do
    candidate_version="$(dpkg-deb -f "${candidate}" Version)"
    if [[ -z "${DEEPSTREAM_DEB}" ]] || dpkg --compare-versions "${candidate_version}" gt "${best_version}"; then
      DEEPSTREAM_DEB="${candidate}"
      best_version="${candidate_version}"
    fi
  done
fi
DEEPSTREAM_DEB="$(readlink -f "${DEEPSTREAM_DEB}")"
if [[ ! -f "${DEEPSTREAM_DEB}" ]]; then
  echo "ERROR: DeepStream package not found: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Package)" != "deepstream-9.1" ]]; then
  echo "ERROR: not a deepstream-9.1 package: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi

if [[ -z "${HMLIB_SOURCE}" ]]; then
  HMLIB_SOURCE="${TOPDIR}/../hm"
fi
HMLIB_SOURCE="$(readlink -f "${HMLIB_SOURCE}")"
if [[ ! -d "${HMLIB_SOURCE}/hmlib" || ! -d "${HMLIB_SOURCE}/xmodels/LightGlue/lightglue" ]]; then
  echo "ERROR: hmlib and its LightGlue submodule are required under: ${HMLIB_SOURCE}" >&2
  exit 1
fi

if [[ -z "${PACKAGE_VERSION}" ]]; then
  commit_count="$(git -C "${TOPDIR}" rev-list --count HEAD)"
  short_hash="$(git -C "${TOPDIR}" rev-parse --short=7 HEAD)"
  PACKAGE_VERSION="0.0.${commit_count}+git.${short_hash}"
fi
if [[ -z "${OUTPUT_DIR}" ]]; then
  OUTPUT_DIR="${TOPDIR}/dist/ubuntu${TARGET_UBUNTU}"
fi
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(readlink -f "${OUTPUT_DIR}")"

image_tag="hmstream-deb-builder:ubuntu${TARGET_UBUNTU}"
volume_suffix="${TARGET_UBUNTU//./}"
cache_volume="hmstream-deb-bazel-ubuntu${volume_suffix}"
cuda_repository="ubuntu${TARGET_UBUNTU//./}"

echo "[make_deb_docker] Building ${image_tag}..."
docker build \
  --build-arg "TARGET_UBUNTU=${TARGET_UBUNTU}" \
  --build-arg "CUDA_REPOSITORY=${cuda_repository}" \
  --tag "${image_tag}" \
  "${TOPDIR}/env/debian-package"
docker volume create "${cache_volume}" >/dev/null

docker_args=(
  --rm
  --volume "${TOPDIR}:/source:ro"
  --volume "${DEEPSTREAM_DEB}:/inputs/deepstream.deb:ro"
  --volume "${HMLIB_SOURCE}:/hmlib-source:ro"
  --volume "${OUTPUT_DIR}:/output"
  --volume "${cache_volume}:/root/.cache/bazel"
  --env "PACKAGE_VERSION=${PACKAGE_VERSION}"
  --env "TARGET_UBUNTU=${TARGET_UBUNTU}"
  --env "HOST_UID=$(id -u)"
  --env "HOST_GID=$(id -g)"
)

# Preserve the host path behind the repo's pretrained symlink so declared
# non-engine assets can be staged without copying them into the Docker context.
PRETRAINED_SOURCE="$(readlink -f "${TOPDIR}/pretrained" 2>/dev/null || true)"
if [[ -n "${PRETRAINED_SOURCE}" && -d "${PRETRAINED_SOURCE}" ]]; then
  docker_args+=(--volume "${PRETRAINED_SOURCE}:${PRETRAINED_SOURCE}:ro")
fi

if [[ -n "${SSH_AUTH_SOCK:-}" && -S "${SSH_AUTH_SOCK}" ]]; then
  docker_args+=(--volume "${SSH_AUTH_SOCK}:/run/host-ssh-agent" --env SSH_AUTH_SOCK=/run/host-ssh-agent)
fi
if [[ -f "${HOME}/.ssh/known_hosts" ]]; then
  docker_args+=(--volume "${HOME}/.ssh/known_hosts:/root/.ssh/known_hosts:ro")
fi

echo "[make_deb_docker] Building Ubuntu ${TARGET_UBUNTU} package..."
docker run "${docker_args[@]}" "${image_tag}"
