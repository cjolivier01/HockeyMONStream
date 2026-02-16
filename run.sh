#!/bin/bash
set -euo pipefail

# Two-stage run:
# - Stage -1: stitching configuration (FAKE sink)
# - Stage 0: main app (default RENDER sink unless overridden by user)

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

have_sink_arg=0
rewritten_args=()
for arg in "$@"; do
  case "$arg" in
    --enable-sinks|--enable-sinks=*|-k|-k*) have_sink_arg=1; break ;;
  esac
done

for arg in "$@"; do
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

sink_args=(--enable-sinks=FAKE)
if [ "${have_sink_arg}" -eq 0 ]; then
  sink_args+=(--enable-sinks=RENDER)
fi

bazel-bin/src/apps/pipeline-app/pipeline-app \
  -c configs/ds_hockey_configure_stitching.yaml \
  -c configs/ds_hockey_app_config.yaml \
  --enable-sources=URI-MULTIPLE \
  "${sink_args[@]}" \
  --options=pipeline.hmaudio.enable=1 \
  "${rewritten_args[@]}"
# bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
