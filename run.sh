#!/bin/bash
set -euo pipefail

# Default: one-pass stage 0 run, configuring stitching in-process when control masks are missing.
#
# To run the legacy two-stage stitch/configure flow:
#   ./run.sh --two-stage --game-id=<game_id>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Runtime environment:
# - Our GStreamer plugins are loaded by gst-plugin-scanner (not via Bazel runfiles), so we must
#   expose any non-system shared library deps (e.g. conda OpenCV5) via LD_LIBRARY_PATH.
# - DeepStream plugins can depend on other plugins' shared objects, so include the gst-plugins dir too.
prepend_path() {
  local var_name="$1"
  local dir="$2"
  local cur="${!var_name-}"
  if [ -z "${dir}" ] || [ ! -d "${dir}" ]; then
    return
  fi
  if [ -z "${cur}" ]; then
    export "${var_name}=${dir}"
    return
  fi
  case ":${cur}:" in
    *":${dir}:"*) ;;
    *) export "${var_name}=${dir}:${cur}" ;;
  esac
}

append_path() {
  local var_name="$1"
  local dir="$2"
  local cur="${!var_name-}"
  if [ -z "${dir}" ] || [ ! -d "${dir}" ]; then
    return
  fi
  if [ -z "${cur}" ]; then
    export "${var_name}=${dir}"
    return
  fi
  case ":${cur}:" in
    *":${dir}:"*) ;;
    *) export "${var_name}=${cur}:${dir}" ;;
  esac
}

# Use a repo-local registry so stale blacklists in ~/.cache don't break local plugins.
GST_REGISTRY_DIR="${SCRIPT_DIR}/.cache/gstreamer-1.0"
mkdir -p "${GST_REGISTRY_DIR}"
export GST_REGISTRY="${GST_REGISTRY_DIR}/registry.hstream.$(uname -m).bin"

prepend_path GST_PLUGIN_PATH "${SCRIPT_DIR}/lib/gst-plugins"
prepend_path GST_PLUGIN_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"

prepend_path LD_LIBRARY_PATH "${SCRIPT_DIR}/lib"
prepend_path LD_LIBRARY_PATH "${SCRIPT_DIR}/lib/gst-plugins"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib"
prepend_path LD_LIBRARY_PATH "/opt/nvidia/deepstream/deepstream/lib/gst-plugins"

# Bazel-built GStreamer plugin entrypoints live next to support libraries and test runfiles. Do not add those output
# directories directly to GST_PLUGIN_PATH: GStreamer scans recursively and will try to dlopen test/support .so files as
# plugins. Stage only plugin entrypoint libraries into a clean runtime plugin directory, while keeping their original
# directories on LD_LIBRARY_PATH so dependent support libraries remain visible.
BAZEL_GST_PLUGIN_ROOT="${SCRIPT_DIR}/bazel-bin/src/gst-plugins"
if [ -d "${BAZEL_GST_PLUGIN_ROOT}" ]; then
  BAZEL_GST_RUNTIME_PLUGIN_DIR="${SCRIPT_DIR}/.cache/gst-plugin-path/$(uname -m)"
  mkdir -p "${BAZEL_GST_RUNTIME_PLUGIN_DIR}"
  find "${BAZEL_GST_RUNTIME_PLUGIN_DIR}" -maxdepth 1 -type l -name '*.so' -delete

  while IFS= read -r plugin_so; do
    ln -sfn "${plugin_so}" "${BAZEL_GST_RUNTIME_PLUGIN_DIR}/$(basename "${plugin_so}")"
    prepend_path LD_LIBRARY_PATH "$(dirname "${plugin_so}")"
  done < <(
    find "${BAZEL_GST_PLUGIN_ROOT}" \
      -mindepth 2 \
      -maxdepth 3 \
      -type f \
      \( -name 'libnvdsgst_*.so' -o -name 'libgst*.so' \) \
      ! -path '*/testutils/*' \
      ! -path '*.runfiles/*' \
      -print | sort
  )

  while IFS= read -r lib_dir; do
    prepend_path LD_LIBRARY_PATH "${lib_dir}"
  done < <(
    find "${BAZEL_GST_PLUGIN_ROOT}" \
      -mindepth 2 \
      -maxdepth 4 \
      -type f \
      -name '*.so' \
      ! -path '*.runfiles/*' \
      -printf '%h\n' | sort -u
  )

  prepend_path GST_PLUGIN_PATH "${BAZEL_GST_RUNTIME_PLUGIN_DIR}"
fi

YOLO_CUSTOM_IMPL="${SCRIPT_DIR}/bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
YOLO_CUSTOM_IMPL_LINK="${SCRIPT_DIR}/lib/libnvdsinfer_custom_impl_Yolo.so"
if [ -e "${YOLO_CUSTOM_IMPL}" ]; then
  mkdir -p "${SCRIPT_DIR}/lib"
  if [ ! -e "${YOLO_CUSTOM_IMPL_LINK}" ] || [ -L "${YOLO_CUSTOM_IMPL_LINK}" ]; then
    ln -sfn "../bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so" \
      "${YOLO_CUSTOM_IMPL_LINK}"
  fi
