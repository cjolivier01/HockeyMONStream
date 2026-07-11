#!/bin/bash
set -euo pipefail

# Default: one-pass stage 0 run, configuring stitching in-process when control masks are missing.
#
# To run the legacy two-stage stitch/configure flow:
#   ./run.sh --two-stage --game-id=<game_id>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

show_help() {
  cat <<'EOF'
Usage:
  ./run.sh --game-id=<game_id> [wrapper options] [pipeline-app options]
  ./hm_run.sh --game-id=<game_id> [wrapper options] [pipeline-app options]

Common:
  -g, --game-id ID                     Game directory ID under $HM_GAME_DIR or ~/Videos
  -t N, -t=N, --time-limit=N           Stop after N seconds of video
  -k, --enable-sinks SINKS             Comma-separated sinks: FAKE, RENDER, ENCODE_FILE, RTSP, WEBRTC
  --show                               Render when display is available; RTSP preview when headless
  -c, --config FILE                    Additional pipeline-app config file
  --options key=value                  Override pipeline config; repeatable

Pipeline staging:
  --one-pass-only, --stage0-only       Default: configure stitching in-process during stage 0
  --two-stage, --configure-first       Legacy two-stage flow: configure stitching first, then run stage 0

Model precision:
  --models-int8, --int8-models,
  --quant-int8                         Use INT8 model config. Requires existing calibrated INT8 engine and
                                       non-empty calibration table.
  --models-int8-calibrate,
  --int8-calibrate, --calibrate-int8   Extract calibration frames, build a TensorRT INT8 calibration table and
                                       engine offline, then run from timestamp zero.
  --int8-calib-frames N                Number of frames to sample for INT8 calibration. Default: 64.
  --int8-calib-batch-size N            INT8 calibration batch size. Default: 2.
  --int8-calib-start-seconds S         Start offset for calibration frame extraction. Default: 0.
  --models-bf16, --bf16-models         Use a prebuilt BF16 TensorRT detector engine.
  --models-bf16-build, --bf16-build    Build the BF16 detector engine offline, then run from timestamp zero.

Stitcher performance:
  --stitcher-compute fp32|fp16         Stitcher compute precision. Aliases: float32, float16, half.
  --stitcher-compute-precision VALUE   Same as --stitcher-compute.
  --stitcher-minimize-blend,
  --minimize-blend                     Use the faster minimized blend path.

Examples:
  ./run.sh --game-id=tv-12-1-r2 --enable-sinks=FAKE -t=10
  ./run.sh --game-id=tv-12-1-r2 --stitcher-compute=fp16 --stitcher-minimize-blend --enable-sinks=FAKE -t=10
  ./run.sh --game-id=tv-12-1-r2 --models-int8-calibrate --int8-calib-frames=64 --enable-sinks=FAKE -t=10

Related:
  scripts/benchmark_model_precision.py --game-id=tv-12-1-r2 -t=10 --variants=quick --stitcher-minimize-blend

Notes:
  Game directories default to ~/Videos/<game_id>. Set HM_GAME_DIR to override the games root.
  Unrecognized arguments are forwarded to pipeline-app.
EOF
}

for arg in "$@"; do
  case "${arg}" in
    -h|--help|help)
      show_help
      exit 0
      ;;
  esac
done

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
    plugin_so="$(readlink -f "${plugin_so}")"
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
    lib_dir="$(readlink -f "${lib_dir}")"
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

  BAZEL_BIN_REAL="$(readlink -f "${SCRIPT_DIR}/bazel-bin" 2>/dev/null || true)"
  if [ -n "${BAZEL_BIN_REAL}" ] && [ -d "${BAZEL_BIN_REAL}" ]; then
    while IFS= read -r solib_dir; do
      solib_dir="$(readlink -f "${solib_dir}")"
      prepend_path LD_LIBRARY_PATH "${solib_dir}"
    done < <(
      find "${BAZEL_BIN_REAL}" \
        -path '*/_solib_*/*' \
        -type d \
        -printf '%p\n' | sort -u
    )
  fi

  prepend_path GST_PLUGIN_PATH "${BAZEL_GST_RUNTIME_PLUGIN_DIR}"
fi

