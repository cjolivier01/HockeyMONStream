#!/bin/bash
set -euo pipefail

# Two-stage run:
# - Stage -1: stitching configuration (FAKE sink)
# - Stage 0: main app (default RENDER sink unless overridden by user)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
