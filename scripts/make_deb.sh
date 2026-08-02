#!/bin/bash
# Build an HMStream .deb that installs the application to /opt/hmstream.
# The installed run.sh launches hmstream-cli without needing the source tree.
#
# Usage:
#   scripts/make_deb.sh [--build] [--version X.Y.Z] [--output-dir DIR]
#
#   --build          Run 'make hmstream-cli hmstream-ui yolo-custom-lib hmstream-gst-plugins'
#                    before packaging
#                    (default: skip, use existing artifacts).
#   --version X.Y.Z  Override package version (default: git describe --tags --always).
#   --output-dir DIR Where to write the .deb (default: dist/).
#
# Requirements: patchelf, dpkg-deb, dpkg-shlibdeps, python3-yaml
# (auto-installed from apt if missing).
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_PREFIX="/opt/hmstream"
PKG_NAME="hmstream"
PKG_ARCH="${PKG_ARCH:-}"
DEEPSTREAM_REQUIRED_VERSION="9.1.0-1+resolute2"

# ---------- arg parsing ----------
DO_BUILD=0
PKG_VERSION=""
OUTPUT_DIR="${TOPDIR}/dist"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) DO_BUILD=1 ;;
    --version) PKG_VERSION="$2"; shift ;;
    --version=*) PKG_VERSION="${1#--version=}" ;;
    --output-dir) OUTPUT_DIR="$2"; shift ;;
    --output-dir=*) OUTPUT_DIR="${1#--output-dir=}" ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

if [[ -z "$PKG_VERSION" ]]; then
  GIT_COMMIT_COUNT="$(git -C "${TOPDIR}" rev-list --count HEAD 2>/dev/null || true)"
  GIT_SHORT_HASH="$(git -C "${TOPDIR}" rev-parse --short=7 HEAD 2>/dev/null || true)"
  if [[ -n "${GIT_COMMIT_COUNT}" && -n "${GIT_SHORT_HASH}" ]]; then
    PKG_VERSION="0.0.${GIT_COMMIT_COUNT}+git.${GIT_SHORT_HASH}"
  else
    PKG_VERSION="0.0.0"
  fi
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
if ! command -v python3 &>/dev/null || ! python3 -c 'import yaml' >/dev/null 2>&1; then
  echo "[make_deb] python3-yaml not found; installing via apt..."
  sudo apt-get install -y python3 python3-yaml
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
if [[ "${PKG_ARCH}" != "amd64" ]]; then
  echo "ERROR: HMStream Debian packaging currently supports amd64 only; ${PKG_ARCH} runtime paths are not implemented." >&2
  exit 1
fi
if ! dpkg --validate-version "${PKG_VERSION}" >/dev/null 2>&1; then
  echo "ERROR: invalid Debian package version: ${PKG_VERSION}" >&2
  exit 1
fi

# ---------- optional build ----------
if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[make_deb] Building HMStream apps, YOLO custom inference lib, and GStreamer plugins..."
  make -C "${TOPDIR}" hmstream-cli hmstream-ui yolo-custom-lib hmstream-gst-plugins
fi

# ---------- verify artifacts ----------
HMSTREAM_CLI="${TOPDIR}/bazel-bin/src/apps/pipeline-app/hmstream-cli"
HMSTREAM_UI="${TOPDIR}/bazel-bin/src/apps/hmstream-ui/hmstream-ui"
HMSTREAM_GST_PLUGINS=(
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so"
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-playtracker/libgstplaytracker.so"
  "${TOPDIR}/bazel-bin/src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so"
)
if [[ ! -f "${HMSTREAM_CLI}" ]]; then
  echo "ERROR: ${HMSTREAM_CLI} not found. Run 'make hmstream-cli' first, or pass --build." >&2
  exit 1
fi
if [[ ! -f "${HMSTREAM_UI}" ]]; then
  echo "ERROR: ${HMSTREAM_UI} not found. Run 'make hmstream-ui' first, or pass --build." >&2
  exit 1
fi
for plugin in "${HMSTREAM_GST_PLUGINS[@]}"; do
  if [[ ! -f "${plugin}" ]]; then
    echo "ERROR: ${plugin} not found. Run 'make hmstream-gst-plugins' first, or use 'make deb'." >&2
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
validate_elf_arch "${HMSTREAM_CLI}"
validate_elf_arch "${HMSTREAM_UI}"

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
  "${STAGING}${INSTALL_PREFIX}/hm/xmodels/LightGlue" \
  "${STAGING}${INSTALL_PREFIX}/python" \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/libnccl2" \
  "${STAGING}${INSTALL_PREFIX}/scripts" \
  "${STAGING}/usr/bin"

