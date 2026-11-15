#!/bin/bash
# Build an immutable Jetson Linux 39 arm64 Debian package on a native Jetson
# build host and copy the completed artifact back to this checkout.
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JETSON_HOST="${JETSON_HOST:-stubby}"
JETSON_QT="${JETSON_QT:-0}"
OUTPUT_DIR=""
PACKAGE_VERSION=""

usage() {
  cat <<'USAGE'
Usage: scripts/make_deb_jetson.sh [--host=HOST] [--version=vX.Y.Z] [--output-dir=DIR]

The host must be a Jetson Linux 39 / Ubuntu 24.04 arm64 Jetson with DeepStream 9.1.
The source is transferred as an immutable git archive; local tracked changes
are never included.
Set JETSON_QT=1 to include the Qt UI and its web/experiment helpers (default: 0).
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) JETSON_HOST="$2"; shift ;;
    --host=*) JETSON_HOST="${1#*=}" ;;
    --version) PACKAGE_VERSION="$2"; shift ;;
    --version=*) PACKAGE_VERSION="${1#*=}" ;;
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [[ "${JETSON_QT}" != 0 && "${JETSON_QT}" != 1 ]]; then
  echo "ERROR: JETSON_QT must be 0 or 1." >&2
  exit 1
fi

if [[ ! "${JETSON_HOST}" =~ ^[A-Za-z0-9_.@:-]+$ ]]; then
  echo "ERROR: invalid Jetson SSH host: ${JETSON_HOST}" >&2
  exit 1
fi
if [[ -z "${OUTPUT_DIR}" ]]; then
  OUTPUT_DIR="${TOPDIR}/dist/jetson"
fi
if [[ -z "${PACKAGE_VERSION}" ]]; then
  commit_epoch="$(git -C "${TOPDIR}" show -s --format=%ct HEAD)"
  short_hash="$(git -C "${TOPDIR}" rev-parse --short=7 HEAD)"
  PACKAGE_VERSION="0.0.${commit_epoch}+git.${short_hash}"
fi
debian_version="${PACKAGE_VERSION#v}"
if [[ ! "${debian_version}" =~ ^[0-9]+([.][0-9]+){2}([+~.-][A-Za-z0-9.+:~-]+)?$ ]]; then
  echo "ERROR: invalid Jetson package version: ${PACKAGE_VERSION}" >&2
  exit 1
fi

for command_name in git ssh rsync tar dpkg-deb; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "ERROR: required command not found: ${command_name}" >&2
    exit 1
  fi
done
if ! git -C "${TOPDIR}" diff --quiet HEAD -- || ! git -C "${TOPDIR}" diff --cached --quiet; then
  echo "ERROR: refusing to package tracked or staged source changes." >&2
  exit 1
fi
unexpected_untracked="$({
  git -C "${TOPDIR}" ls-files --others --exclude-standard \
    | grep -Ev '^(bazelisk|run|stitching-calibration-note[.]txt|dist/|dist-staging/|output_workdirs/|bazel-[^/]+(/|$))'
} || true)"
if [[ -n "${unexpected_untracked}" ]]; then
  echo "ERROR: refusing to package untracked source files:" >&2
  while IFS= read -r source_path; do
    printf '  %s\n' "${source_path}" >&2
  done <<< "${unexpected_untracked}"
  exit 1
fi

model_cache="${HSTREAM_MODEL_CACHE_DIR:-${HOME}/.cache/hstream/models}"
if [[ ! -d "${model_cache}" ]]; then
  echo "ERROR: native model cache is missing: ${model_cache}" >&2
  echo "Run the normal asset download once before building a release package." >&2
  exit 1
fi

source_revision="$(git -C "${TOPDIR}" rev-parse HEAD)"
source_epoch="$(git -C "${TOPDIR}" show -s --format=%ct "${source_revision}")"
local_snapshot="$(mktemp -d "${TMPDIR:-/tmp}/hstream-jetson-source.XXXXXX")"
remote_root=""

cleanup() {
  local status=$?
  set +e
  if [[ -n "${remote_root}" && "${remote_root}" == /tmp/hstream-jetson-deb.* ]]; then
    if [[ "${status}" -ne 0 && "${HSTREAM_KEEP_JETSON_BUILD_ON_FAILURE:-0}" == "1" ]]; then
      echo "Jetson failure workspace retained for diagnosis: ${JETSON_HOST}:${remote_root}" >&2
    else
      ssh -o BatchMode=yes "${JETSON_HOST}" rm -rf -- "${remote_root}" >/dev/null 2>&1
    fi
  fi
  rm -rf -- "${local_snapshot}"
  return "${status}"
}
trap cleanup EXIT

