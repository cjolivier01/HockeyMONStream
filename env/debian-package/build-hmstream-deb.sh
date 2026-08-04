#!/bin/bash
set -euo pipefail

SOURCE_DIR=/source
BUILD_DIR=/home/colivier/src/hstream
OUTPUT_DIR=/output
CONTAINER_OUTPUT_DIR=/tmp/hmstream-deb-output
DEEPSTREAM_DEB=/inputs/deepstream.deb

: "${PACKAGE_VERSION:?PACKAGE_VERSION is required}"
: "${HOST_UID:?HOST_UID is required}"
: "${HOST_GID:?HOST_GID is required}"
: "${TARGET_UBUNTU:?TARGET_UBUNTU is required}"

case "${TARGET_UBUNTU}" in
  24.04)
    BAZEL_DEB_CONFIG=deb_ubuntu24
    TARGET_CUDA_ROOT=/usr/local/cuda-12.6
    TARGET_CUDA_SONAME=12
    TARGET_CUDA_ARCHES=(sm_70 sm_75 sm_80 sm_86 sm_89 sm_90)
    ;;
  26.04)
    BAZEL_DEB_CONFIG=deb_ubuntu26
    TARGET_CUDA_ROOT=/usr/local/cuda-13.2
    TARGET_CUDA_SONAME=13
    TARGET_CUDA_ARCHES=(sm_75 sm_80 sm_86 sm_89 sm_90 sm_100 sm_120)
    ;;
  *) echo "ERROR: unsupported target Ubuntu release: ${TARGET_UBUNTU}" >&2; exit 1 ;;
esac

apt-get update
apt-get install -y --no-install-recommends "${DEEPSTREAM_DEB}"

# The pinned Bazel repositories are public, so container builds should not
# depend on a developer's SSH agent having a GitHub identity loaded.
git config --global url.https://github.com/.insteadOf ssh://git@github.com/

if ! dpkg-query -W -f='${db:Status-Status} ${Version}\n' deepstream-9.1 | grep -Eq '^installed 9[.]1[.]'; then
  echo "ERROR: the input package did not install DeepStream 9.1." >&2
  exit 1
fi
if ! dpkg-query -W -f='${Version}\n' libnvinfer-dev | grep -Eq '^10[.]'; then
  echo "ERROR: the builder must use TensorRT ABI 10 for DeepStream 9.1." >&2
  exit 1
fi

# Several legacy linkopts still use /usr/local/cuda directly. Select the same
# toolkit as the target-specific Bazel config so compilation and final linkage
# cannot silently mix CUDA major versions.
update-alternatives --set cuda "${TARGET_CUDA_ROOT}"
if [[ "$(readlink -f /usr/local/cuda)" != "${TARGET_CUDA_ROOT}" ]]; then
  echo "ERROR: failed to select target CUDA toolkit: ${TARGET_CUDA_ROOT}" >&2
  exit 1
fi

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
rsync -a \
  --exclude=/.cache \
  --exclude=/.git \
  --exclude=/bazel-* \
  --exclude=/dist \
  --exclude=/dist-staging \
  --exclude=/output_workdirs \
  --exclude=/bazelisk \
  --exclude=/run \
  --exclude=/stitching-calibration-note.txt \
  "${SOURCE_DIR}/" "${BUILD_DIR}/"

cd "${BUILD_DIR}"
make HOST_CUDA_FLAGS="--config=${BAZEL_DEB_CONFIG}" \
  hmstream-cli hmstream-assets hmstream-ui yolo-custom-lib hmstream-gst-plugins

VIDEOPREP_PLUGIN="${BUILD_DIR}/bazel-bin/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so"
CUDA_NEEDED="$(patchelf --print-needed "${VIDEOPREP_PLUGIN}" | grep -E '^lib(cudart|npp[^.]*)[.]so[.]' || true)"
if [[ -z "${CUDA_NEEDED}" ]] || grep -Ev "[.]so[.]${TARGET_CUDA_SONAME}$" <<< "${CUDA_NEEDED}" >/dev/null; then
  echo "ERROR: Ubuntu ${TARGET_UBUNTU} videoprep linked against an unexpected CUDA major:" >&2
  printf '  %s\n' "${CUDA_NEEDED:-(no CUDA libraries found)}" >&2
  exit 1
fi

validate_native_cuda_code() {
  local label="$1"
  local elf="$2"
  local cuda_cubins
  cuda_cubins="$("${TARGET_CUDA_ROOT}/bin/cuobjdump" --list-elf "${elf}")"
  for cuda_arch in "${TARGET_CUDA_ARCHES[@]}"; do
    if ! grep -q "[.]${cuda_arch}[.]cubin$" <<< "${cuda_cubins}"; then
      echo "ERROR: Ubuntu ${TARGET_UBUNTU} ${label} is missing native ${cuda_arch} CUDA code." >&2
      exit 1
    fi
  done
  if "${TARGET_CUDA_ROOT}/bin/cuobjdump" --list-ptx "${elf}" 2>&1 | grep -q '^PTX file'; then
    echo "ERROR: Ubuntu ${TARGET_UBUNTU} ${label} unexpectedly contains PTX." >&2
    exit 1
  fi
}

validate_native_cuda_code videoprep "${VIDEOPREP_PLUGIN}"
validate_native_cuda_code hmstream-cli "${BUILD_DIR}/bazel-bin/src/apps/pipeline-app/hmstream-cli"

rm -rf "${CONTAINER_OUTPUT_DIR}"
mkdir -p "${CONTAINER_OUTPUT_DIR}"
scripts/make_deb.sh --version "${PACKAGE_VERSION}" --output-dir "${CONTAINER_OUTPUT_DIR}"

# Bind-mounted output directories can be root-squashed. Copy the completed
# artifact as the invoking host user instead of relying on container-root
# ownership or chmod changes on the host directory.
setpriv --reuid="${HOST_UID}" --regid="${HOST_GID}" --clear-groups \
  cp -a "${CONTAINER_OUTPUT_DIR}/." "${OUTPUT_DIR}/"
