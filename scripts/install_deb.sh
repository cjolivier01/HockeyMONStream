#!/bin/bash
# Install the local DeepStream and HMStream Debian artifacts on a clean Ubuntu
# host, including the NVIDIA repositories that provide their CUDA/TensorRT
# dependencies.
set -euo pipefail

HMSTREAM_DEB=""
DEEPSTREAM_DEB=""
EXPECTED_DEEPSTREAM_VERSION="9.1.0-1+resolute2"
SIMULATE=0

usage() {
  cat <<'USAGE'
Usage:
  sudo ./install-hmstream-deb \
    --deepstream-deb=/path/to/deepstream-9.1_9.1.0-1+resolute2_amd64.deb \
    --hmstream-deb=/path/to/hmstream_*_amd64.deb

Options:
  --deepstream-deb FILE  Local deepstream-9.1 release artifact.
  --hmstream-deb FILE    Local HMStream artifact for this Ubuntu release.
  --simulate             Configure repositories and only simulate apt install.
  -h, --help             Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deepstream-deb) DEEPSTREAM_DEB="$2"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
    --hmstream-deb) HMSTREAM_DEB="$2"; shift ;;
    --hmstream-deb=*) HMSTREAM_DEB="${1#*=}" ;;
    --simulate) SIMULATE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: run this installer as root (for example, with sudo)." >&2
  exit 1
fi
if [[ -z "${HMSTREAM_DEB}" || -z "${DEEPSTREAM_DEB}" ]]; then
  echo "ERROR: --hmstream-deb and --deepstream-deb are required." >&2
  usage >&2
  exit 1
fi

for deb in "${HMSTREAM_DEB}" "${DEEPSTREAM_DEB}"; do
  if [[ ! -f "${deb}" ]]; then
    echo "ERROR: Debian artifact not found: ${deb}" >&2
    exit 1
  fi
done
HMSTREAM_DEB="$(readlink -f "${HMSTREAM_DEB}")"
DEEPSTREAM_DEB="$(readlink -f "${DEEPSTREAM_DEB}")"
if [[ "$(dpkg-deb -f "${HMSTREAM_DEB}" Package)" != "hmstream" ]]; then
  echo "ERROR: not an hmstream package: ${HMSTREAM_DEB}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Package)" != "deepstream-9.1" ]]; then
  echo "ERROR: not a deepstream-9.1 package: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Version)" != "${EXPECTED_DEEPSTREAM_VERSION}" ]]; then
  echo "ERROR: DeepStream ${EXPECTED_DEEPSTREAM_VERSION} is required: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi

if [[ ! -r /etc/os-release ]]; then
  echo "ERROR: cannot identify the target operating system." >&2
  exit 1
fi
# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]]; then
  echo "ERROR: HMStream Debian artifacts currently support Ubuntu only." >&2
  exit 1
fi
case "${VERSION_ID:-}" in
  24.04) CUDA_REPOSITORY=ubuntu2404 ;;
  26.04) CUDA_REPOSITORY=ubuntu2604 ;;
  *)
    echo "ERROR: unsupported Ubuntu release: ${VERSION_ID:-unknown} (expected 24.04 or 26.04)." >&2
    exit 1
    ;;
esac

HMSTREAM_DEPENDS="$(dpkg-deb -f "${HMSTREAM_DEB}" Depends)"
if [[ "${VERSION_ID}" == "24.04" && "${HMSTREAM_DEPENDS}" == *"libc6 (>= 2.43)"* ]]; then
  echo "ERROR: the selected HMStream artifact targets Ubuntu 26.04, not 24.04." >&2
  exit 1
fi
if [[ "${VERSION_ID}" == "26.04" && "${HMSTREAM_DEPENDS}" != *"libc6 (>= 2.43)"* ]]; then
  echo "ERROR: the selected HMStream artifact does not target Ubuntu 26.04." >&2
  exit 1
fi

HOST_ARCH="$(dpkg --print-architecture)"
for deb in "${HMSTREAM_DEB}" "${DEEPSTREAM_DEB}"; do
  deb_arch="$(dpkg-deb -f "${deb}" Architecture)"
  if [[ "${deb_arch}" != "${HOST_ARCH}" ]]; then
    echo "ERROR: ${deb} targets ${deb_arch}, but this host is ${HOST_ARCH}." >&2
    exit 1
  fi
done

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl

keyring_deb="$(mktemp --suffix=.deb /tmp/hmstream-cuda-keyring.XXXXXX)"
compat_dir=""
cleanup() {
  rm -f "${keyring_deb}"
  if [[ -n "${compat_dir}" ]]; then rm -rf "${compat_dir}"; fi
}
trap cleanup EXIT

curl -fsSLo "${keyring_deb}" \
  "https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_REPOSITORY}/x86_64/cuda-keyring_1.1-1_all.deb"
dpkg -i "${keyring_deb}"

# NVIDIA currently publishes the TensorRT 10 / CUDA 13.2 packages consumed by
# DeepStream 9.1 in its Ubuntu 24.04 repository. Resolute therefore needs that
# compatibility repository in addition to its native CUDA repository.
if [[ "${VERSION_ID}" == "26.04" ]]; then
  curl -fsSLo "${keyring_deb}" \
    "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb"
  compat_dir="$(mktemp -d /tmp/hmstream-cuda-keyring.XXXXXX)"
  dpkg-deb -x "${keyring_deb}" "${compat_dir}"
  install -m 0644 "${compat_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" \
    /usr/share/keyrings/cuda-archive-keyring-ubuntu2404.gpg
  printf '%s\n' \
    'deb [signed-by=/usr/share/keyrings/cuda-archive-keyring-ubuntu2404.gpg] https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
    >/etc/apt/sources.list.d/cuda-ubuntu2404-x86_64.list
fi

apt-get update
trt_version="$(apt-cache madison libnvinfer-dev \
  | awk '$3 ~ /^10[.]/ && $3 ~ /[+]cuda13[.]2$/ && !found { print $3; found = 1 }')"
if [[ -z "${trt_version}" ]]; then
  echo "ERROR: NVIDIA repositories do not provide the TensorRT 10 / CUDA 13.2 dependencies required by DeepStream 9.1." >&2
  exit 1
fi
printf '%s\n' \
  'Package: tensorrt* libnvinfer* libnvonnxparsers*' \
  "Pin: version ${trt_version}" \
  'Pin-Priority: 1001' \
  >/etc/apt/preferences.d/hmstream-tensorrt10
# Older installer revisions pinned NCCL system-wide. HMStream no longer uses
# NCCL, so remove only that obsolete installer-owned pin.
rm -f /etc/apt/preferences.d/hmstream-nccl

apt_args=(-y --no-install-recommends)
if [[ "${SIMULATE}" -eq 1 ]]; then apt_args+=(--simulate); fi
apt-get install "${apt_args[@]}" "${DEEPSTREAM_DEB}" "${HMSTREAM_DEB}"

if [[ "${SIMULATE}" -eq 1 ]]; then
  echo "Dependency resolution succeeded for Ubuntu ${VERSION_ID}."
else
  apt-get check
  echo "Installed DeepStream $(dpkg-query -W -f='${Version}' deepstream-9.1) and HMStream $(dpkg-query -W -f='${Version}' hmstream)."
fi