YOLO_CUSTOM_IMPL="${SCRIPT_DIR}/bazel-bin/src/libs/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so"
YOLO_CUSTOM_IMPL_LINK="${SCRIPT_DIR}/lib/libnvdsinfer_custom_impl_Yolo.so"
if [ -e "${YOLO_CUSTOM_IMPL}" ]; then
  YOLO_CUSTOM_IMPL="$(readlink -f "${YOLO_CUSTOM_IMPL}")"
  mkdir -p "${SCRIPT_DIR}/lib"
  if [ ! -e "${YOLO_CUSTOM_IMPL_LINK}" ] || [ -L "${YOLO_CUSTOM_IMPL_LINK}" ]; then
    ln -sfn "${YOLO_CUSTOM_IMPL}" "${YOLO_CUSTOM_IMPL_LINK}"
  fi
fi

PIPELINE_APP_BIN="${SCRIPT_DIR}/bazel-bin/src/apps/pipeline-app/hmstream-cli"
if [ -x "${PIPELINE_APP_BIN}" ]; then
  PIPELINE_APP_BIN="$(readlink -f "${PIPELINE_APP_BIN}")"
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
extra_options=()
models_int8=0
models_int8_calibrate=0
models_bf16=0
models_bf16_build=0
int8_calib_frames=64
int8_calib_batch_size=2
int8_calib_start_seconds=0
int8_asset_config_file=""
int8_engine_file=""
int8_calib_table=""
bf16_asset_config_file=""
bf16_engine_file=""
stitcher_compute_precision=""
stitcher_minimize_blend=0
game_id=""
args=("$@")

next_arg_is_sink_value=0
for ((i = 0; i < ${#args[@]}; i++)); do
  arg="${args[$i]}"
  if [ "${next_arg_is_sink_value}" -eq 1 ]; then
    next_arg_is_sink_value=0
    continue
  fi
  case "$arg" in
    --one-pass-only|--stage0-only) one_pass_only=1 ;;
    --two-stage|--configure-first) one_pass_only=0 ;;
    --models-int8|--int8-models|--quant-int8)
      models_int8=1
      ;;
    --models-int8-calibrate|--int8-calibrate|--calibrate-int8)
      models_int8=1
      models_int8_calibrate=1
      ;;
    --models-bf16|--bf16-models)
      models_bf16=1
      ;;
    --models-bf16-build|--bf16-build)
      models_bf16=1
      models_bf16_build=1
      ;;
    --int8-calib-frames=*)
      int8_calib_frames="${arg#*=}"
      ;;
    --int8-calib-frames)
      if [ "$((i + 1))" -lt "${#args[@]}" ]; then
        int8_calib_frames="${args[$((i + 1))]}"
      fi
      ;;
    --int8-calib-batch-size=*)
      int8_calib_batch_size="${arg#*=}"
      ;;
    --int8-calib-batch-size)
      if [ "$((i + 1))" -lt "${#args[@]}" ]; then
        int8_calib_batch_size="${args[$((i + 1))]}"
      fi
      ;;
    --int8-calib-start-seconds=*)
      int8_calib_start_seconds="${arg#*=}"
      ;;
    --int8-calib-start-seconds)
      if [ "$((i + 1))" -lt "${#args[@]}" ]; then
        int8_calib_start_seconds="${args[$((i + 1))]}"
      fi
      ;;
    --stitcher-compute=*|--stitcher-compute-precision=*)
      stitcher_compute_precision="${arg#*=}"
      ;;
    --stitcher-compute|--stitcher-compute-precision)
      if [ "$((i + 1))" -lt "${#args[@]}" ]; then
        stitcher_compute_precision="${args[$((i + 1))]}"
      fi
      ;;
    --stitcher-minimize-blend|--minimize-blend)
      stitcher_minimize_blend=1
      ;;
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
    --game-id=*)
      game_id="${arg#*=}"
      ;;
    --game-id|-g)
      if [ "$((i + 1))" -lt "${#args[@]}" ]; then
        game_id="${args[$((i + 1))]}"
      fi
      ;;
    -g=*)
      game_id="${arg#*=}"
      ;;
  esac
done

case "${int8_calib_frames}" in
  ''|*[!0-9]*)
    echo "Unsupported --int8-calib-frames value: ${int8_calib_frames} (expected a positive integer)"
    exit 2
    ;;
esac
if [ "${int8_calib_frames}" -lt 1 ]; then
  echo "--int8-calib-frames must be at least 1"
  exit 2
fi
case "${int8_calib_batch_size}" in
  ''|*[!0-9]*)
    echo "Unsupported --int8-calib-batch-size value: ${int8_calib_batch_size} (expected a positive integer)"
    exit 2
    ;;
