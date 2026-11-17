#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PYTHON_BIN="${PYTHON_BIN:-python3}"
YOLOX_GIT_URL="${YOLOX_GIT_URL:-https://github.com/Megvii-BaseDetection/YOLOX.git}"
YOLOX_COMMIT="${YOLOX_COMMIT:-}" # optional pin
YOLOX_DIR="${YOLOX_DIR:-${ROOT_DIR}/.cache/YOLOX}"

WEIGHTS_URL="${WEIGHTS_URL:-https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_s.pth}"
WEIGHTS_FILE_BASENAME="yolox_s.pth"

LABELS_URL="${LABELS_URL:-https://raw.githubusercontent.com/pjreddie/darknet/master/data/coco.names}"
LABELS_FILE_BASENAME="labels_coco.txt"

PRETRAINED_DIR="${ROOT_DIR}/pretrained/deepstream/yolox"

ensure_dir_writable() {
  local dir="$1"
  if mkdir -p "${dir}" 2>/dev/null; then
    return 0
  fi

  # Likely a root-owned mount point behind the `pretrained` symlink.
  local real_dir="${dir}"
  if [[ "${dir}" == "${ROOT_DIR}/pretrained"* ]]; then
    local real_pretrained rel
    real_pretrained="$(readlink -f "${ROOT_DIR}/pretrained")"
    rel="${dir#${ROOT_DIR}/pretrained/}"
    real_dir="${real_pretrained}"
    if [ "${rel}" != "${dir}" ] && [ -n "${rel}" ]; then
      real_dir="${real_pretrained}/${rel}"
    fi
  fi
  sudo mkdir -p "${real_dir}"
  sudo chown -R "${USER}:${USER}" "${real_dir}"
}

echo "Pretrained dir: ${PRETRAINED_DIR}"
ensure_dir_writable "${PRETRAINED_DIR}"

weights_path="${PRETRAINED_DIR}/${WEIGHTS_FILE_BASENAME}"
labels_path="${PRETRAINED_DIR}/${LABELS_FILE_BASENAME}"
onnx_path="${weights_path}.onnx"

if [ ! -f "${weights_path}" ]; then
  echo "Downloading weights: ${WEIGHTS_URL}"
  curl -L --fail -o "${weights_path}" "${WEIGHTS_URL}"
else
  echo "Weights already present: ${weights_path}"
fi

if [ ! -f "${labels_path}" ]; then
  echo "Downloading labels: ${LABELS_URL}"
  curl -L --fail -o "${labels_path}" "${LABELS_URL}"
else
  echo "Labels already present: ${labels_path}"
fi

if [ ! -d "${YOLOX_DIR}/.git" ]; then
  echo "Cloning YOLOX into ${YOLOX_DIR}"
  mkdir -p "$(dirname "${YOLOX_DIR}")"
  git clone --depth 1 "${YOLOX_GIT_URL}" "${YOLOX_DIR}"
fi
if [ -n "${YOLOX_COMMIT}" ]; then
  git -C "${YOLOX_DIR}" fetch --depth 1 origin "${YOLOX_COMMIT}"
  git -C "${YOLOX_DIR}" checkout "${YOLOX_COMMIT}"
fi

# Recent torch ONNX export uses onnxscript.
if ! "${PYTHON_BIN}" -c "import onnxscript" >/dev/null 2>&1; then
  echo "Installing python dependency: onnxscript"
  "${PYTHON_BIN}" -m pip install onnxscript
fi

if [ ! -f "${onnx_path}" ]; then
  echo "Exporting ONNX: ${onnx_path}"
  PYTHONPATH="${YOLOX_DIR}${PYTHONPATH:+:${PYTHONPATH}}" \
    "${PYTHON_BIN}" "${ROOT_DIR}/utils/export_yolox.py" \
    -w "${weights_path}" \
    -c "${YOLOX_DIR}/exps/default/yolox_s.py" \
    --batch 2 \
    --opset 18
else
  echo "ONNX already present: ${onnx_path}"
fi

echo "Done."
