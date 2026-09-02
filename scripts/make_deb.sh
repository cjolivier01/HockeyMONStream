#!/bin/bash
# Build an HStream .deb that installs the application to /opt/hstream.
# The installed run.sh launches hstream-cli without needing the source tree.
#
# Internal usage (the public entrypoints are make deb-ubuntu24/deb-ubuntu26):
#   HSTREAM_IMMUTABLE_SOURCE=1 scripts/make_deb.sh [--version X.Y.Z] [--output-dir DIR]
#
#   --version X.Y.Z  Override package version (default: git describe --tags --always).
#   --output-dir DIR Where to write the .deb (default: dist/).
#
# Requirements: patchelf, dpkg-deb, dpkg-shlibdeps
# (auto-installed from apt if missing).
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_PREFIX="/opt/hstream"
PKG_NAME="hstream"
PKG_ARCH="${PKG_ARCH:-}"
TARGET_PLATFORM="${HSTREAM_TARGET_PLATFORM:-desktop}"
case "${TARGET_PLATFORM}" in
  desktop)
    DEEPSTREAM_PACKAGE="${DEEPSTREAM_PACKAGE:-deepstream-9.1}"
    DEEPSTREAM_MIN_VERSION="${DEEPSTREAM_MIN_VERSION:-9.1.0-1}"
    DEEPSTREAM_MAX_VERSION="${DEEPSTREAM_MAX_VERSION:-9.2~}"
    EXPECTED_CUDA_SONAME="${EXPECTED_CUDA_SONAME:-13}"
    ;;
  jetson)
    DEEPSTREAM_PACKAGE="${DEEPSTREAM_PACKAGE:-deepstream-7.1}"
    DEEPSTREAM_MIN_VERSION="${DEEPSTREAM_MIN_VERSION:-7.1.0-1}"
    DEEPSTREAM_MAX_VERSION="${DEEPSTREAM_MAX_VERSION:-7.2~}"
    EXPECTED_CUDA_SONAME="${EXPECTED_CUDA_SONAME:-12}"
    ;;
  *) echo "ERROR: HSTREAM_TARGET_PLATFORM must be desktop or jetson." >&2; exit 1 ;;
esac
DEEPSTREAM_DEPENDS="${DEEPSTREAM_PACKAGE} (>= ${DEEPSTREAM_MIN_VERSION}), ${DEEPSTREAM_PACKAGE} (<< ${DEEPSTREAM_MAX_VERSION})"

# ---------- arg parsing ----------
PKG_VERSION=""
OUTPUT_DIR="${TOPDIR}/dist"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) PKG_VERSION="$2"; shift ;;
    --version=*) PKG_VERSION="${1#--version=}" ;;
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#--output-dir=}" ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

if [[ "${HSTREAM_IMMUTABLE_SOURCE:-}" != "1" ]]; then
  echo "ERROR: make_deb.sh only packages an immutable source snapshot from the target-OS Docker builder." >&2
  echo "Use 'make deb-ubuntu24' or 'make deb-ubuntu26'." >&2
  exit 1
fi

if [[ ! -f "${TOPDIR}/.hstream-package-source" ]]; then
  echo "ERROR: immutable source revision manifest is missing." >&2
  exit 1
fi
read -r SOURCE_REVISION SOURCE_EPOCH < "${TOPDIR}/.hstream-package-source"
if [[ ! "${SOURCE_REVISION}" =~ ^[0-9a-f]{40}$ || ! "${SOURCE_EPOCH}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: immutable source revision manifest is invalid." >&2
  exit 1
fi
TARGET_UBUNTU="${HSTREAM_TARGET_UBUNTU:-}"
if [[ -z "${TARGET_UBUNTU}" && -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  if [[ "${ID:-}" == "ubuntu" ]]; then
    TARGET_UBUNTU="${VERSION_ID:-}"
  fi
fi
case "${TARGET_PLATFORM}:${TARGET_UBUNTU}" in
  desktop:24.04|desktop:26.04|jetson:22.04) ;;
  desktop:*)
    echo "ERROR: desktop packages require Ubuntu 24.04 or 26.04." >&2
    exit 1
    ;;
  jetson:*)
    echo "ERROR: Jetson packages require the JetPack 6 Ubuntu 22.04 baseline." >&2
    exit 1
    ;;
esac
if [[ -z "$PKG_VERSION" ]]; then
  echo "ERROR: the immutable package builder must provide an explicit version." >&2
  exit 1
fi
# dpkg needs versions that start with a digit; strip a leading 'v'
PKG_VERSION="${PKG_VERSION#v}"
PKG_VERSION="$(printf '%s' "${PKG_VERSION}" | sed -E 's/[^A-Za-z0-9.+:~-]+/./g; s/[.]+/./g; s/^[.]+//; s/[.]+$//')"
if [[ ! "${PKG_VERSION}" =~ ^[0-9] ]]; then
  PKG_VERSION="0.0+git.${PKG_VERSION}"
fi
if ! command -v dpkg &>/dev/null; then
  echo "[make_deb] dpkg not found; installing via apt..."
  sudo apt-get install -y dpkg
fi
if [[ -z "${PKG_ARCH}" ]]; then
  PKG_ARCH="$(dpkg --print-architecture)"
fi
if ! dpkg --validate-archname "${PKG_ARCH}" >/dev/null 2>&1; then
  echo "ERROR: invalid Debian package architecture: ${PKG_ARCH}" >&2
  exit 1
fi
if [[ "${PKG_ARCH}" == "all" || "${PKG_ARCH}" == "any" || "${PKG_ARCH}" == "source" || "${PKG_ARCH}" == *any* ]]; then
  echo "ERROR: unsupported binary package architecture: ${PKG_ARCH}" >&2
  exit 1
fi
case "${TARGET_PLATFORM}:${PKG_ARCH}" in
  desktop:amd64)
    DEB_MULTIARCH=x86_64-linux-gnu
    CUDA_TARGET_TRIPLE=x86_64-linux
    ;;
  jetson:arm64)
    DEB_MULTIARCH=aarch64-linux-gnu
    CUDA_TARGET_TRIPLE=aarch64-linux
    ;;
  *)
    echo "ERROR: unsupported HStream package platform/architecture: ${TARGET_PLATFORM}/${PKG_ARCH}" >&2
    exit 1
    ;;
esac
if ! dpkg --validate-version "${PKG_VERSION}" >/dev/null 2>&1; then
  echo "ERROR: invalid Debian package version: ${PKG_VERSION}" >&2
  exit 1
fi
installed_deepstream_version="$(dpkg-query -W -f='${Version}' "${DEEPSTREAM_PACKAGE}" 2>/dev/null || true)"
if [[ -z "${installed_deepstream_version}" ]] ||
   ! dpkg --compare-versions "${installed_deepstream_version}" ge "${DEEPSTREAM_MIN_VERSION}" ||
   ! dpkg --compare-versions "${installed_deepstream_version}" lt "${DEEPSTREAM_MAX_VERSION}"; then
  echo "ERROR: ${DEEPSTREAM_PACKAGE} >= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION} is required to build this package." >&2
  echo "Installed version: ${installed_deepstream_version:-not installed}" >&2
  exit 1
fi

# ---------- verify artifacts ----------
HSTREAM_CLI="${TOPDIR}/bazel-bin/src/apps/pipeline-app/hstream-cli"
HSTREAM_ASSETS="${TOPDIR}/bazel-bin/src/apps/hstream-assets/hstream-assets"
HSTREAM_UI="${TOPDIR}/bazel-bin/src/apps/hstream-ui/hstream-ui"
HSTREAM_HUGIN_TOOLS_DIR="${HSTREAM_HUGIN_TOOLS_DIR:-}"
HSTREAM_GST_PLUGINS=(
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-dsxvideoconvert/libgstdsxvideoconvert.so"
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so"
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-playtracker/libgstplaytracker.so"
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so"
)
if [[ ! -f "${HSTREAM_CLI}" ]]; then
  echo "ERROR: ${HSTREAM_CLI} not found. Run 'make hstream-cli' first, or pass --build." >&2
  exit 1
fi
if [[ ! -f "${HSTREAM_ASSETS}" ]]; then
  echo "ERROR: ${HSTREAM_ASSETS} not found. Run 'make hstream-assets' first, or pass --build." >&2
  exit 1
