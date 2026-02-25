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
if [ -n "${CONDA_PREFIX:-}" ]; then
  append_path LD_LIBRARY_PATH "${CONDA_PREFIX}/lib"
fi

# Default model assets are not committed; set them up on-demand for the default configs.
DEFAULT_WEIGHTS="${SCRIPT_DIR}/pretrained/deepstream/yolox/yolox_s.pth"
DEFAULT_LABELS="${SCRIPT_DIR}/pretrained/deepstream/yolox/labels_coco.txt"
DEFAULT_ONNX="${SCRIPT_DIR}/pretrained/deepstream/yolox/yolox_s.pth.onnx"
if [ ! -f "${DEFAULT_ONNX}" ] || [ ! -f "${DEFAULT_LABELS}" ] || [ ! -f "${DEFAULT_WEIGHTS}" ]; then
  echo "Missing default YOLOX pretrained assets; running scripts/setup_yolox_s_pretrained.sh"
  bash "${SCRIPT_DIR}/scripts/setup_yolox_s_pretrained.sh"
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

bazel-bin/src/apps/pipeline-app/pipeline-app \
  "${config_args[@]}" \
  --enable-sources=URI-MULTIPLE \
  "${sink_args[@]}" \
  --options=pipeline.hmaudio.enable=1 \
  "${rewritten_args[@]}"
# bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
