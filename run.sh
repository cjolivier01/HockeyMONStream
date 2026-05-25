#!/bin/bash
set -euo pipefail

# Default: two-stage run
# - Stage -1: stitching configuration (FAKE sink)
# - Stage 0: main app (default RENDER sink unless overridden by user)
#
# To test the one-pass stitcher (stage 0 only, config in-process when control masks are missing):
#   ./run.sh --one-pass-only --game-id=<game_id>

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

# Bazel-built GStreamer plugins live under per-plugin output directories. Keep those visible so running a focused
# target build, such as pipeline-app plus gst-videoprep, is enough for local runs without manually staging .so
# files.
BAZEL_GST_PLUGIN_ROOT="${SCRIPT_DIR}/bazel-bin/src/gst-plugins"
if [ -d "${BAZEL_GST_PLUGIN_ROOT}" ]; then
  for plugin_dir in "${BAZEL_GST_PLUGIN_ROOT}"/*; do
    if [ -d "${plugin_dir}" ]; then
      prepend_path GST_PLUGIN_PATH "${plugin_dir}"
      prepend_path LD_LIBRARY_PATH "${plugin_dir}"
    fi
  done
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

one_pass_only=0
have_sink_arg=0
rewritten_args=()
for arg in "$@"; do
  case "$arg" in
    --one-pass-only|--stage0-only) one_pass_only=1 ;;
    --enable-sinks|--enable-sinks=*|-k|-k*) have_sink_arg=1 ;;
  esac
done

for arg in "$@"; do
  case "$arg" in
    --one-pass-only|--stage0-only)
      # run.sh-only flag; do not forward to pipeline-app
      continue
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

default_main_sink="RENDER"
if [ -z "${DISPLAY:-}" ]; then
  # Headless / SSH shells often have no X server. Avoid defaulting to a video overlay sink.
  default_main_sink="ENCODE_FILE"
  if [ "${have_sink_arg}" -eq 0 ]; then
    echo "DISPLAY is not set and no sink was specified; defaulting to --enable-sinks=${default_main_sink} (override with --enable-sinks=RENDER)"
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

bazel-bin/src/apps/pipeline-app/pipeline-app \
  "${config_args[@]}" \
  --enable-sources=URI-MULTIPLE \
  "${sink_args[@]}" \
  --options=pipeline.hmaudio.enable=1 \
  "${rewritten_args[@]}"
# bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
