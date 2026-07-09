#!/bin/bash
# Build a self-contained .deb that installs HMStream to /opt/hmstream.
# The installed run.sh launches hmstream-cli without needing the source tree.
#
# Usage:
#   scripts/make_deb.sh [--build] [--version X.Y.Z] [--output-dir DIR]
#
#   --build          Run 'make hmstream-cli hmstream-ui' before packaging (default: skip, use existing artifacts).
#   --version X.Y.Z  Override package version (default: git describe --tags --always).
#   --output-dir DIR Where to write the .deb (default: dist/).
#
# Requirements: patchelf, dpkg-deb (auto-installed from apt if missing).
set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_PREFIX="/opt/hmstream"
PKG_NAME="hmstream"
PKG_ARCH="${PKG_ARCH:-}"

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
  PKG_VERSION="$(git -C "${TOPDIR}" describe --tags --always 2>/dev/null || echo "0.0.0")"
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
if ! dpkg --validate-version "${PKG_VERSION}" >/dev/null 2>&1; then
  echo "ERROR: invalid Debian package version: ${PKG_VERSION}" >&2
  exit 1
fi

# ---------- optional build ----------
if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[make_deb] Building hmstream-cli and hmstream-ui..."
  make -C "${TOPDIR}" hmstream-cli hmstream-ui
fi

# ---------- verify artifacts ----------
HMSTREAM_CLI="${TOPDIR}/bazel-bin/src/apps/pipeline-app/hmstream-cli"
HMSTREAM_UI="${TOPDIR}/bazel-bin/src/apps/hmstream-ui/hmstream-ui"
if [[ ! -f "${HMSTREAM_CLI}" ]]; then
  echo "ERROR: ${HMSTREAM_CLI} not found. Run 'make hmstream-cli' first, or pass --build." >&2
  exit 1
fi
if [[ ! -f "${HMSTREAM_UI}" ]]; then
  echo "ERROR: ${HMSTREAM_UI} not found. Run 'make hmstream-ui' first, or pass --build." >&2
  exit 1
fi
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

# ---------- staging tree ----------
STAGING="${TOPDIR}/dist-staging/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
rm -rf "${STAGING}"

mkdir -p \
  "${STAGING}/DEBIAN" \
  "${STAGING}${INSTALL_PREFIX}/bin" \
  "${STAGING}${INSTALL_PREFIX}/lib/gst-plugins" \
  "${STAGING}${INSTALL_PREFIX}/configs" \
  "${STAGING}${INSTALL_PREFIX}/scripts" \
  "${STAGING}/usr/local/bin"

# ---------- helpers ----------
INSTALL_RPATH="${INSTALL_PREFIX}/lib:/opt/nvidia/deepstream/deepstream/lib:/usr/local/cuda/lib64:/usr/local/cuda/targets/x86_64-linux/lib"