fi
if [[ "${TARGET_PLATFORM}" == "desktop" && ! -f "${HSTREAM_UI}" ]]; then
  echo "ERROR: ${HSTREAM_UI} not found. Run 'make hstream-ui' first, or pass --build." >&2
  exit 1
fi
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  for required_hugin_file in \
    bin/autooptimiser \
    bin/pano_modify \
    bin/nona \
    lib/libhuginbase.so.0.0 \
    lib/libvigraimpex.so.11.1.11.1 \
    licenses/hugin/COPYING.txt \
    licenses/vigra/LICENSE.txt; do
    if [[ -z "${HSTREAM_HUGIN_TOOLS_DIR}" || ! -f "${HSTREAM_HUGIN_TOOLS_DIR}/${required_hugin_file}" ]]; then
      echo "ERROR: pinned Jetson Hugin artifact is missing: ${required_hugin_file}" >&2
      exit 1
    fi
  done
fi
for plugin in "${HSTREAM_GST_PLUGINS[@]}"; do
  if [[ ! -f "${plugin}" ]]; then
    echo "ERROR: ${plugin} not found. Run 'make hstream-gst-plugins' first, or use 'make deb'." >&2
    exit 1
  fi
done
if ! command -v file &>/dev/null; then
  echo "[make_deb] file not found; installing via apt..."
  sudo apt-get install -y file
fi
validate_elf_arch() {
  local elf="$1"
  local description
  description="$(file -Lb "${elf}")"
  case "${PKG_ARCH}" in
    amd64)
      [[ "${description}" == *"x86-64"* ]] ;;
    arm64)
      [[ "${description}" == *"aarch64"* || "${description}" == *"ARM aarch64"* ]] ;;
    armhf)
      [[ "${description}" == *"ARM"* && "${description}" == *"32-bit"* ]] ;;
    *)
      echo "ERROR: unsupported package architecture for ELF validation: ${PKG_ARCH}" >&2
      return 1 ;;
  esac || {
    echo "ERROR: ${elf} does not match package architecture ${PKG_ARCH}: ${description}" >&2
    return 1
  }
}
validate_elf_arch "${HSTREAM_CLI}"
validate_elf_arch "${HSTREAM_ASSETS}"
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  validate_elf_arch "${HSTREAM_UI}"
fi

# ---------- ensure tools ----------
if ! command -v patchelf &>/dev/null; then
  echo "[make_deb] patchelf not found; installing via apt..."
  sudo apt-get install -y patchelf
fi
if ! command -v dpkg-deb &>/dev/null; then
  echo "[make_deb] dpkg-deb not found; installing via apt..."
  sudo apt-get install -y dpkg
fi
if ! command -v dpkg-shlibdeps &>/dev/null; then
  echo "[make_deb] dpkg-shlibdeps not found; installing via apt..."
  sudo apt-get install -y dpkg-dev
fi
if ! command -v rsync &>/dev/null; then
  echo "[make_deb] rsync not found; installing via apt..."
  sudo apt-get install -y rsync
fi

# ---------- staging tree ----------
STAGING="${TOPDIR}/dist-staging/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
rm -rf "${STAGING}"

mkdir -p \
  "${STAGING}/DEBIAN" \
  "${STAGING}${INSTALL_PREFIX}/bin" \
  "${STAGING}${INSTALL_PREFIX}/lib/gst-plugins" \
  "${STAGING}${INSTALL_PREFIX}/configs" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/hugin" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/native-calibration-models" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/onnxruntime" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/vigra" \
  "${STAGING}${INSTALL_PREFIX}/scripts" \
  "${STAGING}/usr/share/applications" \
  "${STAGING}/usr/share/doc/${PKG_NAME}" \
  "${STAGING}/usr/share/icons/hicolor/scalable/apps" \
  "${STAGING}/usr/bin"

declare -a package_elfs=()

# ---------- helpers ----------
INSTALL_RPATH="${INSTALL_PREFIX}/lib:/opt/nvidia/deepstream/deepstream/lib:/usr/local/cuda/lib64:/usr/local/cuda/targets/${CUDA_TARGET_TRIPLE}/lib"

patchelf_rpath() {
  local elf="$1"
  chmod u+w "${elf}"
  patchelf --set-rpath "${INSTALL_RPATH}" "${elf}"
}

is_gstreamer_plugin() {
  local elf="$1"
  nm -D "${elf}" 2>/dev/null | grep -E ' gst_plugin_.*_(get_desc|register)$' >/dev/null
}