esac
if [ "${int8_calib_batch_size}" -lt 1 ]; then
  echo "--int8-calib-batch-size must be at least 1"
  exit 2
fi
if [ $((int8_calib_frames % int8_calib_batch_size)) -ne 0 ]; then
  echo "--int8-calib-frames must be divisible by --int8-calib-batch-size so calibration does not drop a partial batch"
  exit 2
fi
case "${int8_calib_start_seconds}" in
  ''|*[!0-9.]*)
    echo "Unsupported --int8-calib-start-seconds value: ${int8_calib_start_seconds} (expected seconds)"
    exit 2
    ;;
esac

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

skip_next_rewritten_arg=0
for ((i = 0; i < ${#args[@]}; i++)); do
  arg="${args[$i]}"
  if [ "${skip_next_rewritten_arg}" -eq 1 ]; then
    skip_next_rewritten_arg=0
    continue
  fi
  case "$arg" in
    --one-pass-only|--stage0-only|--two-stage|--configure-first|--models-int8|--int8-models|--quant-int8|--models-int8-calibrate|--int8-calibrate|--calibrate-int8|--models-bf16|--bf16-models|--models-bf16-build|--bf16-build|--stitcher-minimize-blend|--minimize-blend)
      # run.sh-only flag; do not forward to pipeline-app
      continue
      ;;
    --int8-calib-frames|--int8-calib-batch-size|--int8-calib-start-seconds|--stitcher-compute|--stitcher-compute-precision)
      # run.sh-only flag with a separate value; the value is consumed below.
      skip_next_rewritten_arg=1
      continue
      ;;
    --int8-calib-frames=*|--int8-calib-batch-size=*|--int8-calib-start-seconds=*|--stitcher-compute=*|--stitcher-compute-precision=*)
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
if [ "${have_sink_arg}" -eq 1 ] && args_request_webrtc_sink "${rewritten_args[@]}"; then
  print_webrtc_access_urls 8080
  check_webrtc_runtime
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

if [ "${models_int8}" -eq 1 ] && [ "${models_bf16}" -eq 1 ]; then
  echo "--models-int8 and --models-bf16 are mutually exclusive"
  exit 2
fi

if [ "${models_int8}" -eq 1 ]; then
  extra_options+=(--options=pipeline.primary-gie.config-file=config_infer_yolov8_hockey_int8.yaml)
  int8_asset_config_file="${SCRIPT_DIR}/configs/config_infer_yolov8_hockey_int8.yaml"
elif [ "${models_bf16}" -eq 1 ]; then
  extra_options+=(--options=pipeline.primary-gie.config-file=config_infer_yolov8_hockey_bf16.yaml)
  bf16_asset_config_file="${SCRIPT_DIR}/configs/config_infer_yolov8_hockey_bf16.yaml"
fi

if [ -n "${stitcher_compute_precision}" ]; then
  case "${stitcher_compute_precision}" in
    fp32|float32|fp16|float16|half) ;;
    *)
      echo "Unsupported --stitcher-compute value: ${stitcher_compute_precision} (expected fp32 or fp16)"
      exit 2
      ;;
  esac
  extra_options+=(
    --options=pipeline.hmstitcher.stitch-compute-precision="${stitcher_compute_precision}"
  )
fi

if [ "${stitcher_minimize_blend}" -eq 1 ]; then
  extra_options+=(--options=pipeline.hmstitcher.minimize-blend=1)
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
if [ -n "${int8_asset_config_file}" ]; then
  asset_config_files+=("${int8_asset_config_file}")
fi
if [ -n "${bf16_asset_config_file}" ]; then
  asset_config_files+=("${bf16_asset_config_file}")
fi
if [ "${#asset_config_files[@]}" -gt 0 ]; then
  "${PYTHON_BIN:-python3}" "${SCRIPT_DIR}/scripts/setup_pretrained_assets.py" "${asset_config_files[@]}"
fi

yaml_property() {
  local config_file="$1"
  local key="$2"
  awk -v key="${key}" '
    $1 == key ":" {
      sub("^[^:]*:[[:space:]]*", "")
      gsub(/^["'\'']|["'\'']$/, "")
      print
      exit
    }
  ' "${config_file}"
}

