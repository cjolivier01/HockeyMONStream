#!/bin/bash
# Install the HStream host build dependencies on Ubuntu 24.04 and 26.04.
# env/debian-package/Dockerfile does the equivalent for the container builds.
set -uo pipefail

APT_PACKAGES=(
  curl
  fonts-dejavu-core
  libva-dev
  libsoup2.4-dev
  libjson-glib-dev
  libmosquitto-dev
  libjsoncpp-dev
  libglfw3-dev
  protobuf-compiler
  libgstrtspserver-1.0-dev
  libgstreamer-plugins-bad1.0-dev
  gstreamer1.0-rtsp
  gstreamer1.0-nice
  libglew-dev
  libfftw3-dev
  libv4l-dev
  v4l-utils
  v4l-conf
  libgtk-3-dev
  libtiff-dev
  qt6-base-dev
  qt6-base-dev-tools
  qt6-qpa-plugins
  qt6-wayland
  libyaml-cpp-dev
  apt-file
  libbluetooth-dev
  aptitude
)

# DeepStream 9.1 links libnvinfer.so.10, and bazel/tensorrt_sdk_repository.bzl
# compiles the offline engine builder against whatever TensorRT /usr provides.
# Keep /usr on the TensorRT 10 ABI so the engines it writes stay loadable.
# These are the packages env/debian-package/Dockerfile installs; the tensorrt-dev
# metapackage drags in another ~2GB of lean/dispatch/Windows resources we never use.
TENSORRT_PACKAGES=(
  libnvinfer-headers-dev
  libnvinfer-safe-headers-dev
  libnvinfer-headers-plugin-dev
  libnvinfer10
  libnvinfer-plugin10
  libnvonnxparsers10
  libnvinfer-dev
  libnvinfer-plugin-dev
  libnvonnxparsers-dev
)

# Paths shared with scripts/install_deb.sh so the two agree on one definition.
CUDA_COMPAT_KEYRING='/usr/share/keyrings/hstream-cuda-ubuntu2404-compat.gpg'
CUDA_COMPAT_RELEASE='ubuntu2404'

# Deliberately not /etc/apt/preferences.d/hstream-tensorrt10, which
# scripts/install_deb.sh deletes. That installer only needs the TensorRT 10
# runtime, which coexists with a newer SDK, so a pin there would pointlessly
# downgrade someone's TensorRT 11. A build host is the other case: the
# unversioned -dev packages own /usr/include/NvInfer.h, and that header has to
# stay on 10 for the engines we build to load under DeepStream 9.1.
TENSORRT_PREFERENCES='/etc/apt/preferences.d/hstream-build-tensorrt10'

has_candidate() {
  apt-cache policy "$1" 2>/dev/null |
    awk '/^  Candidate:/ { found = $2 != "(none)" } END { exit !found }'
}

install_apt_packages() {
  local package
  local -a failed=() missing=() blocked=()

  # apt 3.0 (Ubuntu 25.04 and newer) empties /var/lib/apt/lists on `apt clean`,
  # which leaves every uninstalled package "unable to locate". Refresh first so
  # a stale or absent index never looks like a broken dependency.
  sudo apt-get update

  if sudo apt-get install -y "${APT_PACKAGES[@]}"; then
    return 0
  fi

  # apt-get installs all or nothing, so one unsatisfiable package aborts the
  # batch without naming it. Retry individually to report the real offenders.
  for package in "${APT_PACKAGES[@]}"; do
    sudo apt-get install -y "${package}" >/dev/null 2>&1 || failed+=("${package}")
  done
  if [ "${#failed[@]}" -eq 0 ]; then
    return 0
  fi

  for package in "${failed[@]}"; do
    if has_candidate "${package}"; then
      blocked+=("${package}")
    else
      missing+=("${package}")
    fi
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    echo "No installation candidate: ${missing[*]}" >&2
    echo "These are not in the configured repositories. Check that main," >&2
    echo "universe, restricted and multiverse are all enabled for this release." >&2
  fi
  if [ "${#blocked[@]}" -gt 0 ]; then
    echo "Could not install: ${blocked[*]}" >&2
    echo "Run 'sudo apt-get install ${blocked[*]}' to see the conflict." >&2
    echo "Qt held back by a third-party repository (KDE neon, for example) blocks" >&2
    echo "qt6-base-dev; remove those packages so the distribution Qt can install." >&2
  fi
  return 1
}

tensorrt_10_version() {
  apt-cache madison libnvinfer10 2>/dev/null |
    awk '$3 ~ /^10[.]/ && $3 ~ /[+]cuda13[.]2$/ { print $3; exit }'
}

cuda_repository_arch() {
  case "$(dpkg --print-architecture)" in
    amd64) echo x86_64 ;;
    arm64) echo sbsa ;;
    *) return 1 ;;
  esac
}