# Collect non-system shared lib paths from ldd output.
# Excludes: standard system ld paths, DeepStream, and CUDA (all assumed present on target).
collect_bundled_libs() {
  local elf="$1"
  ldd "${elf}" 2>/dev/null \
    | awk '{print $3}' \
    | grep -v '^$' \
    | grep -v '^not$' \
    | while read -r lib; do
        case "${lib}" in
          /opt/nvidia/*) ;;
          /usr/local/cuda/*) ;;
          *) echo "${lib}" ;;
        esac
      done
}

# Returns true if $1 (a real/resolved path) is a system lib we should NOT bundle.
declare -A system_lib_ownership_cache=()
is_system_lib() {
  local real="$1"
  case "${real}" in
    /opt/nvidia/*) return 0 ;;
    /usr/local/cuda*) return 0 ;;
    /usr/lib/*|/lib/*)
      # JetPack images can contain locally installed native libraries (for
      # example OpenCV under /lib) that no Debian package owns. Bundle those
      # libraries so the resulting .deb is self-contained. Distro-owned files
      # remain external dependencies resolved by dpkg-shlibdeps.
      if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
        if [[ -n "${system_lib_ownership_cache[${real}]+set}" ]]; then
          [[ "${system_lib_ownership_cache[${real}]}" == system ]]
          return
        fi
        if dpkg-query -S "${real}" >/dev/null 2>&1; then
          system_lib_ownership_cache["${real}"]=system
          return 0
        fi
        system_lib_ownership_cache["${real}"]=bundle
        return 1
      fi
      return 0
      ;;
  esac
  return 1
}

# Copy a shared lib (resolving symlinks) and create the SONAME symlink.
# Silently skips libs whose resolved path is a system/CUDA/DeepStream lib.
install_lib() {
  local src="$1"
  local dest_dir="$2"
  local real
  real="$(realpath "${src}" 2>/dev/null)" || return 0
  [[ -f "${real}" ]] || return 0

  is_system_lib "${real}" && return 0

  local real_base
  real_base="$(basename "${real}")"
  local soname_base
  soname_base="$(patchelf --print-soname "${real}" 2>/dev/null || true)"
  if [[ -z "${soname_base}" ]]; then soname_base="$(basename "${src}")"; fi

  if [[ ! -f "${dest_dir}/${real_base}" ]]; then
    validate_elf_arch "${real}"
    cp "${real}" "${dest_dir}/${real_base}"
    patchelf_rpath "${dest_dir}/${real_base}"
    package_elfs+=("${dest_dir}/${real_base}")
  fi
  # Create SONAME symlink if different from the real filename
  if [[ "${soname_base}" != "${real_base}" && ! -e "${dest_dir}/${soname_base}" ]]; then
    ln -s "${real_base}" "${dest_dir}/${soname_base}"
  fi
}

# ---------- binaries ----------
echo "[make_deb] Staging hstream binaries..."
cp "${HSTREAM_CLI}" "${STAGING}${INSTALL_PREFIX}/bin/hstream-cli"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hstream-cli"
package_elfs+=("${STAGING}${INSTALL_PREFIX}/bin/hstream-cli")
cp "${HSTREAM_ASSETS}" "${STAGING}${INSTALL_PREFIX}/bin/hstream-assets"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hstream-assets"
package_elfs+=("${STAGING}${INSTALL_PREFIX}/bin/hstream-assets")
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  cp "${HSTREAM_UI}" "${STAGING}${INSTALL_PREFIX}/bin/hstream-ui"
  patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hstream-ui"
  package_elfs+=("${STAGING}${INSTALL_PREFIX}/bin/hstream-ui")
fi
ln -s hstream-cli "${STAGING}${INSTALL_PREFIX}/bin/pipeline-app"

# Ubuntu 22.04 arm64 publishes no hugin-tools package. Jetson releases include
# pinned source-built copies of the required commands and their private
# Hugin/VIGRA libraries, with upstream licenses, so clean-state calibration is
# available without an unsatisfiable package dependency.
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  for hugin_tool in autooptimiser pano_modify nona; do
    hugin_source="${HSTREAM_HUGIN_TOOLS_DIR}/bin/${hugin_tool}"
    hugin_destination="${STAGING}${INSTALL_PREFIX}/bin/${hugin_tool}"
    validate_elf_arch "${hugin_source}"
    install -m 0755 "${hugin_source}" "${hugin_destination}"
    patchelf_rpath "${hugin_destination}"
    package_elfs+=("${hugin_destination}")
  done
  install_lib "${HSTREAM_HUGIN_TOOLS_DIR}/lib/libhuginbase.so.0.0" "${STAGING}${INSTALL_PREFIX}/lib"
  install_lib "${HSTREAM_HUGIN_TOOLS_DIR}/lib/libvigraimpex.so.11.1.11.1" "${STAGING}${INSTALL_PREFIX}/lib"
  install -m 0644 "${HSTREAM_HUGIN_TOOLS_DIR}/licenses/hugin/COPYING.txt" \
    "${STAGING}${INSTALL_PREFIX}/share/licenses/hugin/COPYING.txt"
  install -m 0644 "${HSTREAM_HUGIN_TOOLS_DIR}/licenses/vigra/LICENSE.txt" \
    "${STAGING}${INSTALL_PREFIX}/share/licenses/vigra/LICENSE.txt"
  LD_LIBRARY_PATH="${STAGING}${INSTALL_PREFIX}/lib" "${STAGING}${INSTALL_PREFIX}/bin/autooptimiser" \
    --help >/dev/null
  LD_LIBRARY_PATH="${STAGING}${INSTALL_PREFIX}/lib" "${STAGING}${INSTALL_PREFIX}/bin/pano_modify" --help >/dev/null
  LD_LIBRARY_PATH="${STAGING}${INSTALL_PREFIX}/lib" "${STAGING}${INSTALL_PREFIX}/bin/nona" --help >/dev/null
fi

# ---------- bundled private shared libs ----------
echo "[make_deb] Collecting bundled shared libs..."
declare -A seen_libs

# Collect from the binaries and the exact HStream-owned plugin set.
all_elfs=("${HSTREAM_CLI}" "${HSTREAM_ASSETS}" "${HSTREAM_GST_PLUGINS[@]}")
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  all_elfs+=("${HSTREAM_UI}")
fi

for elf in "${all_elfs[@]}"; do
  while IFS= read -r lib_path; do
    real="$(realpath "${lib_path}" 2>/dev/null)" || continue
    [[ -f "${real}" ]] || continue
    is_system_lib "${real}" && continue
    if [[ -z "${seen_libs[${real}]+set}" ]]; then
      seen_libs["${real}"]=1
      install_lib "${lib_path}" "${STAGING}${INSTALL_PREFIX}/lib"
    fi
  done < <(collect_bundled_libs "${elf}")
done

# ONNX Runtime is pinned by WORKSPACE and is not assumed to exist as a distro
# package. collect_bundled_libs stages its shared library; preserve the
# upstream notices alongside it.
BAZEL_OUTPUT_BASE="${HSTREAM_BAZEL_OUTPUT_BASE:-}"
if [[ -z "${BAZEL_OUTPUT_BASE}" ]]; then
  BAZEL_OUTPUT_BASE="$("${TOPDIR}/bazelisk" info output_base 2>/dev/null || bazelisk info output_base)"
fi
case "${PKG_ARCH}" in
  amd64) ORT_REPOSITORY=onnxruntime_linux_x86_64 ;;
  arm64) ORT_REPOSITORY=onnxruntime_linux_aarch64 ;;
  *)
    echo "ERROR: unsupported package architecture for ONNX Runtime notices: ${PKG_ARCH}" >&2
    exit 1
    ;;
esac
ORT_SOURCE="${BAZEL_OUTPUT_BASE}/external/${ORT_REPOSITORY}"
if [[ ! -f "${ORT_SOURCE}/LICENSE" || ! -f "${ORT_SOURCE}/ThirdPartyNotices.txt" ]]; then
  echo "ERROR: pinned ONNX Runtime notices were not found under ${ORT_SOURCE}" >&2
  exit 1
fi
install -m 0644 "${ORT_SOURCE}/LICENSE" "${STAGING}${INSTALL_PREFIX}/share/licenses/onnxruntime/LICENSE"
install -m 0644 "${ORT_SOURCE}/ThirdPartyNotices.txt" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/onnxruntime/ThirdPartyNotices.txt"
for native_model_notice in Apache-2.0-LICENSE.txt DeDoDe-LICENSE.txt NOTICE.txt; do
  native_model_notice_source="${TOPDIR}/third_party/native_model_licenses/${native_model_notice}"
  if [[ ! -f "${native_model_notice_source}" ]]; then
    echo "ERROR: native calibration model notice is missing: ${native_model_notice_source}" >&2
    exit 1
  fi
  install -m 0644 "${native_model_notice_source}" \
    "${STAGING}${INSTALL_PREFIX}/share/licenses/native-calibration-models/${native_model_notice}"
done
install -m 0644 "${TOPDIR}/LICENSE.md" "${STAGING}/usr/share/doc/${PKG_NAME}/copyright"

# ---------- HStream GStreamer plugins ----------
echo "[make_deb] Staging GStreamer plugins..."

# DeepStream owns its NVIDIA plugins. Stage only the four plugins built and
# owned by this repository; never pick up stale Bazel outputs opportunistically.
for so in "${HSTREAM_GST_PLUGINS[@]}"; do
  if ! is_gstreamer_plugin "${so}"; then
    echo "ERROR: expected GStreamer plugin does not export a plugin descriptor: ${so}" >&2
    exit 1
  fi
  dest="${STAGING}${INSTALL_PREFIX}/lib/gst-plugins/$(basename "${so}")"
  validate_elf_arch "${so}"
  cp "${so}" "${dest}"
  patchelf_rpath "${dest}"
  package_elfs+=("${dest}")
done

# ---------- YOLO custom inference lib ----------
YOLO_SO="${TOPDIR}/bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
if [[ ! -f "${YOLO_SO}" ]]; then
  echo "[make_deb] ERROR: missing ${YOLO_SO}; run 'make yolo-custom-lib' before packaging." >&2
  exit 1
fi
echo "[make_deb] Staging libnvdsinfer_custom_impl_Yolo.so..."
validate_elf_arch "${YOLO_SO}"
TENSORRT_NEEDED="$(patchelf --print-needed "${YOLO_SO}" | grep -E '^lib(nvinfer|nvonnxparser)' || true)"
if [[ -z "${TENSORRT_NEEDED}" ]] || grep -Ev '^lib(nvinfer|nvinfer_plugin|nvonnxparser)[.]so[.]10$' <<< "${TENSORRT_NEEDED}" >/dev/null; then
  echo "[make_deb] ERROR: DeepStream 9.1 requires TensorRT ABI 10, but ${YOLO_SO} needs:" >&2
  printf '  %s\n' "${TENSORRT_NEEDED:-(no TensorRT libraries found)}" >&2
  echo "Build in a target-OS container or install the pinned TensorRT 10 development package." >&2
  exit 1
fi
cp "${YOLO_SO}" "${STAGING}${INSTALL_PREFIX}/lib/libnvdsinfer_custom_impl_Yolo.so"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/lib/libnvdsinfer_custom_impl_Yolo.so"
package_elfs+=("${STAGING}${INSTALL_PREFIX}/lib/libnvdsinfer_custom_impl_Yolo.so")

# ---------- configs ----------
echo "[make_deb] Staging configs..."
cp -r "${TOPDIR}/configs/." "${STAGING}${INSTALL_PREFIX}/configs/"
# Remove the systemd unit files — those belong to a separate package/install step
rm -rf "${STAGING}${INSTALL_PREFIX}/configs/systemd"
# Source checkouts keep native models in a per-user cache. Installed configs
# reference immutable package-owned copies for redistributable models. The
# non-redistributable rink and SuperPoint graphs remain per-user downloads.
for native_config in ds_hockey_app_config.yaml ds_hockey_configure_stitching.yaml; do
  for packaged_native_model in \
      dedode-lightglue-lc4v2-bupright-f8bd053e44d57a77.onnx \
      efficient-loftr-outdoor-opt-a2cbdcfef0ddb5cd.onnx; do
    sed -i \
      "s#\\\$HOME/.cache/hstream/models/${packaged_native_model}#${INSTALL_PREFIX}/pretrained/native-calibration/${packaged_native_model}#g" \
      "${STAGING}${INSTALL_PREFIX}/configs/${native_config}"
  done
done
# The hockey YOLO checkpoint likewise has no recorded redistribution grant.
# Keep the source-checkout path unchanged, but make the installed declaration
# and TensorRT input use the same writable per-user download target.
hockey_yolo_model="hm_crowdhuman_e85_yolov8_m_1984_736_dynamic_b1-b2_1984x736.onnx"
sed -i \
  "s#onnx-file: ../pretrained/deepstream/yolov8/${hockey_yolo_model}#onnx-file: \$HOME/.cache/hstream/models/${hockey_yolo_model}#g" \
  "${STAGING}${INSTALL_PREFIX}/configs/config_infer_yolov8_hockey.yaml"
if ! grep -Fqx \
    "  onnx-file: \$HOME/.cache/hstream/models/${hockey_yolo_model}" \
    "${STAGING}${INSTALL_PREFIX}/configs/config_infer_yolov8_hockey.yaml"; then
  echo "ERROR: installed hockey YOLO config does not preserve its per-user model path." >&2
  exit 1
fi

# ---------- pretrained assets ----------
echo "[make_deb] Staging declared non-engine pretrained assets..."
asset_manifest="$(mktemp)"
if ! "${HSTREAM_ASSETS}" --package-assets --verify "${TOPDIR}/configs/ds_hockey_app_config.yaml"; then
  echo "ERROR: every package-owned pretrained asset must exist and match its declared SHA256." >&2
  exit 1
fi
"${HSTREAM_ASSETS}" --package-assets --print-targets "${TOPDIR}/configs/ds_hockey_app_config.yaml" \
  > "${asset_manifest}"
pretrained_root="$(readlink -f "${TOPDIR}/pretrained" 2>/dev/null || true)"
model_cache_root="$(readlink -f "${HOME}/.cache/hstream/models" 2>/dev/null || true)"
while IFS= read -r asset; do
  [[ -n "${asset}" ]] || continue
  [[ "${asset}" != *.engine ]] || continue
  if [[ ! -f "${asset}" ]]; then
    echo "ERROR: verified pretrained asset disappeared before staging: ${asset}" >&2
    exit 1
  fi
  asset_real="$(readlink -f "${asset}")"
  if [[ -n "${pretrained_root}" && "${asset_real}" == "${pretrained_root}/"* ]]; then
    rel="${asset_real#"${pretrained_root}"/}"
  elif [[ -n "${model_cache_root}" && "${asset_real}" == "${model_cache_root}/"* ]]; then
    rel="native-calibration/${asset_real#"${model_cache_root}"/}"
  else
    echo "ERROR: package-owned pretrained asset is outside an approved pretrained/model-cache root: ${asset}" >&2
    exit 1
  fi
  dest="${STAGING}${INSTALL_PREFIX}/pretrained/${rel}"
  mkdir -p "$(dirname "${dest}")"
  # Downloaded assets may inherit mkstemp's owner-only mode.  Package runtime
  # data as world-readable so unprivileged hstream processes can load it.
  source_hash_before="$(sha256sum "${asset_real}")"
  source_hash_before="${source_hash_before%% *}"
  install -m 0644 "${asset_real}" "${dest}"
  source_hash_after="$(sha256sum "${asset_real}")"
  source_hash_after="${source_hash_after%% *}"
  staged_hash="$(sha256sum "${dest}")"
  staged_hash="${staged_hash%% *}"
  if [[ "${source_hash_before}" != "${source_hash_after}" ]] ||
     [[ "${source_hash_before}" != "${staged_hash}" ]]; then
    echo "ERROR: pretrained asset changed while it was staged: ${asset}" >&2
    exit 1
  fi
done < "${asset_manifest}"
# Close the verification/copy window by confirming the sources still match the
# declared manifest after every staged byte has been rehashed.
if ! "${HSTREAM_ASSETS}" --package-assets --verify "${TOPDIR}/configs/ds_hockey_app_config.yaml"; then
  echo "ERROR: a pretrained source changed during package staging." >&2
  exit 1
fi
rm -f "${asset_manifest}"

# The native binaries retain Bazel-linked HockeyMOM C++ components, but no
# HockeyMOM Python or site-packages are part of the Debian runtime.
# ---------- installed run.sh ----------
echo "[make_deb] Writing installed run.sh..."
cat > "${STAGING}${INSTALL_PREFIX}/run.sh" <<'RUNSH'
#!/bin/bash
set -euo pipefail

INSTALL_DIR=/opt/hstream

# DeepStream 9.1's legacy nvstreammux rejects native 8K input buffers. The
# replacement mux handles the source resolution used for stitching. Preserve
# an explicit caller override for diagnostics and older DeepStream releases.
export USE_NEW_NVSTREAMMUX="${USE_NEW_NVSTREAMMUX:-yes}"

prepend_path() {
  local var_name="$1"
  local dir="$2"
  local cur="${!var_name-}"
  if [ -z "${dir}" ] || [ ! -d "${dir}" ]; then return; fi
  if [ -z "${cur}" ]; then export "${var_name}=${dir}"; return; fi
  case ":${cur}:" in
    *":${dir}:"*) ;;
    *) export "${var_name}=${dir}:${cur}" ;;
  esac
}

append_path() {
  local var_name="$1"
  local dir="$2"
  local cur="${!var_name-}"
  if [ -z "${dir}" ] || [ ! -d "${dir}" ]; then return; fi
  if [ -z "${cur}" ]; then export "${var_name}=${dir}"; return; fi
  case ":${cur}:" in
    *":${dir}:"*) ;;
    *) export "${var_name}=${cur}:${dir}" ;;
  esac
}

# Per-user registry so the read-only install dir stays clean.
GST_REGISTRY_DIR="${HOME}/.cache/gstreamer-1.0"
mkdir -p "${GST_REGISTRY_DIR}"
export GST_REGISTRY="${GST_REGISTRY_DIR}/registry.hstream.$(uname -m).bin"

prepend_path GST_PLUGIN_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path PATH "${INSTALL_DIR}/bin"

# Bundled private libs are embedded in /opt/hstream/lib; the binary's
# RPATH already includes this dir, but gst-plugins are dlopen'd at runtime so
# LD_LIBRARY_PATH is still needed for them.
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/libcusparseLt/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/aarch64-linux-gnu/tegra"
prepend_path LD_LIBRARY_PATH "/usr/local/cuda/targets/aarch64-linux/lib"
one_pass_only=1
have_sink_arg=0
show_arg=0
rewritten_args=()

next_arg_is_sink_value=0
for arg in "$@"; do
  if [ "${next_arg_is_sink_value}" -eq 1 ]; then
    next_arg_is_sink_value=0
    continue
  fi
  case "$arg" in
    --one-pass-only|--stage0-only) one_pass_only=1 ;;
    --two-stage|--configure-first) one_pass_only=0 ;;
    --enable-sinks|-k)
      have_sink_arg=1
      next_arg_is_sink_value=1
      ;;
    --enable-sinks=*)
      have_sink_arg=1
      ;;
    -k*)
      have_sink_arg=1
      ;;
    --show) show_arg=1 ;;
  esac
done

has_display=0
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
  has_display=1
fi
ssh_forwarded_display=0
case "${DISPLAY:-}" in
  localhost:*|127.0.0.1:*|::1:*)
    ssh_forwarded_display=1
    ;;
esac
if [ "${ssh_forwarded_display}" -eq 1 ] && [ "${HM_ALLOW_SSH_RENDER:-0}" != "1" ]; then
  # SSH-forwarded X displays are technically present, but DeepStream EGL/nv3dsink output is not a reliable default.
  has_display=0
fi
headless_show_rtsp=0
if [ "${show_arg}" -eq 1 ] && [ "${has_display}" -eq 0 ]; then
  headless_show_rtsp=1
fi

for arg in "$@"; do
  case "$arg" in
    --one-pass-only|--stage0-only|--two-stage|--configure-first)
      continue
      ;;
    --show)
      if [ "${headless_show_rtsp}" -eq 1 ]; then
        continue
      fi
      ;;
  esac
  case "$arg" in
    -t=*)
      rewritten_args+=("-t" "${arg#-t=}")
      ;;
    *)
      rewritten_args+=("$arg")
      ;;
  esac
done

print_rtsp_access_urls() {
  local port="${1:-8554}"
  local path="${2:-/ds-test}"
  local reason="${3:-RTSP sink requested; streaming RTSP}"
  local addresses=()
  local seen=" "
  local candidate

  add_address() {
    local addr="$1"
    if [[ ! "${addr}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
      return
    fi
    case "${addr}" in
      ""|127.*|0.0.0.0) return ;;
    esac
    case " ${seen} " in
      *" ${addr} "*) return ;;
    esac
    seen="${seen}${addr} "
    addresses+=("${addr}")
  }

  if command -v hostname >/dev/null 2>&1; then
    for candidate in $(hostname -I 2>/dev/null || true); do
      add_address "${candidate}"
    done
  fi
  if command -v ip >/dev/null 2>&1; then
    while IFS= read -r candidate; do
      add_address "${candidate}"
    done < <(ip -o -4 addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
  fi
  if command -v ifconfig >/dev/null 2>&1; then
    while IFS= read -r candidate; do
      add_address "${candidate}"
    done < <(ifconfig 2>/dev/null | awk '/inet / {print $2}')
  fi

  echo "${reason} on 0.0.0.0:${port}${path}"
  echo "Chrome does not play RTSP URLs."
  echo "Open with a TCP-capable RTSP client, for example:"
  echo "VLC needs a generic RTSP client plugin such as live555; VLC builds that fall back to SAT>IP will not open this stream."
  if [ "${#addresses[@]}" -eq 0 ]; then
    echo "  No non-loopback IPv4 address detected; try: ffplay -rtsp_transport tcp rtsp://localhost:${port}${path}"
    return
  fi
  for candidate in "${addresses[@]}"; do
    echo "  ffplay -rtsp_transport tcp rtsp://${candidate}:${port}${path}"
    echo "  vlc rtsp://${candidate}:${port}${path}"
  done
}

print_webrtc_access_urls() {
  local port="${1:-8080}"
  local addresses=()
  local seen=" "
  local candidate

  if command -v hostname >/dev/null 2>&1; then
    while IFS= read -r candidate; do
      candidate="${candidate%% *}"
      if [ -z "${candidate}" ] || [[ "${candidate}" == 127.* ]]; then
        continue
      fi
      if [[ "${seen}" != *" ${candidate} "* ]]; then
        addresses+=("${candidate}")
        seen="${seen}${candidate} "
      fi
    done < <(hostname -I 2>/dev/null | tr ' ' '\n')
  fi

  if [ "${#addresses[@]}" -eq 0 ] && command -v ip >/dev/null 2>&1; then
    while IFS= read -r candidate; do
      candidate="${candidate%/*}"
      if [ -z "${candidate}" ] || [[ "${candidate}" == 127.* ]]; then
        continue
      fi
      if [[ "${seen}" != *" ${candidate} "* ]]; then
        addresses+=("${candidate}")
        seen="${seen}${candidate} "
      fi
    done < <(ip -o -4 addr show scope global 2>/dev/null | awk '{print $4}')
  fi

  if [ "${#addresses[@]}" -eq 0 ] && command -v ifconfig >/dev/null 2>&1; then
    while IFS= read -r candidate; do
      if [ -z "${candidate}" ] || [[ "${candidate}" == 127.* ]]; then
        continue
      fi
      if [[ "${seen}" != *" ${candidate} "* ]]; then
        addresses+=("${candidate}")
        seen="${seen}${candidate} "
      fi
    done < <(ifconfig 2>/dev/null | awk '/inet / {print $2}')
  fi

  echo "WEBRTC sink requested; browser preview signaling on 0.0.0.0:${port}"
  if [ "${#addresses[@]}" -eq 0 ]; then
    echo "  http://localhost:${port}/"
    return
  fi
  for candidate in "${addresses[@]}"; do
    echo "  http://${candidate}:${port}/"
  done
}

check_webrtc_runtime() {
  if command -v gst-inspect-1.0 >/dev/null 2>&1 &&
    ! gst-inspect-1.0 nicesrc >/dev/null 2>&1; then
    echo "WEBRTC sink requires the GStreamer libnice plugin; install package: gstreamer1.0-nice"
  fi
}

sink_value_has_any() {
  local value="$1"
  shift
  local token normalized
  local -a sink_tokens
  local accepted
  IFS=',' read -r -a sink_tokens <<< "${value}"
  for token in "${sink_tokens[@]}"; do
    token="${token#"${token%%[![:space:]]*}"}"
    token="${token%"${token##*[![:space:]]}"}"
    normalized="${token//-/_}"
    normalized="${normalized^^}"
    for accepted in "$@"; do
      if [ "${normalized}" = "${accepted}" ]; then
        return 0
      fi
    done
  done
  return 1
}

sink_value_has_render() {
  sink_value_has_any "$1" RENDER 2
}

sink_value_has_rtsp() {
  sink_value_has_any "$1" RTSP UDPSINK
}

sink_value_has_server() {
  sink_value_has_any "$1" RTSP UDPSINK RTMP 4
}

sink_value_has_webrtc() {
  sink_value_has_any "$1" WEBRTC 7
}

args_request_sink_matching() {
  local matcher="$1"
  shift
  local args=("$@")
  local arg i
  for ((i = 0; i < ${#args[@]}; i++)); do
    arg="${args[$i]}"
    case "${arg}" in
      --enable-sinks|-k)
        if [ "$((i + 1))" -lt "${#args[@]}" ] && "${matcher}" "${args[$((i + 1))]}"; then
          return 0
        fi
        ;;
      --enable-sinks=*)
        if "${matcher}" "${arg#*=}"; then
          return 0
        fi
        ;;
      -k*)
        if "${matcher}" "${arg#-k}"; then
          return 0
        fi
        ;;
    esac
  done
  return 1
}

args_request_render_sink() {
  args_request_sink_matching sink_value_has_render "$@"
}

args_request_rtsp_sink() {
  args_request_sink_matching sink_value_has_rtsp "$@"
}

args_request_server_sink() {
  args_request_sink_matching sink_value_has_server "$@"
}

args_request_webrtc_sink() {
  args_request_sink_matching sink_value_has_webrtc "$@"
}

default_main_sink="RENDER"
if [ "${headless_show_rtsp}" -eq 1 ] && [ "${have_sink_arg}" -eq 0 ]; then
  default_main_sink="RTSP"
  print_rtsp_access_urls 8554 /ds-test "Headless --show requested; streaming RTSP"
elif [ "${have_sink_arg}" -eq 1 ] && args_request_rtsp_sink "${rewritten_args[@]}"; then
  print_rtsp_access_urls 8554 /ds-test "RTSP sink requested; streaming RTSP"
elif [ "${have_sink_arg}" -eq 1 ] && args_request_server_sink "${rewritten_args[@]}"; then
  print_rtsp_access_urls 8554 /ds-test "Server sink requested; RTSP-configured sinks stream RTSP"
elif [ "${has_display}" -eq 0 ]; then
  default_main_sink="ENCODE_FILE"
  if [ "${have_sink_arg}" -eq 0 ]; then
    if [ "${ssh_forwarded_display}" -eq 1 ] && [ "${HM_ALLOW_SSH_RENDER:-0}" != "1" ]; then
      echo "DISPLAY=${DISPLAY} looks SSH-forwarded and no sink was specified; defaulting to --enable-sinks=${default_main_sink} (set HM_ALLOW_SSH_RENDER=1 or pass --enable-sinks=RENDER to force render)"
    else
      echo "DISPLAY is not set and no sink was specified; defaulting to --enable-sinks=${default_main_sink} (override with --enable-sinks=RENDER)"
    fi
  fi
fi
if [ "${have_sink_arg}" -eq 1 ] && args_request_webrtc_sink "${rewritten_args[@]}"; then
  print_webrtc_access_urls 8080
  check_webrtc_runtime
fi

sink_args=()
config_args=()
if [ "${one_pass_only}" -eq 1 ]; then
  config_args=(-c "${INSTALL_DIR}/configs/ds_hockey_app_config.yaml")
  if [ "${have_sink_arg}" -eq 0 ]; then
    sink_args+=(--enable-sinks="${default_main_sink}")
  fi
else
  config_args=(
    -c "${INSTALL_DIR}/configs/ds_hockey_configure_stitching.yaml"
    -c "${INSTALL_DIR}/configs/ds_hockey_app_config.yaml"
  )
  sink_args+=(--enable-sinks=FAKE)
  if [ "${have_sink_arg}" -eq 0 ]; then
    sink_args+=(--enable-sinks="${default_main_sink}")
  fi
fi

hmaudio_enable=1

# cd so that relative paths in DeepStream config files (e.g. custom-lib-path=lib/...)
# resolve against the install directory.
cd "${INSTALL_DIR}"

pipeline_args=(
  "${config_args[@]}"
  --enable-sources=URI-MULTIPLE
  "${sink_args[@]}"
  --options=pipeline.hmaudio.enable="${hmaudio_enable}"
  "${rewritten_args[@]}"
)

if [ "${ssh_forwarded_display}" -eq 1 ] && [ "${HM_ALLOW_SSH_RENDER:-0}" != "1" ] &&
  ! args_request_render_sink "${pipeline_args[@]}"; then
  echo "Unsetting SSH-forwarded DISPLAY=${DISPLAY} for non-render sinks so DeepStream EGL uses headless mode"
  unset DISPLAY
fi

exec "${INSTALL_DIR}/bin/hstream-cli" "${pipeline_args[@]}"
RUNSH
chmod 755 "${STAGING}${INSTALL_PREFIX}/run.sh"

# ---------- installed UI wrapper ----------
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  cat > "${STAGING}${INSTALL_PREFIX}/hstream-ui.sh" <<'UISH'
#!/bin/bash
set -euo pipefail

INSTALL_DIR=/opt/hstream

export USE_NEW_NVSTREAMMUX="${USE_NEW_NVSTREAMMUX:-yes}"

prepend_path() {
  local var_name="$1"
  local dir="$2"
  local cur="${!var_name-}"
  if [ -z "${dir}" ] || [ ! -d "${dir}" ]; then return; fi
  if [ -z "${cur}" ]; then export "${var_name}=${dir}"; return; fi
  case ":${cur}:" in
    *":${dir}:"*) ;;
    *) export "${var_name}=${dir}:${cur}" ;;
  esac
}

prepend_path GST_PLUGIN_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path PATH "${INSTALL_DIR}/bin"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/libcusparseLt/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/aarch64-linux-gnu/tegra"
prepend_path LD_LIBRARY_PATH "/usr/local/cuda/targets/aarch64-linux/lib"
exec "${INSTALL_DIR}/bin/hstream-ui" "$@"
UISH
  chmod 755 "${STAGING}${INSTALL_PREFIX}/hstream-ui.sh"
fi

# A short-lived older package left its runtime calibration tree unowned after
# upgrades. Current releases are fully native, so remove only that exact legacy
# install-prefix residue during configuration.
cat > "${STAGING}/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -d /opt/hstream/python ]; then
  rm -rf -- /opt/hstream/python
fi
exit 0
POSTINST
chmod 0755 "${STAGING}/DEBIAN/postinst"

# ---------- package-owned command wrappers ----------
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/hstream-cli"
ln -s "${INSTALL_PREFIX}/bin/hstream-assets" "${STAGING}/usr/bin/hstream-assets"
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/hstream"
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/pipeline-app"
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  ln -s "${INSTALL_PREFIX}/hstream-ui.sh" "${STAGING}/usr/bin/hstream-ui"
  install -m 0644 \
    "${TOPDIR}/src/apps/hstream-ui/hstream-ui.desktop" \
    "${STAGING}/usr/share/applications/hstream-ui.desktop"
  install -m 0644 \
    "${TOPDIR}/src/apps/hstream-ui/hstream-ui.svg" \
    "${STAGING}/usr/share/icons/hicolor/scalable/apps/hstream-ui.svg"
fi

# ---------- DEBIAN/control ----------
echo "[make_deb] Writing DEBIAN/control..."

# Resolve distro- and ABI-specific package names (for example libavformat62,
# libfftw3-single3, and Qt t64 transitions) from the ELF files being packaged.
# CUDA, DeepStream, and TensorRT dependencies are resolved from the target-OS
# builder so each artifact names packages available for its ABI baseline.
SHLIBDEPS_WORK_DIR="${STAGING}/.shlibdeps"
mkdir -p "${SHLIBDEPS_WORK_DIR}/debian"
cat > "${SHLIBDEPS_WORK_DIR}/debian/control" <<SHLIBDEPS_CONTROL
Source: ${PKG_NAME}
Section: misc
Priority: optional
Maintainer: Christopher Olivier <cjolivier01@gmail.com>
Standards-Version: 4.6.2

Package: ${PKG_NAME}
Architecture: any
Description: HStream dependency resolution metadata
SHLIBDEPS_CONTROL

declare -a shlibdeps_elf_args=()
declare -a shlibdeps_private_lib_args=()
declare -a shlibdeps_private_lib_dirs=()
for elf in "${package_elfs[@]}"; do
  CUDA_NEEDED="$(patchelf --print-needed "${elf}" \
    | grep -E '^lib(cudart|npp[^.]*|cublas[^.]*|cufft[^.]*|curand[^.]*|cusolver[^.]*|cusparse[^.]*|nvrtc[^.]*|nvJitLink)[.]so[.][0-9]+$' || true)"
  if [[ -n "${CUDA_NEEDED}" ]] && grep -Ev "[.]so[.]${EXPECTED_CUDA_SONAME}$" <<< "${CUDA_NEEDED}" >/dev/null; then
    echo "ERROR: ${TARGET_PLATFORM} package ELF linked against an unexpected CUDA major: ${elf}" >&2
    printf '  %s\n' "${CUDA_NEEDED}" >&2
    exit 1
  fi
  shlibdeps_elf_args+=("-e${elf}")
done

# nvtracker is provided by the DeepStream Debian package, but its low-level
# implementation is dlopen'd and therefore its CUDA/MQTT dependencies are not
# visible in HStream's own ELF graph. Resolve that runtime graph without
# copying any DeepStream-owned files into this package.
DEEPSTREAM_TRACKER_RUNTIME="/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so"
if [[ ! -f "${DEEPSTREAM_TRACKER_RUNTIME}" ]]; then
  echo "ERROR: DeepStream nvtracker runtime not found: ${DEEPSTREAM_TRACKER_RUNTIME}" >&2
  exit 1
fi
validate_elf_arch "${DEEPSTREAM_TRACKER_RUNTIME}"
dependency_elfs=("${package_elfs[@]}")
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  # Desktop DeepStream's package metadata does not expose every dependency of
  # its dlopen'd tracker implementation (notably MQTT), so include that ELF in
  # dependency discovery. JetPack's deepstream-7.1 package already declares
  # the tracker/VPI runtime graph; asking dpkg-shlibdeps to analyze NVIDIA's
  # private tracker ELF there fails because libnvvpi3 ships no shlibs metadata.
  dependency_elfs+=("${DEEPSTREAM_TRACKER_RUNTIME}")
  shlibdeps_elf_args+=("-e${DEEPSTREAM_TRACKER_RUNTIME}")
fi

# NVIDIA does not ship Debian shlibs metadata for its unversioned DeepStream
# libraries or most CUDA toolkit libraries. Generate metadata from the packages
# that own the resolved CUDA files; TensorRT retains its package-provided
# metadata. libcuda is supplied by the host driver through the
# libcuda.so.1 virtual package. NVIDIA's CUDA-repository driver packages do
# not provide Ubuntu's older libcuda1 alias, so depending on that alias can
# make apt replace an otherwise compatible installed driver.
SHLIBS_LOCAL="${SHLIBDEPS_WORK_DIR}/debian/shlibs.local"
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  printf '%s\n' 'libcuda 1 nvidia-l4t-cuda | libcuda.so.1' > "${SHLIBS_LOCAL}"
else
  printf '%s\n' 'libcuda 1 libcuda.so.1' > "${SHLIBS_LOCAL}"
fi

# NVIDIA's JetPack repository owns its OpenCV 4.8 libraries in the `libopencv`
# package but publishes no shlibs metadata. Supply minimum-version mappings so
# dpkg-shlibdeps emits the maintained package dependency instead of bundling
# about 40 MiB of duplicate OpenCV code into HStream. A minimum permits
# JetPack security/bug-fix package upgrades, while a next-minor upper bound
# prevents apt from accepting a future package which drops the required ABI.
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  jetson_opencv_version="$(dpkg-query -W -f='${Version}' libopencv 2>/dev/null || true)"
  if [[ -z "${jetson_opencv_version}" ]]; then
    echo "ERROR: JetPack libopencv package is required to build the Jetson package." >&2
    exit 1
  fi
  if [[ ! "${jetson_opencv_version}" =~ ^([0-9]+:)?([0-9]+)[.]([0-9]+)([.+:~-]|$) ]]; then
    echo "ERROR: cannot derive an ABI range from JetPack libopencv version: ${jetson_opencv_version}" >&2
    exit 1
  fi
  jetson_opencv_major="${BASH_REMATCH[2]}"
  jetson_opencv_minor="${BASH_REMATCH[3]}"
  jetson_opencv_upper_version="${jetson_opencv_major}.$((10#${jetson_opencv_minor} + 1))~"
  opencv_mapping_count=0
  for opencv_library in /usr/lib/libopencv_*.so.*; do
    [[ -f "${opencv_library}" && ! -L "${opencv_library}" ]] || continue
    opencv_owner="$(dpkg-query -S "${opencv_library}" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    [[ "${opencv_owner}" == "libopencv" ]] || continue
    opencv_soname="$(patchelf --print-soname "${opencv_library}" 2>/dev/null || true)"
    if [[ "${opencv_soname}" =~ ^(libopencv_.+)[.]so[.]([0-9]+)$ ]]; then
      printf '%s %s libopencv (>= %s), libopencv (<< %s)\n' \
        "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
        "${jetson_opencv_version}" "${jetson_opencv_upper_version}" >> "${SHLIBS_LOCAL}"
      opencv_mapping_count=$((opencv_mapping_count + 1))
    fi
  done
  if [[ "${opencv_mapping_count}" -eq 0 ]]; then
    echo "ERROR: no JetPack libopencv shlibs mappings could be generated." >&2
    exit 1
  fi
fi
CUDA_STUB_DIR="${SHLIBDEPS_WORK_DIR}/cuda-stubs"
mkdir -p "${CUDA_STUB_DIR}"
if [[ -f /usr/local/cuda/lib64/stubs/libcuda.so ]]; then
  ln -s /usr/local/cuda/lib64/stubs/libcuda.so "${CUDA_STUB_DIR}/libcuda.so.1"
elif [[ -f "/usr/local/cuda/targets/${CUDA_TARGET_TRIPLE}/lib/stubs/libcuda.so" ]]; then
  ln -s "/usr/local/cuda/targets/${CUDA_TARGET_TRIPLE}/lib/stubs/libcuda.so" "${CUDA_STUB_DIR}/libcuda.so.1"
fi

declare -a cuda_search_dirs=(
  /usr/local/cuda/lib64
  "/usr/local/cuda/targets/${CUDA_TARGET_TRIPLE}/lib"
  "/usr/lib/${DEB_MULTIARCH}"
  "/usr/lib/${DEB_MULTIARCH}/tegra"
  "/usr/lib/${DEB_MULTIARCH}/libcusparseLt/${EXPECTED_CUDA_SONAME}"
  "/usr/lib/${DEB_MULTIARCH}/nvshmem/${EXPECTED_CUDA_SONAME}"
)
declare -a shlibdeps_cuda_lib_args=()
while IFS= read -r cuda_versioned_dir; do
  cuda_search_dirs+=("${cuda_versioned_dir}")
done < <(find /usr/local -maxdepth 4 -type d -path "/usr/local/cuda-*/targets/${CUDA_TARGET_TRIPLE}/lib" -print | sort -V)
for cuda_dir in "${cuda_search_dirs[@]}"; do
  [[ -d "${cuda_dir}" ]] || continue
  shlibdeps_cuda_lib_args+=("-l${cuda_dir}")
done

for elf in "${dependency_elfs[@]}"; do
  while IFS= read -r needed; do
    provided_by_package=0
    for package_lib_dir in \
      "${STAGING}${INSTALL_PREFIX}/lib" \
      "${STAGING}${INSTALL_PREFIX}/lib/gst-plugins" \
      "${shlibdeps_private_lib_dirs[@]}"; do
      if [[ -e "${package_lib_dir}/${needed}" ]]; then
        provided_by_package=1
        break
      fi
    done
    [[ "${provided_by_package}" -eq 0 ]] || continue

    cuda_library=""
    for cuda_dir in "${cuda_search_dirs[@]}"; do
      if [[ -e "${cuda_dir}/${needed}" ]]; then
        cuda_library="$(readlink -f "${cuda_dir}/${needed}")"
        break
      fi
    done
    [[ -n "${cuda_library}" ]] || continue
    cuda_package="$(dpkg-query -S "${cuda_library}" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    [[ -n "${cuda_package}" ]] || continue
    case "${cuda_package}" in
      cuda-*|libcu*|libnpp*|libnvfatbin*|libnvjitlink*|libnvshmem*) ;;
      *) continue ;;
    esac
    cuda_dependency="${cuda_package}"
    if [[ "${needed}" =~ ^(lib.+)[.]so[.]([0-9]+) ]]; then
      printf '%s %s %s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${cuda_dependency}"
    elif [[ "${needed}" =~ ^(lib.+)[.]so$ ]]; then
      printf '%s 0 %s\n' "${BASH_REMATCH[1]}" "${cuda_dependency}"
    fi
  done < <(patchelf --print-needed "${elf}")
