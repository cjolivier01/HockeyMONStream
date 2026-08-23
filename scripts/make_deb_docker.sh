#!/bin/bash
# Build HStream against an explicit Ubuntu ABI baseline in Docker.
#
# Usage:
#   scripts/make_deb_docker.sh --target-ubuntu=24.04 [--deepstream-deb=FILE]
#   scripts/make_deb_docker.sh --target-ubuntu=26.04 [--deepstream-deb=FILE]
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_UBUNTU=""
DEEPSTREAM_DEB="${DEEPSTREAM_DEB:-}"
OUTPUT_DIR=""
PACKAGE_VERSION=""
DEEPSTREAM_MIN_VERSION="9.1.0-1"
DEEPSTREAM_MAX_VERSION="9.2~"

deepstream_version_supported() {
  local version="$1"
  dpkg --compare-versions "${version}" ge "${DEEPSTREAM_MIN_VERSION}" &&
    dpkg --compare-versions "${version}" lt "${DEEPSTREAM_MAX_VERSION}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target-ubuntu) TARGET_UBUNTU="$2"; shift ;;
    --target-ubuntu=*) TARGET_UBUNTU="${1#*=}" ;;
    --deepstream-deb) DEEPSTREAM_DEB="$2"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
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
    echo "When using make, pass DEEPSTREAM_DEB=FILE instead." >&2
    exit 1
  fi
  for candidate in "${candidates[@]}"; do
    candidate_version="$(dpkg-deb -f "${candidate}" Version)"
    if deepstream_version_supported "${candidate_version}"; then
      DEEPSTREAM_DEB="${candidate}"
      break
    fi
  done
  if [[ -z "${DEEPSTREAM_DEB}" ]]; then
    echo "ERROR: sibling DeepStream artifact version >= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION} is required." >&2
    exit 1
  fi
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
deepstream_version="$(dpkg-deb -f "${DEEPSTREAM_DEB}" Version)"
if ! deepstream_version_supported "${deepstream_version}"; then
  echo "ERROR: DeepStream >= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION} is required: ${DEEPSTREAM_DEB}" >&2
  echo "Found version: ${deepstream_version}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Architecture)" != "amd64" ]]; then
  echo "ERROR: the target-OS package builder currently supports amd64 DeepStream artifacts only." >&2
  exit 1
fi

if ! git -C "${TOPDIR}" diff --quiet HEAD --; then
  echo "ERROR: refusing to label a package with Git HEAD while tracked source changes are present." >&2
  echo "Commit or stash the tracked changes, then rebuild." >&2
  exit 1
fi
unexpected_untracked="$(
  git -C "${TOPDIR}" ls-files --others --exclude-standard \
    | grep -Ev '^(bazelisk|run|stitching-calibration-note[.]txt|dist/|dist-staging/|output_workdirs/|bazel-[^/]+(/|$))' \
    || true
)"
if [[ -n "${unexpected_untracked}" ]]; then
  echo "ERROR: refusing to package untracked source files:" >&2
  while IFS= read -r source_path; do
    printf '  %s\n' "${source_path}" >&2
  done <<< "${unexpected_untracked}"
  exit 1
fi

# Resolve the immutable source identity once. Every version field, archive,
# and provenance record below refers to this object even if another process
# moves the worktree or branch while Docker is building.
SOURCE_REVISION="$(git -C "${TOPDIR}" rev-parse HEAD)"

if [[ -z "${PACKAGE_VERSION}" ]]; then
  commit_epoch="$(git -C "${TOPDIR}" show -s --format=%ct "${SOURCE_REVISION}")"
  short_hash="$(git -C "${TOPDIR}" rev-parse --short=7 "${SOURCE_REVISION}")"
  PACKAGE_VERSION="0.0.${commit_epoch}+git.${short_hash}"
fi
if [[ -z "${OUTPUT_DIR}" ]]; then
  OUTPUT_DIR="${TOPDIR}/dist/ubuntu${TARGET_UBUNTU}"