declare -a package_elfs=()

# ---------- helpers ----------
INSTALL_RPATH="${INSTALL_PREFIX}/lib:/opt/nvidia/deepstream/deepstream/lib:/usr/local/cuda/lib64:/usr/local/cuda/targets/x86_64-linux/lib"

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
          /lib/x86_64-linux-gnu/*) ;;
          /lib/aarch64-linux-gnu/*) ;;
          /usr/lib/x86_64-linux-gnu/*) ;;
          /usr/lib/aarch64-linux-gnu/*) ;;
          /usr/lib/*)  ;;
          /lib/*)      ;;
          /opt/nvidia/*) ;;
          /usr/local/cuda/*) ;;
          *) echo "${lib}" ;;
        esac
      done
}

# Returns true if $1 (a real/resolved path) is a system lib we should NOT bundle.
is_system_lib() {
  local real="$1"
  case "${real}" in
    /lib/x86_64-linux-gnu/*) return 0 ;;
    /lib/aarch64-linux-gnu/*) return 0 ;;
    /usr/lib/x86_64-linux-gnu/*) return 0 ;;
    /usr/lib/aarch64-linux-gnu/*) return 0 ;;
    /usr/lib/*) return 0 ;;
    /lib/*) return 0 ;;
    /opt/nvidia/*) return 0 ;;
    /usr/local/cuda*) return 0 ;;
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
  soname_base="$(basename "${src}")"

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
echo "[make_deb] Staging hmstream binaries..."
cp "${HMSTREAM_CLI}" "${STAGING}${INSTALL_PREFIX}/bin/hmstream-cli"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hmstream-cli"
package_elfs+=("${STAGING}${INSTALL_PREFIX}/bin/hmstream-cli")
cp "${HMSTREAM_UI}" "${STAGING}${INSTALL_PREFIX}/bin/hmstream-ui"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hmstream-ui"
package_elfs+=("${STAGING}${INSTALL_PREFIX}/bin/hmstream-ui")
ln -s hmstream-cli "${STAGING}${INSTALL_PREFIX}/bin/pipeline-app"

# ---------- bundled shared libs (OpenCV etc.) ----------
echo "[make_deb] Collecting bundled shared libs..."
declare -A seen_libs

# Collect from the binaries and the exact HMStream-owned plugin set.
all_elfs=("${HMSTREAM_CLI}" "${HMSTREAM_UI}" "${HMSTREAM_GST_PLUGINS[@]}")

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

# PyTorch needs NCCL, but Ubuntu 24's V100-capable CUDA-12 build and Ubuntu
# 26's CUDA-13 build share the same system package name. Keep the builder's
# target-specific runtime private instead of downgrading/upgrading host NCCL.
NCCL_SONAME_SOURCE="/usr/lib/x86_64-linux-gnu/libnccl.so.2"
if [[ ! -f "${NCCL_SONAME_SOURCE}" ]]; then
  echo "ERROR: target-specific NCCL runtime not found: ${NCCL_SONAME_SOURCE}" >&2
  exit 1
fi
NCCL_REAL_SOURCE="$(readlink -f "${NCCL_SONAME_SOURCE}")"
NCCL_PACKAGE_PATH="${STAGING}${INSTALL_PREFIX}/lib/libnccl.so.2"
validate_elf_arch "${NCCL_REAL_SOURCE}"
install -m 0644 "${NCCL_REAL_SOURCE}" "${NCCL_PACKAGE_PATH}"
package_elfs+=("${NCCL_PACKAGE_PATH}")
install -m 0644 /usr/share/doc/libnccl2/copyright \
  "${STAGING}${INSTALL_PREFIX}/share/licenses/libnccl2/copyright"

# ---------- HMStream GStreamer plugins ----------
echo "[make_deb] Staging GStreamer plugins..."

# DeepStream owns its NVIDIA plugins. Stage only the three plugins built and
# owned by this repository; never pick up stale Bazel outputs opportunistically.
for so in "${HMSTREAM_GST_PLUGINS[@]}"; do
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

# ---------- pretrained assets ----------
echo "[make_deb] Staging declared non-engine pretrained assets..."
asset_manifest="$(mktemp)"
python3 "${TOPDIR}/scripts/setup_pretrained_assets.py" --print-targets "${TOPDIR}/configs/ds_hockey_app_config.yaml" \
  > "${asset_manifest}"
pretrained_root="$(readlink -f "${TOPDIR}/pretrained" 2>/dev/null || true)"
while IFS= read -r asset; do
  [[ -n "${asset}" ]] || continue
  [[ "${asset}" != *.engine ]] || continue
  if [[ ! -f "${asset}" ]]; then
    echo "[make_deb] WARNING: declared pretrained asset missing, not staged: ${asset}" >&2
    continue
  fi
  asset_real="$(readlink -f "${asset}")"
  case "${asset_real}" in
    "${pretrained_root}/"*)
      rel="${asset_real#"${pretrained_root}"/}"
      ;;
    *)
      echo "[make_deb] WARNING: declared pretrained asset outside repo pretrained dir, not staged: ${asset}" >&2
      continue
      ;;
  esac
  dest="${STAGING}${INSTALL_PREFIX}/pretrained/${rel}"
  mkdir -p "$(dirname "${dest}")"
  # Downloaded assets may inherit mkstemp's owner-only mode.  Package runtime
  # data as world-readable so unprivileged hmstream processes can load it.
  install -m 0644 "${asset_real}" "${dest}"
done < "${asset_manifest}"
rm -f "${asset_manifest}"

# ---------- scripts ----------
echo "[make_deb] Staging scripts..."
cp "${TOPDIR}/scripts/setup_pretrained_assets.py" "${STAGING}${INSTALL_PREFIX}/scripts/"
cp "${TOPDIR}/scripts/export_hm_yolov8_onnx.py" "${STAGING}${INSTALL_PREFIX}/scripts/"

# ---------- HockeyMOM Python runtime ----------
HMLIB_SOURCE="${HMLIB_SOURCE:-}"
if [[ -z "${HMLIB_SOURCE}" && -d "${TOPDIR}/../hm/hmlib" ]]; then
  HMLIB_SOURCE="$(readlink -f "${TOPDIR}/../hm")"
fi
if [[ -z "${HMLIB_SOURCE}" || ! -d "${HMLIB_SOURCE}/hmlib" ]]; then
  echo "ERROR: HockeyMOM hmlib source is required. Set HMLIB_SOURCE or provide sibling ../hm." >&2
  exit 1
fi
if [[ ! -d "${HMLIB_SOURCE}/xmodels/LightGlue/lightglue" ]]; then
  echo "ERROR: HockeyMOM's xmodels/LightGlue submodule is required: ${HMLIB_SOURCE}/xmodels/LightGlue" >&2
  exit 1
fi
EXPECTED_HMLIB_REVISION="$(tr -d '[:space:]' < "${TOPDIR}/scripts/hmlib-runtime-revision")"
ACTUAL_HMLIB_REVISION="$(git -C "${HMLIB_SOURCE}" rev-parse HEAD 2>/dev/null || true)"
if [[ "${ACTUAL_HMLIB_REVISION}" != "${EXPECTED_HMLIB_REVISION}" ]]; then
  echo "ERROR: HockeyMOM runtime must be revision ${EXPECTED_HMLIB_REVISION}; found ${ACTUAL_HMLIB_REVISION:-unknown}." >&2
  exit 1
fi
HMLIB_CHANGES="$(git -C "${HMLIB_SOURCE}" status --porcelain -- hmlib xmodels/LightGlue)"
if [[ -n "${HMLIB_CHANGES}" ]]; then
  echo "ERROR: HockeyMOM hmlib/LightGlue runtime paths must be clean before packaging:" >&2
  printf '%s\n' "${HMLIB_CHANGES}" >&2
  exit 1
fi
echo "[make_deb] Staging HockeyMOM Python runtime..."
RUNTIME_RSYNC_EXCLUDES=(
  --exclude='__pycache__/'
  --exclude='*.pyc'
  --exclude='*.pyo'
  --exclude='.pytest_cache/'
  --exclude='.mypy_cache/'
  --exclude='.ruff_cache/'
  --exclude='.cache/'
  --exclude='tests/'
  --exclude='test/'
  # Anchor checkout build artifacts to the transfer root. Runtime packages
  # legitimately contain nested modules such as mmengine/dist/.
  --exclude='/build/'
  --exclude='/dist/'
  --exclude='*.egg-info/'
  --exclude='BUILD'
  --exclude='BUILD.bazel'
)
mkdir -p "${STAGING}${INSTALL_PREFIX}/hm/hmlib" "${STAGING}${INSTALL_PREFIX}/hm/xmodels/LightGlue/lightglue"
rsync -a --prune-empty-dirs "${RUNTIME_RSYNC_EXCLUDES[@]}" \
  "${HMLIB_SOURCE}/hmlib/" "${STAGING}${INSTALL_PREFIX}/hm/hmlib/"
rsync -a --prune-empty-dirs "${RUNTIME_RSYNC_EXCLUDES[@]}" \
  "${HMLIB_SOURCE}/xmodels/LightGlue/lightglue/" \
  "${STAGING}${INSTALL_PREFIX}/hm/xmodels/LightGlue/lightglue/"
# Developer checkouts contain Bazel-only dangling links and mixed umask modes.
# They are not package runtime inputs; normalize what remains for deterministic
# unprivileged use.
find "${STAGING}${INSTALL_PREFIX}/hm" -xtype l -delete
find "${STAGING}${INSTALL_PREFIX}/hm" -type d -exec chmod 0755 {} +
find "${STAGING}${INSTALL_PREFIX}/hm" -type f -exec chmod 0644 {} +
if [[ -n "${HMSTREAM_PYTHON_DEPS:-}" ]]; then
  if [[ ! -d "${HMSTREAM_PYTHON_DEPS}" ]]; then
    echo "ERROR: HMSTREAM_PYTHON_DEPS is not a directory: ${HMSTREAM_PYTHON_DEPS}" >&2
    exit 1
  fi
  rsync -a --prune-empty-dirs "${RUNTIME_RSYNC_EXCLUDES[@]}" \
    "${HMSTREAM_PYTHON_DEPS}/" "${STAGING}${INSTALL_PREFIX}/python/"
fi

# Python 3.14 removed pkgutil.find_loader().  The pinned MMEngine fork still
# uses it to detect MMCV's native extension, so make the staged runtime work on
# both Ubuntu 24.04's Python 3.12 and Ubuntu 26.04's Python 3.14.
MMENGINE_DL_MISC="${STAGING}${INSTALL_PREFIX}/python/mmengine/utils/dl_utils/misc.py"
if [[ -f "${MMENGINE_DL_MISC}" ]] && grep -q "pkgutil.find_loader('mmcv._ext')" "${MMENGINE_DL_MISC}"; then
  sed -i \
    -e 's/^import pkgutil$/import importlib.util/' \
    -e "s/pkgutil.find_loader('mmcv\._ext')/importlib.util.find_spec('mmcv._ext')/" \
    "${MMENGINE_DL_MISC}"
fi
if [[ -f "${MMENGINE_DL_MISC}" ]]; then
  PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="${STAGING}${INSTALL_PREFIX}/python" \
    python3 -c 'from mmengine.utils.dl_utils.misc import mmcv_full_available; assert mmcv_full_available()'
fi
PYTHONDONTWRITEBYTECODE=1 \
PYTHONPATH="${STAGING}${INSTALL_PREFIX}/python:${STAGING}${INSTALL_PREFIX}/hm:${STAGING}${INSTALL_PREFIX}/hm/xmodels/LightGlue" \
  python3 -c 'import hmlib.cli.create_control_points'

# ---------- installed run.sh ----------
echo "[make_deb] Writing installed run.sh..."
cat > "${STAGING}${INSTALL_PREFIX}/run.sh" <<'RUNSH'
#!/bin/bash
set -euo pipefail

INSTALL_DIR=/opt/hmstream

# DeepStream 9.1's legacy nvstreammux rejects native 8K input buffers. The
# replacement mux handles the source resolution used for stitching. Preserve
# an explicit caller override for diagnostics and older DeepStream releases.
export USE_NEW_NVSTREAMMUX="${USE_NEW_NVSTREAMMUX:-yes}"

# Use the pinned bundled hmlib runtime unless the caller explicitly overrides
# HMLIB_ROOT/HM_ROOT.
if [ -z "${HMLIB_ROOT:-}" ] && [ -z "${HM_ROOT:-}" ] && [ -d "${INSTALL_DIR}/hm/hmlib" ]; then
  export HMLIB_ROOT="${INSTALL_DIR}/hm"
fi
hm_runtime_root="${HMLIB_ROOT:-${HM_ROOT:-}}"
if [ -z "${HM_CONFIG_ROOT:-}" ] && [ -n "${hm_runtime_root}" ] && [ -f "${hm_runtime_root}/hmlib/config/baseline.yaml" ]; then
  export HM_CONFIG_ROOT="${hm_runtime_root}/hmlib/config"
fi

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

# The bundled native modules match the distribution's system Python ABI. Keep
# active Conda/virtualenv console scripts and libraries out of the default
# packaged runtime; setting HM_PYTHON or PYTHON_BIN is an explicit opt-in.
if [ -z "${HM_PYTHON:-}" ] && [ -z "${PYTHON_BIN:-}" ]; then
  export HM_PYTHON=/usr/bin/python3
  export PYTHON_BIN=/usr/bin/python3
  unset CONDA_PREFIX CONDA_DEFAULT_ENV VIRTUAL_ENV
  export PATH=/usr/bin:/bin:/usr/sbin:/sbin
else
  export HM_PYTHON="${HM_PYTHON:-${PYTHON_BIN}}"
  export PYTHON_BIN="${PYTHON_BIN:-${HM_PYTHON}}"
  export PATH="/usr/bin:/bin:${PATH:-}"
fi

# Per-user registry so the read-only install dir stays clean.
GST_REGISTRY_DIR="${HOME}/.cache/gstreamer-1.0"
mkdir -p "${GST_REGISTRY_DIR}"
export GST_REGISTRY="${GST_REGISTRY_DIR}/registry.hstream.$(uname -m).bin"

# The installed launcher runs from /opt/hmstream so packaged config-relative
# paths resolve correctly. Keep generated video outputs in a per-user writable
# location instead of trying to create them below the read-only install tree.
export HM_OUTPUT_WORK_DIR="${HM_OUTPUT_WORK_DIR:-${XDG_STATE_HOME:-${HOME}/.local/state}/hmstream/output_workdirs}"
mkdir -p "${HM_OUTPUT_WORK_DIR}"

prepend_path GST_PLUGIN_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"

# Bundled libs (OpenCV etc.) are embedded in /opt/hmstream/lib; the binary's
# RPATH already includes this dir, but gst-plugins are dlopen'd at runtime so
# LD_LIBRARY_PATH is still needed for them.
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/12"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/libcusparseLt/12"
for python_private_lib in "${INSTALL_DIR}"/python/nvidia/*/lib "${INSTALL_DIR}"/python/*.libs; do
  prepend_path LD_LIBRARY_PATH "${python_private_lib}"
done
prepend_path PYTHONPATH "${INSTALL_DIR}/python"

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

# Collect any -c/--config arguments from user-provided args so pretrained asset
# setup runs for them too.
asset_config_files=()
collect_asset_config_files() {
  local -n args_ref="$1"
  local arg cfg i
  for ((i = 0; i < ${#args_ref[@]}; i++)); do
    arg="${args_ref[$i]}"
    cfg=""
    case "${arg}" in
      -c|--config)
        i=$((i + 1))
        if [ "${i}" -lt "${#args_ref[@]}" ]; then cfg="${args_ref[$i]}"; fi
        ;;
      -c=*|--config=*)
        cfg="${arg#*=}"
        ;;
    esac
    if [ -n "${cfg}" ]; then
      case "${cfg}" in
        /*) asset_config_files+=("${cfg}") ;;
        *) asset_config_files+=("${INSTALL_DIR}/${cfg}") ;;
      esac
    fi
  done
}

collect_asset_config_files config_args
collect_asset_config_files rewritten_args
if [ "${#asset_config_files[@]}" -gt 0 ]; then
  asset_setup_python() {
    local candidates=()
    local candidate
    if [ -n "${PYTHON_BIN:-}" ]; then candidates+=("${PYTHON_BIN}"); fi
    if [ -n "${CONDA_PREFIX:-}" ]; then candidates+=("${CONDA_PREFIX}/bin/python3"); fi
    if [ -n "${VIRTUAL_ENV:-}" ]; then candidates+=("${VIRTUAL_ENV}/bin/python3"); fi
    candidates+=("${HOME}/miniforge3/envs/ubuntu/bin/python3")
    candidates+=("${HOME}/miniconda3/envs/ubuntu/bin/python3")
    candidates+=("${HOME}/.conda/envs/ubuntu/bin/python3")
    candidates+=("python3")
    for candidate in "${candidates[@]}"; do
      if command -v "${candidate}" >/dev/null 2>&1 &&
        "${candidate}" -c 'import yaml' >/dev/null 2>&1; then
        printf '%s\n' "${candidate}"
        return 0
      fi
    done
    printf '%s\n' "${PYTHON_BIN:-python3}"
  }
  asset_python="$(asset_setup_python)"
  echo "checking pretrained assets with ${asset_python}"
  "${asset_python}" "${INSTALL_DIR}/scripts/setup_pretrained_assets.py" "${asset_config_files[@]}"
fi

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

exec "${INSTALL_DIR}/bin/hmstream-cli" "${pipeline_args[@]}"
RUNSH
chmod 755 "${STAGING}${INSTALL_PREFIX}/run.sh"

# ---------- installed UI wrapper ----------
cat > "${STAGING}${INSTALL_PREFIX}/hmstream-ui.sh" <<'UISH'
#!/bin/bash
set -euo pipefail

INSTALL_DIR=/opt/hmstream

export USE_NEW_NVSTREAMMUX="${USE_NEW_NVSTREAMMUX:-yes}"

if [ -z "${HMLIB_ROOT:-}" ] && [ -z "${HM_ROOT:-}" ] && [ -d "${INSTALL_DIR}/hm/hmlib" ]; then
  export HMLIB_ROOT="${INSTALL_DIR}/hm"
fi
hm_runtime_root="${HMLIB_ROOT:-${HM_ROOT:-}}"
if [ -z "${HM_CONFIG_ROOT:-}" ] && [ -n "${hm_runtime_root}" ] && [ -f "${hm_runtime_root}/hmlib/config/baseline.yaml" ]; then
  export HM_CONFIG_ROOT="${hm_runtime_root}/hmlib/config"
fi

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

if [ -z "${HM_PYTHON:-}" ] && [ -z "${PYTHON_BIN:-}" ]; then
  export HM_PYTHON=/usr/bin/python3
  export PYTHON_BIN=/usr/bin/python3
  unset CONDA_PREFIX CONDA_DEFAULT_ENV VIRTUAL_ENV
  export PATH=/usr/bin:/bin:/usr/sbin:/sbin
else
  export HM_PYTHON="${HM_PYTHON:-${PYTHON_BIN}}"
  export PYTHON_BIN="${PYTHON_BIN:-${HM_PYTHON}}"
  export PATH="/usr/bin:/bin:${PATH:-}"
fi

# Archive/ENCODE_FILE runs launched by the installed UI need a writable
# working directory just like direct CLI runs.
export HM_OUTPUT_WORK_DIR="${HM_OUTPUT_WORK_DIR:-${XDG_STATE_HOME:-${HOME}/.local/state}/hmstream/output_workdirs}"
mkdir -p "${HM_OUTPUT_WORK_DIR}"

prepend_path GST_PLUGIN_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/13"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/nvshmem/12"
prepend_path LD_LIBRARY_PATH "/usr/lib/x86_64-linux-gnu/libcusparseLt/12"
for python_private_lib in "${INSTALL_DIR}"/python/nvidia/*/lib "${INSTALL_DIR}"/python/*.libs; do
  prepend_path LD_LIBRARY_PATH "${python_private_lib}"
done
prepend_path PYTHONPATH "${INSTALL_DIR}/python"

exec "${INSTALL_DIR}/bin/hmstream-ui" "$@"
UISH
chmod 755 "${STAGING}${INSTALL_PREFIX}/hmstream-ui.sh"

# ---------- package-owned command wrappers ----------
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/hmstream-cli"
ln -s "${INSTALL_PREFIX}/hmstream-ui.sh" "${STAGING}/usr/bin/hmstream-ui"
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/hstream"
ln -s "${INSTALL_PREFIX}/run.sh" "${STAGING}/usr/bin/pipeline-app"

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
Description: HMStream dependency resolution metadata
SHLIBDEPS_CONTROL

declare -a shlibdeps_elf_args=()
declare -a shlibdeps_private_lib_args=()
declare -a shlibdeps_private_lib_dirs=()
for elf in "${package_elfs[@]}"; do
  shlibdeps_elf_args+=("-e${elf}")
done

# The target-OS builds bundle Python extension modules, including PyTorch and
# full MMCV ops. Include those ELF files in dependency resolution as well.
if [[ -n "${HMSTREAM_PYTHON_DEPS:-}" ]]; then
  while IFS= read -r -d '' python_elf; do
    if file -Lb "${python_elf}" | grep -q '^ELF '; then
      validate_elf_arch "${python_elf}"
      python_rpath="$(patchelf --print-rpath "${python_elf}" 2>/dev/null || true)"
      if [[ -n "${python_rpath}" ]]; then
        sanitized_rpath="$(printf '%s' "${python_rpath}" | tr ':' '\n' \
          | sed -E '\#^/(tmp|__w)/#d' | paste -sd: -)"
        if [[ "${sanitized_rpath}" != "${python_rpath}" ]]; then
          chmod u+w "${python_elf}"
          if [[ -n "${sanitized_rpath}" ]]; then
            patchelf --set-rpath "${sanitized_rpath}" "${python_elf}"
          else
            patchelf --remove-rpath "${python_elf}"
          fi
        fi
      fi
      package_elfs+=("${python_elf}")
      shlibdeps_elf_args+=("-e${python_elf}")
    fi
  done < <(find "${STAGING}${INSTALL_PREFIX}/python" -type f -print0)

  # Binary wheels commonly keep private dependencies in sibling directories
  # such as shapely.libs or under nvidia/<component>/lib. dpkg-shlibdeps does
  # not follow wheel-specific loader paths, so expose each private directory
  # while resolving the staged Python ELF graph.
  while IFS= read -r -d '' private_lib_dir; do
    shlibdeps_private_lib_dirs+=("${private_lib_dir}")
    shlibdeps_private_lib_args+=("-l${private_lib_dir}")
  done < <(find "${STAGING}${INSTALL_PREFIX}/python" -type d \
    \( -name '*.libs' -o -path '*/nvidia/*/lib' \) -print0)
fi

# nvtracker is provided by the DeepStream Debian package, but its low-level
# implementation is dlopen'd and therefore its CUDA/MQTT dependencies are not
# visible in HMStream's own ELF graph. Resolve that runtime graph without
# copying any DeepStream-owned files into this package.
DEEPSTREAM_TRACKER_RUNTIME="/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so"
if [[ ! -f "${DEEPSTREAM_TRACKER_RUNTIME}" ]]; then
  echo "ERROR: DeepStream nvtracker runtime not found: ${DEEPSTREAM_TRACKER_RUNTIME}" >&2
  exit 1
fi
validate_elf_arch "${DEEPSTREAM_TRACKER_RUNTIME}"
dependency_elfs=("${package_elfs[@]}" "${DEEPSTREAM_TRACKER_RUNTIME}")
shlibdeps_elf_args+=("-e${DEEPSTREAM_TRACKER_RUNTIME}")

# NVIDIA does not ship Debian shlibs metadata for its unversioned DeepStream
# libraries or most CUDA toolkit libraries. Generate metadata from the packages
# that own the resolved CUDA files; TensorRT retains its package-provided
# metadata. libcuda is supplied by the host driver, so make its toolkit stub
# discoverable and let --ignore-missing-info omit a distro-specific driver
# package dependency.
SHLIBS_LOCAL="${SHLIBDEPS_WORK_DIR}/debian/shlibs.local"
: > "${SHLIBS_LOCAL}"
CUDA_STUB_DIR="${SHLIBDEPS_WORK_DIR}/cuda-stubs"
mkdir -p "${CUDA_STUB_DIR}"
if [[ -f /usr/local/cuda/lib64/stubs/libcuda.so ]]; then
  ln -s /usr/local/cuda/lib64/stubs/libcuda.so "${CUDA_STUB_DIR}/libcuda.so.1"
elif [[ -f /usr/local/cuda/targets/x86_64-linux/lib/stubs/libcuda.so ]]; then
  ln -s /usr/local/cuda/targets/x86_64-linux/lib/stubs/libcuda.so "${CUDA_STUB_DIR}/libcuda.so.1"
fi

declare -a cuda_search_dirs=(
  /usr/local/cuda/lib64
  /usr/local/cuda/targets/x86_64-linux/lib
  /usr/lib/x86_64-linux-gnu
  /usr/lib/x86_64-linux-gnu/libcusparseLt/12
  /usr/lib/x86_64-linux-gnu/nvshmem/12
  /usr/lib/x86_64-linux-gnu/nvshmem/13
)
declare -a shlibdeps_cuda_lib_args=()
while IFS= read -r cuda_versioned_dir; do
  cuda_search_dirs+=("${cuda_versioned_dir}")
done < <(find /usr/local -maxdepth 4 -type d -path '/usr/local/cuda-*/targets/x86_64-linux/lib' -print | sort -V)
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
      "${STAGING}${INSTALL_PREFIX}/python/torch/lib" \
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
    cuda_package="$(dpkg-query -S "${cuda_library}" 2>/dev/null | head -n1 | cut -d: -f1)"
    [[ -n "${cuda_package}" ]] || continue
    case "${cuda_package}" in
      cuda-*|libcu*|libnccl*|libnpp*|libnvfatbin*|libnvjitlink*|libnvshmem*) ;;
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
      printf '%s %s deepstream-9.1 (= %s)\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${DEEPSTREAM_REQUIRED_VERSION}"
    elif [[ "${needed}" =~ ^(lib.+)[.]so$ ]]; then
      printf '%s 0 deepstream-9.1 (= %s)\n' "${BASH_REMATCH[1]}" "${DEEPSTREAM_REQUIRED_VERSION}"
    fi
  done < <(patchelf --print-needed "${elf}")