done | sort -u >> "${SHLIBS_LOCAL}"

for elf in "${dependency_elfs[@]}"; do
  while IFS= read -r needed; do
    ds_library=""
    for ds_dir in /opt/nvidia/deepstream/deepstream/lib /opt/nvidia/deepstream/deepstream/lib/gst-plugins; do
      if [[ -e "${ds_dir}/${needed}" ]]; then
        ds_library="${ds_dir}/${needed}"
        break
      fi
    done
    [[ -n "${ds_library}" ]] || continue
    if [[ "${needed}" =~ ^(lib.+)[.]so[.]([0-9]+) ]]; then
      printf '%s %s %s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${DEEPSTREAM_DEPENDS}"
    elif [[ "${needed}" =~ ^(lib.+)[.]so$ ]]; then
      printf '%s 0 %s\n' "${BASH_REMATCH[1]}" "${DEEPSTREAM_DEPENDS}"
    fi
  done < <(patchelf --print-needed "${elf}")
done | sort -u >> "${SHLIBS_LOCAL}"

SHLIBDEPS_LOG="${SHLIBDEPS_WORK_DIR}/warnings.log"
if ! SHLIBDEPS_OUTPUT="$({
  cd "${SHLIBDEPS_WORK_DIR}"
  dpkg-shlibdeps \
    --warnings=7 \
    -O \
    -S"${STAGING}" \
    "${shlibdeps_cuda_lib_args[@]}" \
    -l"${CUDA_STUB_DIR}" \
    -l/opt/nvidia/deepstream/deepstream/lib \
    -l"${STAGING}${INSTALL_PREFIX}/lib" \
    -l"${STAGING}${INSTALL_PREFIX}/lib/gst-plugins" \
    "${shlibdeps_private_lib_args[@]}" \
    "${shlibdeps_elf_args[@]}"
} 2>"${SHLIBDEPS_LOG}")"; then
  echo "ERROR: dpkg-shlibdeps could not resolve package dependencies:" >&2
  cat "${SHLIBDEPS_LOG}" >&2
  exit 1