fi
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(readlink -f "${OUTPUT_DIR}")"

# Freeze the complete build input before the slow image build. The container
# never sees the mutable checkout, ignored artifacts, or untracked files.
SOURCE_SNAPSHOT="$(mktemp -d "${TMPDIR:-/tmp}/hstream-deb-source.XXXXXX")"
cleanup_snapshot() {
  rm -rf -- "${SOURCE_SNAPSHOT}"
}
trap cleanup_snapshot EXIT
git -C "${TOPDIR}" archive --format=tar "${SOURCE_REVISION}" | tar -xf - -C "${SOURCE_SNAPSHOT}"
source_epoch="$(git -C "${TOPDIR}" show -s --format=%ct "${SOURCE_REVISION}")"
printf '%s %s\n' "${SOURCE_REVISION}" "${source_epoch}" > "${SOURCE_SNAPSHOT}/.hstream-package-source"

image_tag="hstream-deb-builder:ubuntu${TARGET_UBUNTU}"
volume_suffix="${TARGET_UBUNTU//./}"
cache_volume="hstream-deb-bazel-ubuntu${volume_suffix}"
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
  --volume "${SOURCE_SNAPSHOT}:/source:ro"
  --volume "${DEEPSTREAM_DEB}:/inputs/deepstream.deb:ro"
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

# Native calibration models intentionally live in a content-addressed user
# cache for source-tree runs.  Expose that cache read-only to the immutable
# package build; make_deb.sh verifies every declared digest before and after
# copying the models into the package-owned pretrained tree.
MODEL_CACHE_SOURCE="${HSTREAM_MODEL_CACHE_DIR:-${HOME}/.cache/hstream/models}"
if [[ -d "${MODEL_CACHE_SOURCE}" ]]; then
  MODEL_CACHE_SOURCE="$(readlink -f "${MODEL_CACHE_SOURCE}")"
  docker_args+=(--volume "${MODEL_CACHE_SOURCE}:/root/.cache/hstream/models:ro")
fi

if [[ -n "${SSH_AUTH_SOCK:-}" && -S "${SSH_AUTH_SOCK}" ]]; then
  docker_args+=(--volume "${SSH_AUTH_SOCK}:/run/host-ssh-agent" --env SSH_AUTH_SOCK=/run/host-ssh-agent)
fi
if [[ -f "${HOME}/.ssh/known_hosts" ]]; then
  docker_args+=(--volume "${HOME}/.ssh/known_hosts:/root/.ssh/known_hosts:ro")
fi

echo "[make_deb_docker] Building Ubuntu ${TARGET_UBUNTU} package..."
docker run "${docker_args[@]}" "${image_tag}"

package_filename_version="${PACKAGE_VERSION#v}"
package_filename_version="$(printf '%s' "${package_filename_version}" | sed -E 's/[^A-Za-z0-9.+:~-]+/./g; s/[.]+/./g; s/^[.]+//; s/[.]+$//')"
if [[ ! "${package_filename_version}" =~ ^[0-9] ]]; then
  package_filename_version="0.0+git.${package_filename_version}"
fi
host_deb_path="${OUTPUT_DIR}/hstream_${package_filename_version}_amd64.deb"
host_installer_path="${OUTPUT_DIR}/install-hstream-deb"
if [[ ! -f "${host_deb_path}" || ! -x "${host_installer_path}" ]]; then
  echo "ERROR: Docker completed without exporting the expected host artifacts to ${OUTPUT_DIR}." >&2
  exit 1
fi

echo ""
echo "Done: ${host_deb_path}"
echo "Package output directory: ${OUTPUT_DIR}"
echo ""
echo "Install with:"
printf '  sudo %q \\\n' "${host_installer_path}"
printf '    --deepstream-deb=%q \\\n' "${DEEPSTREAM_DEB}"
printf '    --hstream-deb=%q\n' "${host_deb_path}"