abs_config_path() {
  local config_file="$1"
  local value="$2"
  if [ -z "${value}" ]; then
    return 1
  fi
  case "${value}" in
    /*) realpath -m "${value}" ;;
    *) realpath -m "$(dirname "${config_file}")/${value}" ;;
  esac
}

abs_cwd_path() {
  local value="$1"
  if [ -z "${value}" ]; then
    return 1
  fi
  case "${value}" in
    /*) realpath -m "${value}" ;;
    *) realpath -m "${PWD}/${value}" ;;
  esac
}

int8_artifact_paths() {
  int8_config_file="${int8_asset_config_file}"
  int8_engine_file="$(abs_config_path "${int8_config_file}" "$(yaml_property "${int8_config_file}" model-engine-file)")"
  int8_calib_table="$(abs_config_path "${int8_config_file}" "$(yaml_property "${int8_config_file}" int8-calib-file)")"
}

bf16_artifact_paths() {
  bf16_config_file="${bf16_asset_config_file}"
  bf16_engine_file="$(abs_config_path "${bf16_config_file}" "$(yaml_property "${bf16_config_file}" model-engine-file)")"
}

require_calibrated_int8_artifacts() {
  if [ ! -s "${int8_calib_table}" ]; then
    echo "INT8 requested but calibration table is missing or empty: ${int8_calib_table}"
    echo "Provide a pre-generated non-empty calibration table and INT8 engine; uncalibrated INT8 is not allowed."
    exit 2
  fi
  if [ ! -s "${int8_engine_file}" ]; then
    echo "INT8 requested but engine is missing or empty: ${int8_engine_file}"
    echo "Provide a pre-generated non-empty calibration table and INT8 engine; uncalibrated INT8 is not allowed."
    exit 2
  fi
}

require_bf16_artifacts() {
  if [ ! -s "${bf16_engine_file}" ]; then
    echo "BF16 requested but engine is missing or empty: ${bf16_engine_file}"
    echo "Run with --models-bf16-build first."
    exit 2
  fi
}

extract_int8_calibration_frames() {
  local out_dir="$1"
  local frame_count="$2"
  local start_seconds="$3"
  local frame_list="${out_dir}/images.txt"
  local game_dir
  local videos=()
  local video
  local per_video
  local produced
  local idx=0

  if [ -z "${game_id}" ]; then
    echo "--models-int8-calibrate requires --game-id"
    exit 2
  fi
  if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "--models-int8-calibrate requires ffmpeg on PATH"
    exit 2
  fi

  if [ -n "${HM_GAME_DIR:-}" ]; then
    game_dir="${HM_GAME_DIR}/${game_id}"
  else
    game_dir="${HOME}/Videos/${game_id}"
  fi
  if [ ! -d "${game_dir}" ]; then
    echo "Game directory not found for INT8 calibration: ${game_dir}"
    exit 2
  fi

  mapfile -d '' videos < <(
    find "${game_dir}" -type f \
      \( -iname '*.mp4' -o -iname '*.mov' -o -iname '*.mkv' -o -iname '*.avi' \) \
      -print0 | sort -z
  )
  if [ "${#videos[@]}" -eq 0 ]; then
    echo "No video files found for INT8 calibration in ${game_dir}"
    exit 2
  fi

  rm -rf "${out_dir}"
  mkdir -p "${out_dir}"
  per_video=$(((frame_count + ${#videos[@]} - 1) / ${#videos[@]}))
  if [ "${per_video}" -lt 1 ]; then
    per_video=1
  fi

  for video in "${videos[@]}"; do
    ffmpeg -hide_banner -loglevel error -y \
      -ss "${start_seconds}" \
      -i "${video}" \
      -vf fps=2 \
      -frames:v "${per_video}" \
      "${out_dir}/calib_${idx}_%04d.jpg"
    produced=$(find "${out_dir}" -maxdepth 1 -type f -name "calib_${idx}_*.jpg" | wc -l)
    if [ "${produced}" -gt 0 ] && [ "$(find "${out_dir}" -maxdepth 1 -type f -name 'calib_*.jpg' | wc -l)" -ge "${frame_count}" ]; then
      break
    fi
    idx=$((idx + 1))
  done

  find "${out_dir}" -maxdepth 1 -type f -name 'calib_*.jpg' | sort | head -n "${frame_count}" > "${frame_list}"
  if [ ! -s "${frame_list}" ]; then
    echo "Failed to extract INT8 calibration frames from ${game_dir}"
    exit 2
  fi
  produced=$(wc -l < "${frame_list}")
  if [ "${produced}" -lt "${frame_count}" ]; then
    echo "Only extracted ${produced}/${frame_count} INT8 calibration frames from ${game_dir}"
    exit 2
  fi
  echo "${frame_list}"
}

build_int8_calibration_artifacts() {
  local image_list="$1"
  local onnx_file
  local scale
  local infer_batch_size
  local builder_bin="${SCRIPT_DIR}/bazel-bin/src/apps/int8-calib-builder/int8-calib-builder"
  local tmp_engine
  local tmp_calib_table
  local manifest_file
  local tmp_manifest_file
  local onnx_sha
  local images_sha

  onnx_file="$(abs_config_path "${int8_config_file}" "$(yaml_property "${int8_config_file}" onnx-file)")"
  scale="$(yaml_property "${int8_config_file}" net-scale-factor)"
  infer_batch_size="$(yaml_property "${int8_config_file}" batch-size)"
  if [ -z "${scale}" ]; then
    scale="0.0039215697906911373"
  fi
  if [ -z "${infer_batch_size}" ]; then
    infer_batch_size=1
  fi
  if [ "${int8_calib_batch_size}" -ne "${infer_batch_size}" ]; then
    echo "--int8-calib-batch-size=${int8_calib_batch_size} does not match ${int8_config_file} batch-size=${infer_batch_size}"
    echo "The TensorRT engine profile must match the DeepStream nvinfer batch size."
    exit 2
  fi

  echo "Building INT8 calibration builder"
  bazelisk build --config=opt //src/apps/int8-calib-builder:int8-calib-builder

  echo "Building calibrated INT8 engine from ${int8_calib_frames} sampled frame(s); normal run will still start at timestamp zero"
  mkdir -p "$(dirname "${int8_calib_table}")" "$(dirname "${int8_engine_file}")"
  tmp_engine="${int8_engine_file}.tmp.$$"
  tmp_calib_table="${int8_calib_table}.tmp.$$"
  manifest_file="${int8_engine_file}.manifest.json"
  tmp_manifest_file="${manifest_file}.tmp.$$"
  rm -f "${tmp_engine}" "${tmp_calib_table}" "${tmp_manifest_file}"

  "${builder_bin}" \
    --onnx="${onnx_file}" \
    --image-list="${image_list}" \
    --calib-table="${tmp_calib_table}" \
    --engine="${tmp_engine}" \
    --batch-size="${int8_calib_batch_size}" \
    --min-batch-size=1 \
    --scale="${scale}"

  if [ ! -s "${tmp_calib_table}" ]; then
    echo "INT8 calibration builder did not produce a non-empty calibration table: ${tmp_calib_table}"
    rm -f "${tmp_engine}" "${tmp_calib_table}"
    exit 2
  fi
  if [ ! -s "${tmp_engine}" ]; then
    echo "INT8 calibration builder did not produce a non-empty engine: ${tmp_engine}"
    rm -f "${tmp_engine}" "${tmp_calib_table}"
    exit 2
  fi

  onnx_sha="$(sha256sum "${onnx_file}" | awk '{print $1}')"
  images_sha="$(sha256sum "${image_list}" | awk '{print $1}')"
  cat > "${tmp_manifest_file}" <<EOF
{
  "onnx_file": "${onnx_file}",
  "onnx_sha256": "${onnx_sha}",
  "image_list": "${image_list}",
  "image_list_sha256": "${images_sha}",
  "calibration_frames": ${int8_calib_frames},
  "batch_size": ${int8_calib_batch_size},
  "min_batch_size": 1,
  "net_scale_factor": ${scale},
  "engine_file": "${int8_engine_file}",
  "calibration_table": "${int8_calib_table}"
}
EOF

  mv -f "${tmp_calib_table}" "${int8_calib_table}"
  mv -f "${tmp_engine}" "${int8_engine_file}"
  mv -f "${tmp_manifest_file}" "${manifest_file}"
}

build_bf16_engine_artifact() {
  local onnx_file
  local builder_bin="${SCRIPT_DIR}/bazel-bin/src/apps/int8-calib-builder/int8-calib-builder"
  local tmp_engine
  local manifest_file
  local tmp_manifest_file
  local onnx_sha
  local infer_batch_size

  onnx_file="$(abs_config_path "${bf16_asset_config_file}" "$(yaml_property "${bf16_asset_config_file}" onnx-file)")"
  infer_batch_size="$(yaml_property "${bf16_asset_config_file}" batch-size)"
  if [ -z "${infer_batch_size}" ]; then
    infer_batch_size=1
  fi
  if [ "${int8_calib_batch_size}" -ne "${infer_batch_size}" ]; then
    echo "--int8-calib-batch-size=${int8_calib_batch_size} does not match ${bf16_asset_config_file} batch-size=${infer_batch_size}"
    echo "The TensorRT engine profile must match the DeepStream nvinfer batch size."
    exit 2
  fi

  echo "Building BF16 engine builder"
  bazelisk build --config=opt //src/apps/int8-calib-builder:int8-calib-builder

  echo "Building BF16 detector engine; normal run will still start at timestamp zero"
  mkdir -p "$(dirname "${bf16_engine_file}")"
  tmp_engine="${bf16_engine_file}.tmp.$$"
  manifest_file="${bf16_engine_file}.manifest.json"
  tmp_manifest_file="${manifest_file}.tmp.$$"
  rm -f "${tmp_engine}" "${tmp_manifest_file}"

  "${builder_bin}" \
    --precision=bf16 \
    --onnx="${onnx_file}" \
    --engine="${tmp_engine}" \
    --batch-size="${infer_batch_size}" \
    --min-batch-size=1

  if [ ! -s "${tmp_engine}" ]; then
    echo "BF16 engine builder did not produce a non-empty engine: ${tmp_engine}"
    rm -f "${tmp_engine}" "${tmp_manifest_file}"
    exit 2
  fi

  onnx_sha="$(sha256sum "${onnx_file}" | awk '{print $1}')"
  cat > "${tmp_manifest_file}" <<EOF
{
  "precision": "bf16",
  "onnx_file": "${onnx_file}",
  "onnx_sha256": "${onnx_sha}",
  "batch_size": ${infer_batch_size},
  "min_batch_size": 1,
  "engine_file": "${bf16_engine_file}"
}
EOF

  mv -f "${tmp_engine}" "${bf16_engine_file}"
  mv -f "${tmp_manifest_file}" "${manifest_file}"
}

filter_calibration_pipeline_args() {
  local skip_next=0
  local arg
  for arg in "$@"; do
    if [ "${skip_next}" -eq 1 ]; then
      skip_next=0
      continue
    fi
    case "${arg}" in
      --enable-sinks|-k|-t|--time-limit)
        skip_next=1
        continue
        ;;
      --enable-sinks=*|-k*|-t=*|--time-limit=*|--show)
        continue
        ;;
    esac
    printf '%s\n' "${arg}"
  done
}

pipeline_args=(
  "${config_args[@]}"
  --enable-sources=URI-MULTIPLE
  "${sink_args[@]}"
  --options=pipeline.hmaudio.enable="${hmaudio_enable}"
  "${extra_options[@]}"
  "${rewritten_args[@]}"
)

if [ "${models_int8_calibrate}" -eq 1 ]; then
  int8_artifact_paths
  int8_calib_dir="${SCRIPT_DIR}/.cache/int8-calib-${game_id}"
  int8_calib_image_list="$(extract_int8_calibration_frames "${int8_calib_dir}" "${int8_calib_frames}" "${int8_calib_start_seconds}")"
  build_int8_calibration_artifacts "${int8_calib_image_list}"
  require_calibrated_int8_artifacts
elif [ "${models_int8}" -eq 1 ]; then
  int8_artifact_paths
  require_calibrated_int8_artifacts
elif [ "${models_bf16_build}" -eq 1 ]; then
  bf16_artifact_paths
  build_bf16_engine_artifact
  require_bf16_artifacts
elif [ "${models_bf16}" -eq 1 ]; then
  bf16_artifact_paths
  require_bf16_artifacts
fi

if [ "${ssh_forwarded_display}" -eq 1 ] && [ "${HM_ALLOW_SSH_RENDER:-0}" != "1" ] &&
  ! args_request_render_sink "${pipeline_args[@]}"; then
  echo "Unsetting SSH-forwarded DISPLAY=${DISPLAY} for non-render sinks so DeepStream EGL uses headless mode"
  unset DISPLAY
fi

if [ ! -x "${PIPELINE_APP_BIN}" ]; then
  echo "hmstream-cli is not built at ${PIPELINE_APP_BIN}; build //src/apps/pipeline-app:hmstream-cli first"
  exit 2
fi

"${PIPELINE_APP_BIN}" "${pipeline_args[@]}"
# bazel-bin/src/apps/pipeline-app/hmstream-cli -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
