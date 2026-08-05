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

HMSTREAM_TARGET_UBUNTU="$(dpkg-deb -f "${HMSTREAM_DEB}" X-HMStream-Target-Ubuntu 2>/dev/null || true)"
if [[ "${HMSTREAM_TARGET_UBUNTU}" != "${VERSION_ID}" ]]; then
  echo "ERROR: the selected HMStream artifact targets Ubuntu ${HMSTREAM_TARGET_UBUNTU:-unknown}, not ${VERSION_ID}." >&2
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
apt-get install -y --no-install-recommends binutils ca-certificates curl zstd

keyring_deb="$(mktemp --suffix=.deb /tmp/hmstream-cuda-keyring.XXXXXX)"
compat_dir=""
combined_keyring=""
transition_dir=""
cleanup() {
  rm -f "${keyring_deb}"
  if [[ -n "${compat_dir}" ]]; then rm -rf "${compat_dir}"; fi
  if [[ -n "${combined_keyring}" ]]; then rm -f "${combined_keyring}"; fi
  if [[ -n "${transition_dir}" ]]; then rm -rf "${transition_dir}"; fi
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
  # cuda-keyring uses one package-owned filename for release-specific keys.
  # Installing the Resolute keyring above therefore replaces a Noble key that
  # an existing compatibility source may still reference.  Keep both public
  # keys in that keyring so existing Noble and native Resolute sources remain
  # valid without registering the same URI under conflicting Signed-By paths.
  combined_keyring="$(mktemp /tmp/hmstream-cuda-combined.XXXXXX.gpg)"
  cat /usr/share/keyrings/cuda-archive-keyring.gpg \
    "${compat_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" >"${combined_keyring}"
  install -m 0644 "${combined_keyring}" /usr/share/keyrings/cuda-archive-keyring.gpg

  compat_repository='https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/'
  compat_source_files=()
  if [[ -f /etc/apt/sources.list ]]; then compat_source_files+=(/etc/apt/sources.list); fi
  while IFS= read -r -d '' source_file; do
    compat_source_files+=("${source_file}")
  done < <(find /etc/apt/sources.list.d -maxdepth 1 -type f \( -name '*.list' -o -name '*.sources' \) -print0)
  compat_source_found=0
  for source_file in "${compat_source_files[@]}"; do
    if awk -v uri="${compat_repository}" '
      /^[[:space:]]*#/ { next }
      /^[[:space:]]*deb[[:space:]]/ && index($0, uri) { found = 1 }
      /^[[:space:]]*URIs:[[:space:]]/ && index($0, uri) { found = 1 }
      END { exit found ? 0 : 1 }
    ' "${source_file}"; then
      compat_source_found=1
      break
    fi
  done
  if [[ "${compat_source_found}" -eq 0 ]]; then
    printf '%s\n' \
      'deb [signed-by=/usr/share/keyrings/cuda-archive-keyring.gpg] https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/ /' \
      >/etc/apt/sources.list.d/hmstream-cuda-ubuntu2404-x86_64.list
  fi
fi

apt-get update
trt_runtime_version="$(apt-cache madison libnvinfer10 \
  | awk '$3 ~ /^10[.]/ && $3 ~ /[+]cuda13[.]2$/ && !found { print $3; found = 1 }')"
if [[ -z "${trt_runtime_version}" ]]; then
  echo "ERROR: NVIDIA repositories do not provide the TensorRT 10 / CUDA 13.2 dependencies required by DeepStream 9.1." >&2
  exit 1
fi

# Older HMStream installers pinned every TensorRT package to version 10. That
# needlessly attempted to downgrade an independently installed TensorRT 11 SDK.
# The versioned TensorRT 10 runtime packages required by the two local .debs
# coexist with newer SDK packages and apt resolves them without a global pin.
rm -f /etc/apt/preferences.d/hmstream-tensorrt10

apt_args=(-y --no-install-recommends)
if [[ "${SIMULATE}" -eq 1 ]]; then apt_args+=(--simulate); fi

# NVIDIA's versioned DeepStream artifacts install many of the same absolute
# paths but do not declare Conflicts/Replaces against older versioned releases
# (for example, deepstream-8.0).  APT does not order a package-name removal
# before unpacking a local artifact when dpkg cannot see a declared conflict.
# Add those relationships to a temporary local copy, allowing APT to perform
# one coherent replacement transaction without a standalone removal.
# Keep unrelated split packages out of this list; the 9.1 artifact declares its
# own conflicts with the legacy binaries/sample-data packages.
old_deepstream_packages=()
while IFS=$'\t' read -r package status; do
  package_name="${package%%:*}"
  if [[ "${status:0:1}" == "i" && "${status:1:1}" != "n" &&
        "${package_name}" =~ ^deepstream-[0-9]+([.][0-9]+)*$ &&
        "${package_name}" != "deepstream-9.1" ]]; then
    old_deepstream_packages+=("${package}")
  fi
done < <(dpkg-query -W -f='${binary:Package}\t${db:Status-Abbrev}\n' 'deepstream-*' 2>/dev/null || true)
install_deepstream_deb="${DEEPSTREAM_DEB}"
if [[ "${#old_deepstream_packages[@]}" -gt 0 ]]; then
  echo "Replacing older DeepStream package(s): ${old_deepstream_packages[*]}"
  transition_dir="$(mktemp -d /tmp/hmstream-deepstream-transition.XXXXXX)"
  control_member="$(ar t "${DEEPSTREAM_DEB}" | awk '/^control[.]tar[.]/{print; exit}')"
  data_member="$(ar t "${DEEPSTREAM_DEB}" | awk '/^data[.]tar[.]/{print; exit}')"
  if [[ -z "${control_member}" || -z "${data_member}" ]]; then
    echo "ERROR: malformed DeepStream Debian artifact." >&2
    exit 1
  fi
  case "${control_member}" in
    *.zst) control_compression=(--zstd) ;;
    *.xz) control_compression=(-J) ;;
    *.gz) control_compression=(-z) ;;
    *) echo "ERROR: unsupported DeepStream control archive: ${control_member}" >&2; exit 1 ;;
  esac
  mkdir "${transition_dir}/control"
  ar p "${DEEPSTREAM_DEB}" "${control_member}" | tar "${control_compression[@]}" -xf - -C "${transition_dir}/control"
  transition_relationships=()
  for package in "${old_deepstream_packages[@]}"; do
    transition_relationships+=("${package%%:*}")
  done
  relationship_list="$(IFS=', '; echo "${transition_relationships[*]}")"
  sed -i -E \
    -e "s/^(Conflicts:.*)$/\\1, ${relationship_list}/" \
    -e "s/^(Replaces:.*)$/\\1, ${relationship_list}/" \
    "${transition_dir}/control/control"
  tar "${control_compression[@]}" -cf "${transition_dir}/${control_member}" -C "${transition_dir}/control" .
  install_deepstream_deb="${transition_dir}/deepstream-9.1-transition.deb"
  printf '!<arch>\n' >"${install_deepstream_deb}"
  append_ar_member() {
    local name="$1"
    local size="$2"
    printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' "${name}/" 0 0 0 100644 "${size}" >>"${install_deepstream_deb}"
  }
  for member in debian-binary "${control_member}" "${data_member}"; do
    if [[ "${member}" == "${control_member}" ]]; then
      member_size="$(stat -c '%s' "${transition_dir}/${control_member}")"
      append_ar_member "${member}" "${member_size}"
      cat "${transition_dir}/${control_member}" >>"${install_deepstream_deb}"
    else
      member_size="$(ar tv "${DEEPSTREAM_DEB}" | awk -v member="${member}" '$NF == member {print $3; exit}')"
      append_ar_member "${member}" "${member_size}"
      ar p "${DEEPSTREAM_DEB}" "${member}" >>"${install_deepstream_deb}"
    fi
    if (( member_size % 2 != 0 )); then printf '\n' >>"${install_deepstream_deb}"; fi
  done
  dpkg-deb --info "${install_deepstream_deb}" >/dev/null
  for relationship in Conflicts Replaces; do
    metadata="$(dpkg-deb -f "${install_deepstream_deb}" "${relationship}")"
    for package in "${transition_relationships[@]}"; do
      if [[ ",${metadata// /}," != *",${package},"* ]]; then
        echo "ERROR: failed to add ${relationship}: ${package} to the DeepStream transition artifact." >&2
        exit 1
      fi
    done
  done