done | sort -u >> "${SHLIBS_LOCAL}"

SHLIBDEPS_LOG="${SHLIBDEPS_WORK_DIR}/warnings.log"
if ! SHLIBDEPS_OUTPUT="$({
  cd "${SHLIBDEPS_WORK_DIR}"
  dpkg-shlibdeps \
    --ignore-missing-info \
    --warnings=0 \
    -O \
    -S"${STAGING}" \
    "${shlibdeps_cuda_lib_args[@]}" \
    -l"${CUDA_STUB_DIR}" \
    -l/opt/nvidia/deepstream/deepstream/lib \
    -l"${STAGING}${INSTALL_PREFIX}/lib" \
    -l"${STAGING}${INSTALL_PREFIX}/lib/gst-plugins" \
    -l"${STAGING}${INSTALL_PREFIX}/python/torch/lib" \
    "${shlibdeps_private_lib_args[@]}" \
    "${shlibdeps_elf_args[@]}"
} 2>"${SHLIBDEPS_LOG}")"; then
  echo "ERROR: dpkg-shlibdeps could not resolve package dependencies:" >&2
  cat "${SHLIBDEPS_LOG}" >&2
  exit 1
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
SHLIB_DEPENDS="$(printf '%s\n' "${SHLIB_DEPENDS}" | sed '/^ deepstream-9[.]1 /d')"
rm -rf "${SHLIBDEPS_WORK_DIR}"