git -C "${TOPDIR}" archive --format=tar "${source_revision}" | tar -xf - -C "${local_snapshot}"
printf '%s %s\n' "${source_revision}" "${source_epoch}" > "${local_snapshot}/.hstream-package-source"

remote_identity="$(ssh -o BatchMode=yes "${JETSON_HOST}" \
  'source /etc/os-release; printf "%s %s %s %s\n" "$(uname -m)" "${ID:-}" "${VERSION_ID:-}" "$(dpkg-query -W -f='"'"'${Version}'"'"' deepstream-9.1 2>/dev/null || true)"')"
read -r remote_arch remote_os remote_release remote_deepstream <<< "${remote_identity}"
if [[ "${remote_arch} ${remote_os} ${remote_release}" != "aarch64 ubuntu 24.04" ]] ||
   ! dpkg --compare-versions "${remote_deepstream:-0}" ge 9.1.0-1 ||
   ! dpkg --compare-versions "${remote_deepstream:-0}" lt '9.2~'; then
  echo "ERROR: ${JETSON_HOST} is not the required Jetson Linux 39 build host." >&2
  echo "Detected: ${remote_identity:-unknown}" >&2
  exit 1
fi

remote_root="$(ssh -o BatchMode=yes "${JETSON_HOST}" mktemp -d /tmp/hstream-jetson-deb.XXXXXX)"
if [[ "${remote_root}" != /tmp/hstream-jetson-deb.* ]]; then
  echo "ERROR: Jetson host returned an unsafe temporary path: ${remote_root}" >&2
  remote_root=""
  exit 1
fi
ssh -o BatchMode=yes "${JETSON_HOST}" mkdir -p \
  "${remote_root}/source" "${remote_root}/home/.cache/hstream/models" "${remote_root}/output"
rsync -a "${local_snapshot}/" "${JETSON_HOST}:${remote_root}/source/"
rsync -a "${model_cache}/" "${JETSON_HOST}:${remote_root}/home/.cache/hstream/models/"

echo "[make_deb_jetson] Building ${PACKAGE_VERSION} on ${JETSON_HOST}..."
ssh -o BatchMode=yes "${JETSON_HOST}" bash -s -- "${remote_root}" "${PACKAGE_VERSION}" "${JETSON_QT}" <<'REMOTE_BUILD'
set -euo pipefail
remote_root="$1"
package_version="$2"
jetson_qt="$3"
source_dir="${remote_root}/source"
package_home="${remote_root}/home"
persistent_cache_root="${HOME}/.cache/hstream/jetson-release-build"
output_base="${persistent_cache_root}/output"
sandbox_base="${persistent_cache_root}/sandboxes"

export HOME="${package_home}"
mkdir -p "${persistent_cache_root}/repository" "${sandbox_base}"
git config --global url.https://github.com/.insteadOf ssh://git@github.com/
cd "${source_dir}"

# Install the native SDK and the distribution calibration tools.
build_packages=(
  cuda-toolkit-13-2
  libcudnn9-cuda-13
  libmosquitto1
  libopencv-dev
  python3
  python3-yaml
  cmake
  curl
  libboost-filesystem-dev
  libboost-system-dev
  libexiv2-dev
  libfftw3-dev
  libglew-dev
  libglu1-mesa-dev
  libjpeg-dev
  liblcms2-dev
  libopenexr-dev
  libpano13-dev
  libpng-dev
  libtiff-dev
  hugin-tools
  enblend
  gstreamer1.0-libav
  ninja-build
  zlib1g-dev
)
qt_build_flags=()
qt_targets=()
if [[ "${jetson_qt}" == 1 ]]; then
  build_packages+=(qt6-base-dev qt6-qpa-plugins qt6-image-formats-plugins)
  # deb_jetson already includes the Jetson config and pins native Orin SASS.
  qt_build_flags+=(--define=jetson_qt=true)
  qt_targets+=(
    //src/apps/hstream-ui:hstream-ui
    //src/apps/hstream-ui:hstream-web-job
    //src/apps/hstream-ui:hstream-experiment-task
  )