fi
if [ -n "${CONDA_PREFIX:-}" ]; then
  # Conda environments often ship their own GLib/GStreamer stack. Adding `${CONDA_PREFIX}/lib` ahead of system
  # libraries can cause hard-to-debug runtime aborts (e.g. GLib pthread TLS errors) when DeepStream/GStreamer load.
  #
  # Opt-in to using conda's shared libs by setting `HM_USE_CONDA_LD_LIBRARY_PATH=1`.
  if [ "${HM_USE_CONDA_LD_LIBRARY_PATH:-0}" = "1" ]; then
    append_path LD_LIBRARY_PATH "${CONDA_PREFIX}/lib"
  elif [ -f "${CONDA_PREFIX}/lib/libglib-2.0.so.0" ] || [ -f "${CONDA_PREFIX}/lib/libgobject-2.0.so.0" ]; then
    echo "CONDA_PREFIX is set but skipping ${CONDA_PREFIX}/lib in LD_LIBRARY_PATH to avoid GLib/GStreamer conflicts (set HM_USE_CONDA_LD_LIBRARY_PATH=1 to force)"
  else
    append_path LD_LIBRARY_PATH "${CONDA_PREFIX}/lib"
  fi
fi

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
      # run.sh-only flag; do not forward to pipeline-app
      continue
      ;;
    --show)
      if [ "${headless_show_rtsp}" -eq 1 ]; then
        # In headless shells, --show maps to the RTSP sink instead of enabling an EGL render sink.
        continue
      fi
      ;;
  esac
  # hm_run.sh historically supports `-t=N`; glib's GOption expects `-t N`.
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

default_main_sink="RENDER"
if [ "${headless_show_rtsp}" -eq 1 ] && [ "${have_sink_arg}" -eq 0 ]; then
  default_main_sink="RTSP"
  print_rtsp_access_urls 8554 /ds-test "Headless --show requested; streaming RTSP"
elif [ "${have_sink_arg}" -eq 1 ] && args_request_rtsp_sink "${rewritten_args[@]}"; then
  print_rtsp_access_urls 8554 /ds-test "RTSP sink requested; streaming RTSP"
elif [ "${have_sink_arg}" -eq 1 ] && args_request_server_sink "${rewritten_args[@]}"; then
  print_rtsp_access_urls 8554 /ds-test "Server sink requested; RTSP-configured sinks stream RTSP"
elif [ "${has_display}" -eq 0 ]; then
  # Headless / SSH shells often have no X server. Avoid defaulting to a video overlay sink.
  default_main_sink="ENCODE_FILE"
  if [ "${have_sink_arg}" -eq 0 ]; then
    if [ "${ssh_forwarded_display}" -eq 1 ] && [ "${HM_ALLOW_SSH_RENDER:-0}" != "1" ]; then
      echo "DISPLAY=${DISPLAY} looks SSH-forwarded and no sink was specified; defaulting to --enable-sinks=${default_main_sink} (set HM_ALLOW_SSH_RENDER=1 or pass --enable-sinks=RENDER to force render)"
    else
      echo "DISPLAY is not set and no sink was specified; defaulting to --enable-sinks=${default_main_sink} (override with --enable-sinks=RENDER)"
    fi
  fi
fi

sink_args=()
config_args=()
if [ "${one_pass_only}" -eq 1 ]; then
  config_args=(-c configs/ds_hockey_app_config.yaml)
  if [ "${have_sink_arg}" -eq 0 ]; then
    sink_args+=(--enable-sinks="${default_main_sink}")
  fi
else
  config_args=(-c configs/ds_hockey_configure_stitching.yaml -c configs/ds_hockey_app_config.yaml)
  sink_args+=(--enable-sinks=FAKE)
  if [ "${have_sink_arg}" -eq 0 ]; then
    sink_args+=(--enable-sinks="${default_main_sink}")
  fi
fi

hmaudio_enable=1

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
        if [ "${i}" -lt "${#args_ref[@]}" ]; then
          cfg="${args_ref[$i]}"
        fi
        ;;
      -c=*|--config=*)
        cfg="${arg#*=}"
        ;;
    esac
    if [ -n "${cfg}" ]; then
      case "${cfg}" in
        /*) asset_config_files+=("${cfg}") ;;
        *) asset_config_files+=("${SCRIPT_DIR}/${cfg}") ;;
      esac
    fi
  done
}

collect_asset_config_files config_args
collect_asset_config_files rewritten_args
if [ "${#asset_config_files[@]}" -gt 0 ]; then
  "${PYTHON_BIN:-python3}" "${SCRIPT_DIR}/scripts/setup_pretrained_assets.py" "${asset_config_files[@]}"
fi

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

bazel-bin/src/apps/pipeline-app/pipeline-app "${pipeline_args[@]}"
# bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