INSTALLED_SIZE=$(du -sk "${STAGING}" | awk '{print $1}')
cat > "${STAGING}/DEBIAN/control" <<CONTROL
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Architecture: ${PKG_ARCH}
Maintainer: Christopher Olivier <cjolivier01@gmail.com>
Installed-Size: ${INSTALLED_SIZE}
Depends: ${SHLIB_DEPENDS},
 deepstream-9.1 (= ${DEEPSTREAM_REQUIRED_VERSION}),
 ffmpeg,
 gstreamer1.0-plugins-bad,
 gstreamer1.0-nice,
 hugin-tools,
 enblend,
 python3,
 python3-matplotlib,
 python3-numpy,
 python3-opencv,
 python3-pil,
 python3-scipy,
 python3-tifffile,
 python3-yaml
Description: HMStream video pipeline application and UI
 Installs the HMStream CLI/UI binaries, private shared libraries,
 GStreamer plugins, configs, and non-engine pretrained
 assets to ${INSTALL_PREFIX}.
 .
 External requirements not otherwise expressed as direct dependencies:
   - NVIDIA CUDA Toolkit (>= 12) at /usr/local/cuda (pulled transitively by DeepStream)
   - Configured model frameworks beyond the packaged stitching runtime
   - The core hmlib, LightGlue, PyTorch, torchvision, full MMCV ops,
     MMDetection, MMEngine, Kornia, and ffmpegio runtimes are included by
     target-OS container builds
 .
 Launch the CLI with: ${INSTALL_PREFIX}/run.sh [args...]
 or via the hmstream-cli wrapper in /usr/bin/hmstream-cli.
 Launch the UI with: ${INSTALL_PREFIX}/hmstream-ui.sh
 or via the hmstream-ui wrapper in /usr/bin/hmstream-ui.
CONTROL

# ---------- build deb ----------
mkdir -p "${OUTPUT_DIR}"
DEB_PATH="${OUTPUT_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"
echo "[make_deb] Building ${DEB_PATH}..."
dpkg-deb --build --root-owner-group "${STAGING}" "${DEB_PATH}"
INSTALLER_PATH="${OUTPUT_DIR}/install-hmstream-deb"
install -m 0755 "${TOPDIR}/scripts/install_deb.sh" "${INSTALLER_PATH}"

echo ""
echo "Done: ${DEB_PATH}"
echo ""
echo "Install with:"
printf '  sudo %s \\\n' "${INSTALLER_PATH}"
printf '    --deepstream-deb=%s \\\n' '/path/to/deepstream-9.1_9.1.0-1+resolute2_amd64.deb'
echo "    --hmstream-deb=${DEB_PATH}"
echo "  (the installer configures NVIDIA repositories; DeepStream itself remains a local release artifact)"
echo ""
echo "Run with:"
echo "  /opt/hmstream/run.sh [args...]"
echo "  hmstream-cli [args...]   (after install)"
echo "  hmstream-ui              (after install)"
echo "  hstream [args...]        (compatibility wrapper after install)"