fi
missing_build_packages=()
for package_name in "${build_packages[@]}"; do
  if [[ "$(dpkg-query -W -f='${Status}' "${package_name}" 2>/dev/null || true)" != "install ok installed" ]]; then
    missing_build_packages+=("${package_name}")
  fi
done
if [[ "${#missing_build_packages[@]}" -gt 0 ]]; then
  sudo -n apt-get install -y --no-install-recommends "${missing_build_packages[@]}"
fi
# Native Jetson uses its installed SDK as the sysroot.
if [[ ! -e /opt/jetson-sysroot ]]; then
  sudo -n ln -s / /opt/jetson-sysroot
fi

# External repositories that use build_file labels contain symlinks back into
# the immutable source snapshot. The snapshot is intentionally deleted after
# each package build, so evict only repositories still tied to an older
# snapshot and let Bazel recreate them from the persistent download cache.
# Keep the action/output cache intact so unchanged native compilation remains
# reusable across package builds.
external_root="${output_base}/external"
declare -A stale_repository_names=()
if [[ -d "${external_root}" ]]; then
  while IFS= read -r -d '' repository_symlink; do
    symlink_target="$(readlink "${repository_symlink}")"
    if [[ "${symlink_target}" != /tmp/hstream-jetson-deb.* ||
          "${symlink_target}" == "${remote_root}"/* ]]; then
      continue
    fi
    repository_relative="${repository_symlink#${external_root}/}"
    repository_name="${repository_relative%%/*}"
    if [[ ! "${repository_name}" =~ ^[A-Za-z0-9._+-]+$ ]]; then
      echo "ERROR: unsafe stale Bazel repository name: ${repository_name}" >&2
      exit 1
    fi
    stale_repository_names["${repository_name}"]=1
  done < <(find "${external_root}" -maxdepth 2 -type l -print0)

  for repository_name in "${!stale_repository_names[@]}"; do
    echo "[make_deb_jetson] Refreshing stale Bazel repository: ${repository_name}"
    rm -rf -- "${external_root}/${repository_name}"
    rm -f -- "${external_root}/@${repository_name}.marker"
  done
fi

# Generate the web catalog from this build's immutable source snapshot.
read -r contract_revision contract_epoch < .hstream-package-source
python3 scripts/sync_stream_contract.py --hstream-root . \
  --native-revision "${contract_revision}" \
  --output dist/web-contract/hstream-web-contract.json \
  --config-checksums dist/web-contract/configs.sha256

bazelisk --batch --output_base="${output_base}" build \
  --repository_cache="${persistent_cache_root}/repository" \
  --sandbox_base="${sandbox_base}" \
  --action_env=TMPDIR=/var/tmp \
  --sandbox_tmpfs_path=/var/tmp \
  --config=opt --config=deb_jetson \
  "${qt_build_flags[@]}" "${qt_targets[@]}" \
  //src/libs/onnx:session_test \
  //src/apps/hstream-cli:hstream-cli \
  //src/apps/hstream-assets:hstream-assets \
  //src/apps/hstream-job:hstream-job \
  //src/apps/hstream-process-guard:hstream-process-guard \
  //src/apps/hstream-runtime-worker:hstream-runtime-worker \
  //src/apps/hstream-finalize:hstream-finalize \
  //src/apps/hstream-calibration-export:hstream-calibration-export \
  //src/apps/hstream-prepared-model-validate:hstream-prepared-model-validate \
  //src/apps/hstream-model-prepare:hstream-model-prepare \
  //src/apps/hstream-model-infer:hstream-model-infer \
  //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo \
  //src/gst-plugins/gst-dsxvideoconvert:libgstdsxvideoconvert.so \
  //src/gst-plugins/gst-videoprep:libnvdsgst_videoprep.so \
  //src/gst-plugins/gst-playtracker:libgstplaytracker.so \
  //src/gst-plugins/gst-fieldmask:libnvdsgst_dsfieldmask.so \
  //src/gst-plugins/gst-player-analytics:libgsthmplayeranalytics.so

cuobjdump=/usr/local/cuda/bin/cuobjdump
if [[ ! -x "${cuobjdump}" ]]; then
  echo "ERROR: Jetson CUDA binary inspector is missing: ${cuobjdump}" >&2
  exit 1
fi
# Imported GPU runtimes need Orin kernels too; link success cannot prove this.
onnx_cuda_provider="${output_base}/external/onnxruntime_linux_jetson/lib/libonnxruntime_providers_cuda.so"
onnx_cubins="$("${cuobjdump}" --all-fatbin --list-elf "${onnx_cuda_provider}")"
if ! grep -q '[.]sm_87[.]cubin$' <<< "${onnx_cubins}"; then
  echo "ERROR: ONNX Runtime CUDA provider lacks native Orin kernels." >&2
  exit 1
fi
HM_REQUIRE_CUDA_TESTS=1 bazel-bin/src/libs/onnx/session_test

cuda_elfs=(
  bazel-bin/src/apps/hstream-cli/hstream-cli
  bazel-bin/src/apps/hstream-calibration-export/hstream-calibration-export
  bazel-bin/src/apps/hstream-assets/hstream-assets
  bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so
  bazel-bin/src/gst-plugins/gst-dsxvideoconvert/libgstdsxvideoconvert.so
  bazel-bin/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so
  bazel-bin/src/gst-plugins/gst-playtracker/libgstplaytracker.so
  bazel-bin/src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so
  bazel-bin/src/gst-plugins/gst-player-analytics/libgsthmplayeranalytics.so
)
cuda_elf_count=0
if [[ "${jetson_qt}" == 1 ]]; then
  cuda_elfs+=(
    bazel-bin/src/apps/hstream-ui/hstream-ui
    bazel-bin/src/apps/hstream-ui/hstream-web-job
    bazel-bin/src/apps/hstream-ui/hstream-experiment-task
  )
fi
for cuda_elf in "${cuda_elfs[@]}"; do
  cubins="$(${cuobjdump} --list-elf "${cuda_elf}" 2>/dev/null || true)"
  [[ -n "${cubins}" ]] || continue
  cuda_elf_count=$((cuda_elf_count + 1))
  if ! grep -q '[.]sm_87[.]cubin$' <<< "${cubins}" || grep -Ev '[.]sm_87[.]cubin$' <<< "${cubins}" >/dev/null; then
    echo "ERROR: Jetson release ELF lacks an sm_87-only native CUDA payload: ${cuda_elf}" >&2
    printf '%s\n' "${cubins}" >&2
    exit 1
  fi
  ptx="$(${cuobjdump} --list-ptx "${cuda_elf}" 2>/dev/null || true)"
  if [[ -n "${ptx}" ]]; then
    echo "ERROR: Jetson release ELF still contains a PTX fallback: ${cuda_elf}" >&2
    printf '%s\n' "${ptx}" >&2
    exit 1
  fi
done
if [[ "${cuda_elf_count}" -eq 0 ]]; then
  echo "ERROR: no CUDA payloads were found in the Jetson release binaries." >&2
  exit 1
fi

HSTREAM_IMMUTABLE_SOURCE=1 \
HSTREAM_TARGET_UBUNTU=24.04 \
HSTREAM_TARGET_PLATFORM=jetson \
JETSON_QT="${jetson_qt}" \
HSTREAM_BAZEL_OUTPUT_BASE="${output_base}" \
HSTREAM_CONTAINER_PACKAGE_STAGING=1 \
scripts/make_deb.sh --version "${package_version}" --output-dir "${remote_root}/output"

deb_version="${package_version#v}"
deb_path="${remote_root}/output/hstream_${deb_version}_arm64.deb"
install_simulation="$(apt-get -s install "${deb_path}")"
if grep -Eq '^(Inst|Conf) (libnvidia-compute|nvidia-firmware|nvidia-kernel-common)' <<< "${install_simulation}"; then
  echo "ERROR: Jetson package installation would pull Ubuntu desktop NVIDIA driver packages." >&2
  printf '%s\n' "${install_simulation}" >&2
  exit 1
fi
REMOTE_BUILD

mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(readlink -f "${OUTPUT_DIR}")"
rsync -a "${JETSON_HOST}:${remote_root}/output/" "${OUTPUT_DIR}/"

deb_path="${OUTPUT_DIR}/hstream_${debian_version}_arm64.deb"
if [[ ! -f "${deb_path}" || "$(dpkg-deb -f "${deb_path}" Architecture)" != "arm64" ||
      "$(dpkg-deb -f "${deb_path}" X-HStream-Qt)" != "${JETSON_QT}" ||
      "$(dpkg-deb -f "${deb_path}" X-HStream-Target-Platform)" != "jetson" ]]; then
  echo "ERROR: Jetson build did not export the expected arm64 package." >&2
  exit 1
fi
echo "Done: ${deb_path}"