patchelf_rpath() {
  local elf="$1"
  chmod u+w "${elf}"
  patchelf --set-rpath "${INSTALL_RPATH}" "${elf}"
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
cp "${HMSTREAM_UI}" "${STAGING}${INSTALL_PREFIX}/bin/hmstream-ui"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/bin/hmstream-ui"
ln -s hmstream-cli "${STAGING}${INSTALL_PREFIX}/bin/pipeline-app"

# ---------- bundled shared libs (OpenCV etc.) ----------
echo "[make_deb] Collecting bundled shared libs..."
declare -A seen_libs

# Collect from binary first, then from our own gst-plugins
all_elfs=("${HMSTREAM_CLI}" "${HMSTREAM_UI}")
for plugin_dir in "${TOPDIR}/bazel-bin/src/gst-plugins"/*/; do
  for so in "${plugin_dir}"lib*.so; do
    [[ -f "${so}" ]] && all_elfs+=("${so}")
  done
done

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

# ---------- GStreamer plugins ----------
echo "[make_deb] Staging GStreamer plugins..."

# lib/gst-plugins/ — pre-staged NVIDIA/DeepStream plugins
for so in "${TOPDIR}/lib/gst-plugins/"lib*.so; do
  [[ -f "${so}" ]] || continue
  dest="${STAGING}${INSTALL_PREFIX}/lib/gst-plugins/$(basename "${so}")"
  validate_elf_arch "${so}"
  cp "${so}" "${dest}"
  patchelf_rpath "${dest}"
done

# bazel-bin gst-plugins — our custom-built plugins
for plugin_dir in "${TOPDIR}/bazel-bin/src/gst-plugins"/*/; do
  for so in "${plugin_dir}"lib*.so; do
    [[ -f "${so}" ]] || continue
    dest="${STAGING}${INSTALL_PREFIX}/lib/gst-plugins/$(basename "${so}")"
    validate_elf_arch "${so}"
    cp "${so}" "${dest}"
    patchelf_rpath "${dest}"
  done
done

# ---------- YOLO custom inference lib ----------
YOLO_SO="${TOPDIR}/bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
if [[ ! -f "${YOLO_SO}" ]]; then
  echo "[make_deb] ERROR: missing ${YOLO_SO}; run 'make yolo-custom-lib' before packaging." >&2
  exit 1
fi
echo "[make_deb] Staging libnvdsinfer_custom_impl_Yolo.so..."
validate_elf_arch "${YOLO_SO}"
cp "${YOLO_SO}" "${STAGING}${INSTALL_PREFIX}/lib/libnvdsinfer_custom_impl_Yolo.so"
patchelf_rpath "${STAGING}${INSTALL_PREFIX}/lib/libnvdsinfer_custom_impl_Yolo.so"

# ---------- configs ----------
echo "[make_deb] Staging configs..."
cp -r "${TOPDIR}/configs/." "${STAGING}${INSTALL_PREFIX}/configs/"
# Remove the systemd unit files — those belong to a separate package/install step
rm -rf "${STAGING}${INSTALL_PREFIX}/configs/systemd"

# ---------- scripts ----------
echo "[make_deb] Staging scripts..."
cp "${TOPDIR}/scripts/setup_pretrained_assets.py" "${STAGING}${INSTALL_PREFIX}/scripts/"
cp "${TOPDIR}/scripts/export_hm_yolov8_onnx.py" "${STAGING}${INSTALL_PREFIX}/scripts/"

# ---------- installed run.sh ----------
echo "[make_deb] Writing installed run.sh..."
cat > "${STAGING}${INSTALL_PREFIX}/run.sh" <<'RUNSH'
#!/bin/bash
set -euo pipefail

INSTALL_DIR=/opt/hmstream

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

# Bundled libs (OpenCV etc.) are embedded in /opt/hmstream/lib; the binary's
# RPATH already includes this dir, but gst-plugins are dlopen'd at runtime so
# LD_LIBRARY_PATH is still needed for them.
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"

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
  "${PYTHON_BIN:-python3}" "${INSTALL_DIR}/scripts/setup_pretrained_assets.py" "${asset_config_files[@]}"
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
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${INSTALL_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"

exec "${INSTALL_DIR}/bin/hmstream-ui" "$@"
UISH
chmod 755 "${STAGING}${INSTALL_PREFIX}/hmstream-ui.sh"

# ---------- /usr/local/bin symlink via postinst ----------
# (symlink created at install time so it lands outside the staging INSTALL_PREFIX)

# ---------- DEBIAN/control ----------
echo "[make_deb] Writing DEBIAN/control..."
INSTALLED_SIZE=$(du -sk "${STAGING}" | awk '{print $1}')
cat > "${STAGING}/DEBIAN/control" <<CONTROL
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Architecture: ${PKG_ARCH}
Maintainer: hmstream <noreply@hmstream>
Installed-Size: ${INSTALLED_SIZE}
Depends: libgstreamer1.0-0 (>= 1.20),
 libgstreamer-plugins-base1.0-0 (>= 1.20),
 gstreamer1.0-plugins-bad,
 gstreamer1.0-nice,
 libglib2.0-0,
 libgomp1,
 libgl1,
 libglu1-mesa,
 libglew2.2,
 libglfw3,
 libavformat60 | libavformat-extra60,
 libavcodec60 | libavcodec-extra60,
 libavutil58,
 libswresample4,
 libfftw3-3,
 libx11-6,
 libqt6core6t64 | libqt6core6,
 libqt6gui6t64 | libqt6gui6,
 libqt6widgets6t64 | libqt6widgets6,
 libsoup2.4-1,
 libtiff6,
 libpng16-16,
 python3,
 python3-yaml
Description: HMStream video pipeline application and UI
 Installs the HMStream CLI/UI binaries, bundled shared libraries
 (OpenCV 4.13), GStreamer plugins, and configs to ${INSTALL_PREFIX}.
 .
 External requirements (not expressed as Depends):
   - NVIDIA DeepStream (>= 6.3) at /opt/nvidia/deepstream/deepstream
   - NVIDIA CUDA Toolkit (>= 12) at /usr/local/cuda
 .
 Launch the CLI with: ${INSTALL_PREFIX}/run.sh [args...]
 or via the hmstream-cli wrapper in /usr/local/bin/hmstream-cli.
 Launch the UI with: ${INSTALL_PREFIX}/hmstream-ui.sh
 or via the hmstream-ui wrapper in /usr/local/bin/hmstream-ui.
CONTROL

# ---------- DEBIAN/postinst ----------
cat > "${STAGING}/DEBIAN/postinst" <<'POSTINST'
#!/bin/bash
set -e
ln -sfn /opt/hmstream/run.sh /usr/local/bin/hmstream-cli
ln -sfn /opt/hmstream/hmstream-ui.sh /usr/local/bin/hmstream-ui
ln -sfn /opt/hmstream/run.sh /usr/local/bin/hstream
ln -sfn /opt/hmstream/run.sh /usr/local/bin/pipeline-app
POSTINST
chmod 755 "${STAGING}/DEBIAN/postinst"

# ---------- DEBIAN/prerm ----------
cat > "${STAGING}/DEBIAN/prerm" <<'PRERM'
#!/bin/bash
set -e
rm -f /usr/local/bin/hmstream-cli
rm -f /usr/local/bin/hmstream-ui
rm -f /usr/local/bin/hstream
rm -f /usr/local/bin/pipeline-app
PRERM
chmod 755 "${STAGING}/DEBIAN/prerm"

# ---------- build deb ----------
mkdir -p "${OUTPUT_DIR}"
DEB_PATH="${OUTPUT_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"
echo "[make_deb] Building ${DEB_PATH}..."
dpkg-deb --build --root-owner-group "${STAGING}" "${DEB_PATH}"

echo ""
echo "Done: ${DEB_PATH}"
echo ""
echo "Install with:"
echo "  sudo dpkg -i ${DEB_PATH}"
echo ""
echo "Run with:"
echo "  /opt/hmstream/run.sh [args...]"
echo "  hmstream-cli [args...]   (after install)"
echo "  hmstream-ui              (after install)"
echo "  hstream [args...]        (compatibility wrapper after install)"