fi

simulation="$(apt-get install --simulate --no-install-recommends "${install_deepstream_deb}" "${HMSTREAM_DEB}")"
printf '%s\n' "${simulation}"
while read -r removed_package; do
  [[ -z "${removed_package}" ]] && continue
  allowed=0
  for package in "${old_deepstream_packages[@]}"; do
    if [[ "${removed_package}" == "${package%%:*}" ]]; then allowed=1; break; fi
  done
  if [[ "${allowed}" -eq 0 ]]; then
    echo "ERROR: DeepStream replacement would remove dependent package ${removed_package}; refusing." >&2
    exit 1
  fi
done < <(awk '$1 == "Remv" {print $2}' <<<"${simulation}")

if [[ "${SIMULATE}" -eq 0 ]]; then
  apt-get install "${apt_args[@]}" "${install_deepstream_deb}" "${HMSTREAM_DEB}"
fi

if [[ "${SIMULATE}" -eq 1 ]]; then
  echo "Dependency resolution succeeded for Ubuntu ${VERSION_ID}."
else
  # A short-lived older HMStream installer revision created this exact
  # system-wide pin. The current package neither depends on nor changes NCCL;
  # remove only HMStream's obsolete policy file after a successful install.
  rm -f /etc/apt/preferences.d/hmstream-nccl
  apt-get check
  echo "Installed DeepStream $(dpkg-query -W -f='${Version}' deepstream-9.1) and HMStream $(dpkg-query -W -f='${Version}' hmstream)."
fi