# 24.04 reaches TensorRT 10 through its own CUDA repository. 26.04 cannot:
# NVIDIA's ubuntu2604 repository starts at TensorRT 11, so the 24.04 repository
# has to be added alongside it. scripts/install_deb.sh adds the same source.
add_cuda_compat_repository() {
  local repo_arch repo_url keyring_deb extract_dir status=0

  if ! repo_arch="$(cuda_repository_arch)"; then
    echo "No TensorRT 10 source is known for $(dpkg --print-architecture)." >&2
    return 1
  fi
  repo_url="https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_COMPAT_RELEASE}/${repo_arch}"

  echo "Adding the ${CUDA_COMPAT_RELEASE} CUDA repository to reach TensorRT 10."
  keyring_deb="$(mktemp --suffix=.deb /tmp/hstream-cuda-keyring.XXXXXX)"
  extract_dir="$(mktemp -d /tmp/hstream-cuda-keyring.XXXXXX)"

  if curl -fsSLo "${keyring_deb}" "${repo_url}/cuda-keyring_1.1-1_all.deb" &&
    dpkg-deb -x "${keyring_deb}" "${extract_dir}" &&
    [ -s "${extract_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" ]; then
    sudo install -m 0644 \
      "${extract_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" \
      "${CUDA_COMPAT_KEYRING}"
    echo "deb [signed-by=${CUDA_COMPAT_KEYRING}] ${repo_url}/ /" |
      sudo tee "/etc/apt/sources.list.d/hstream-cuda-${CUDA_COMPAT_RELEASE}-${repo_arch}.list" >/dev/null
    sudo apt-get update
  else
    echo "Could not fetch the NVIDIA ${CUDA_COMPAT_RELEASE} signing key." >&2
    status=1
  fi

  rm -rf "${keyring_deb}" "${extract_dir}"
  return "${status}"
}

# Installing the right version is not enough on its own. NVIDIA publishes
# TensorRT 11 under these same unversioned names at a higher version, so it is
# the candidate, and `apt full-upgrade` takes it without comment. Hold the
# development packages back; the versioned runtimes stay unpinned so a TensorRT
# 11 SDK can still be installed alongside.
hold_tensorrt_10_development() {
  local package
  local -a development=()

  for package in "${TENSORRT_PACKAGES[@]}"; do
    case "${package}" in
      *-dev) development+=("${package}") ;;
    esac
  done
  [ "${#development[@]}" -gt 0 ] || return 0

  # Priority 1001 is what allows a downgrade, matching --allow-downgrades above.
  # The glob keeps the newest 10.x, and +cuda13.2 sorts above the +cuda12.9
  # build of the same release.
  printf '%s\n' \
    "# Written by env/install_deps.sh." \
    "# DeepStream 9.1 links libnvinfer.so.10, so the headers in /usr that" \
    "# bazel/tensorrt_sdk_repository.bzl compiles against have to stay on 10." \
    "Package: ${development[*]}" \
    "Pin: version 10.*" \
    "Pin-Priority: 1001" |
    sudo tee "${TENSORRT_PREFERENCES}" >/dev/null
}

install_tensorrt_10() {
  local version package
  local -a pinned=()

  version="$(tensorrt_10_version)"
  if [ -z "${version}" ]; then
    add_cuda_compat_repository || return 1
    version="$(tensorrt_10_version)"
  fi
  if [ -z "${version}" ]; then
    echo "NVIDIA's repositories no longer provide the TensorRT 10 / CUDA 13.2" >&2
    echo "packages that DeepStream 9.1 requires." >&2
    return 1
  fi

  for package in "${TENSORRT_PACKAGES[@]}"; do
    pinned+=("${package}=${version}")
  done
  # TensorRT 10 and 11 runtimes coexist, but the -dev packages share one name,
  # so an installed TensorRT 11 SDK has to step aside for the version DeepStream
  # needs.
  sudo apt-get install -y --allow-downgrades "${pinned[@]}" || return 1
  hold_tensorrt_10_development || return 1

  # The unversioned /usr development links must now resolve to TensorRT 10.
  # dpkg-query still reports packages that were removed without being purged,
  # so skip anything that is no longer installed.
  dpkg-query -W -f='${db:Status-Status} ${binary:Package} ${Version}\n' 2>/dev/null |
    awk '$1 == "installed" && $2 ~ /^(tensorrt|libnvinfer|libnvonnxparsers)/ &&
         $2 ~ /-dev$/ && $3 !~ /^10[.]/ {
           bad = 1
           print "Unexpected TensorRT development package: " $2 " " $3 > "/dev/stderr"
         }
         END { exit bad }'
}

install_apt_packages || exit 1
install_tensorrt_10 || exit 1