fi

if [[ -s "${SHLIBDEPS_LOG}" ]]; then
  echo "[make_deb] dpkg-shlibdeps diagnostics:" >&2
  cat "${SHLIBDEPS_LOG}" >&2
fi

SHLIB_DEPENDS="$(printf '%s\n' "${SHLIBDEPS_OUTPUT}" | sed -n 's/^shlibs:Depends=//p')"
if [[ -z "${SHLIB_DEPENDS}" ]]; then
  echo "ERROR: dpkg-shlibdeps returned no package dependencies." >&2
  cat "${SHLIBDEPS_LOG}" >&2
  exit 1
fi
SHLIB_DEPENDS="${SHLIB_DEPENDS//, /,$'\n' }"
# Keep the DeepStream relationship explicit below and avoid emitting it twice
# when dependency-only DeepStream runtime ELFs also resolve to that package.
SHLIB_DEPENDS="$(printf '%s\n' "${SHLIB_DEPENDS}" | sed "/^ ${DEEPSTREAM_PACKAGE//./[.]} /d")"
if [[ "${TARGET_PLATFORM}" == "desktop" ]] &&
   ! grep -Eq '(^|[[:space:]])libmosquitto1([[:space:](,]|$)' <<< "${SHLIB_DEPENDS}"; then
  echo "ERROR: nvtracker dependency analysis did not emit libmosquitto1." >&2
  exit 1
fi
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  expected_cuda_dependency='nvidia-l4t-cuda | libcuda.so.1'
else
  expected_cuda_dependency='libcuda.so.1'
fi
if ! grep -Fq "${expected_cuda_dependency}" <<< "${SHLIB_DEPENDS}" ||
   grep -Eq '(^|[[:space:]])libcuda1([[:space:](,]|$)' <<< "${SHLIB_DEPENDS}"; then
  echo "ERROR: CUDA driver dependency does not match ${TARGET_PLATFORM} policy: ${expected_cuda_dependency}." >&2
  printf '%s\n' "${SHLIB_DEPENDS}" >&2
  exit 1
fi
if [[ "${TARGET_PLATFORM}" == "jetson" ]]; then
  if ! grep -Fq "libopencv (>= ${jetson_opencv_version})" <<< "${SHLIB_DEPENDS}" ||
     ! grep -Fq "libopencv (<< ${jetson_opencv_upper_version})" <<< "${SHLIB_DEPENDS}"; then
    echo "ERROR: Jetson dependency analysis did not emit the required libopencv ABI range." >&2
    printf '%s\n' "${SHLIB_DEPENDS}" >&2
    exit 1
  fi
fi
if grep -Eiq '(^|[[:space:]])(libnccl[^,[:space:]]*|python[^,[:space:]]*|onnxruntime[^,[:space:]]*)' \
    <<< "${SHLIB_DEPENDS}"; then
  echo "ERROR: native HStream unexpectedly acquired a Python, NCCL, or external ONNX Runtime dependency:" >&2
  printf '%s\n' "${SHLIB_DEPENDS}" >&2
  exit 1
fi
rm -rf "${SHLIBDEPS_WORK_DIR}"

INSTALLED_SIZE=$(du -sk "${STAGING}" | awk '{print $1}')
if [[ "${TARGET_PLATFORM}" == "desktop" ]]; then
  PACKAGE_DESCRIPTION="HStream video pipeline application and UI"
  PACKAGE_CONTENTS="Installs the HStream CLI/UI binaries"
  UI_LAUNCH_HELP="Launch the UI with: ${INSTALL_PREFIX}/hstream-ui.sh
 or via the hstream-ui wrapper in /usr/bin/hstream-ui."
  RUNTIME_TOOL_DEPENDS=$',\n hugin-tools,\n enblend'
else
  PACKAGE_DESCRIPTION="HStream video pipeline application for Jetson"
  PACKAGE_CONTENTS="Installs the HStream CLI and pinned native Hugin calibration tools"
  UI_LAUNCH_HELP="The Qt desktop UI is not part of the Jetson package."
  RUNTIME_TOOL_DEPENDS=""
fi
cat > "${STAGING}/DEBIAN/control" <<CONTROL
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Architecture: ${PKG_ARCH}
X-HStream-Source-Commit: ${SOURCE_REVISION}
X-HStream-Source-Epoch: ${SOURCE_EPOCH}
X-HStream-Target-Ubuntu: ${TARGET_UBUNTU}
X-HStream-Target-Platform: ${TARGET_PLATFORM}
Maintainer: Christopher Olivier <cjolivier01@gmail.com>
Installed-Size: ${INSTALLED_SIZE}
Depends: ${SHLIB_DEPENDS},
 ca-certificates,
 ${DEEPSTREAM_DEPENDS},
 ffmpeg,
 gstreamer1.0-plugins-bad,
 gstreamer1.0-nice${RUNTIME_TOOL_DEPENDS}
Description: ${PACKAGE_DESCRIPTION}
 ${PACKAGE_CONTENTS}, private shared libraries,
 GStreamer plugins, configs, native ONNX Runtime, and non-engine pretrained
 assets to ${INSTALL_PREFIX}. Runtime calibration does not launch Python.
 .
 External requirements not otherwise expressed as direct dependencies:
   - NVIDIA CUDA Toolkit ABI ${EXPECTED_CUDA_SONAME} at /usr/local/cuda (provided with DeepStream/JetPack)
   - Configured model frameworks beyond the packaged native stitching runtime
 .
 Launch the CLI with: ${INSTALL_PREFIX}/run.sh [args...]
 or via the hstream-cli wrapper in /usr/bin/hstream-cli.
 ${UI_LAUNCH_HELP}
CONTROL

if find "${STAGING}${INSTALL_PREFIX}" -type f \( -name '*.py' -o -name '*.pyc' -o -name '*.pyo' \) -print -quit \
  | grep -q .; then
  echo "ERROR: Python runtime files unexpectedly entered the native HStream package." >&2
  exit 1
fi
if grep -RIE '(python3|PYTHONPATH|HM_PYTHON|setup_pretrained_assets[.]py|hmlib[.]cli)' \
  "${STAGING}${INSTALL_PREFIX}" --include='*.sh' --include='*.desktop' >/dev/null; then
  echo "ERROR: an installed launcher still refers to Python calibration tooling." >&2
  exit 1
fi
if [[ ! -f "${STAGING}${INSTALL_PREFIX}/lib/libonnxruntime.so.1.23.2" ||
      ! -L "${STAGING}${INSTALL_PREFIX}/lib/libonnxruntime.so.1" ]]; then
  echo "ERROR: the pinned ONNX Runtime library and SONAME link were not staged." >&2
  exit 1
fi
if [[ "$(readlink "${STAGING}${INSTALL_PREFIX}/lib/libonnxruntime.so.1")" != "libonnxruntime.so.1.23.2" ]]; then
  echo "ERROR: the ONNX Runtime SONAME link does not target the pinned runtime." >&2
  exit 1
fi
if [[ "$(patchelf --print-soname "${STAGING}${INSTALL_PREFIX}/lib/libonnxruntime.so.1.23.2")" != \
      "libonnxruntime.so.1" ]]; then
  echo "ERROR: the staged ONNX Runtime library has an unexpected ELF SONAME." >&2
  exit 1
fi
if ! patchelf --print-needed "${STAGING}${INSTALL_PREFIX}/bin/hstream-cli" | grep -qx 'libonnxruntime[.]so[.]1'; then
  echo "ERROR: hstream-cli does not reference the pinned ONNX Runtime SONAME." >&2
  exit 1
fi
for elf in "${package_elfs[@]}"; do
  if patchelf --print-needed "${elf}" 2>/dev/null | grep -qi '^libnccl'; then
    echo "ERROR: an HStream package ELF unexpectedly needs NCCL: ${elf}" >&2
    exit 1
  fi
done

# ---------- build deb ----------
mkdir -p "${OUTPUT_DIR}"
DEB_PATH="${OUTPUT_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"
echo "[make_deb] Building ${DEB_PATH}..."
dpkg-deb --build --root-owner-group "${STAGING}" "${DEB_PATH}"
INSTALLER_PATH="${OUTPUT_DIR}/install-hstream-deb"
install -m 0755 "${TOPDIR}/scripts/install_deb.sh" "${INSTALLER_PATH}"

if [[ "${HSTREAM_CONTAINER_PACKAGE_STAGING:-}" == "1" ]]; then
  echo ""
  echo "Container staging complete: ${DEB_PATH}"
  echo "The Docker wrapper is now copying this package to the host output directory."
else
  echo ""
  echo "Done: ${DEB_PATH}"
  echo ""
  echo "Install with:"
  printf '  sudo %s \\\n' "${INSTALLER_PATH}"
  printf '    --deepstream-deb=%s \\\n' '/path/to/deepstream-9.1_9.1.0-1_amd64.deb'
  echo "    --hstream-deb=${DEB_PATH}"
  echo "  (the installer configures NVIDIA repositories; DeepStream itself remains a local release artifact)"
  echo ""
  echo "Run with:"
  echo "  /opt/hstream/run.sh [args...]"
  echo "  hstream-cli [args...]   (after install)"
  echo "  hstream-ui              (after install)"
  echo "  hstream [args...]        (compatibility wrapper after install)"
fi
