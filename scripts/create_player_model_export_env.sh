#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 EXISTING_GPU_PYTHON NEW_VENV_DIRECTORY" >&2
  echo "The base interpreter must already support HockeyMON torch/torchvision/MMCV." >&2
  exit 2
fi
base_python=$1
export_env=$2
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -e "$export_env" ]]; then
  echo "Refusing to modify an existing environment: $export_env" >&2
  exit 1
fi
"$base_python" -c 'import torch, torchvision, mmcv; print("Base:", torch.__version__, torchvision.__version__, mmcv.__version__)'
"$base_python" -m venv --system-site-packages "$export_env"
if command -v uv >/dev/null 2>&1; then
  uv pip install --python "$export_env/bin/python" --no-deps -r "$script_dir/player_model_export_requirements.txt"
else
  "$export_env/bin/python" -m pip install --no-deps -r "$script_dir/player_model_export_requirements.txt"
fi
"$export_env/bin/python" -c 'import torch, torchvision, mmcv, onnx, onnxruntime, timm, nltk, pytorch_lightning; print("Exporter dependencies loaded")'
"$export_env/bin/python" -m pip freeze --all > "$export_env/player-model-export-freeze.txt"
echo "Created $export_env; base GPU packages are inherited without changing them."
