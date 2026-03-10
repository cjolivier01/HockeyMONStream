#!/usr/bin/env bash
set -euo pipefail

print_help() {
  cat <<'EOF'
DockerBuild.sh: build the hstream DeepStream Docker image

Usage:
  ./DockerBuild.sh [options...]
  ./DockerBuild.sh --help

Options are passed to: scripts/hstream_cuda_container.py build

Common options:
  --tag TAG                        Docker image tag
  --target-platform PLATFORM       One of: x86_64, arm64, jetson
  --platform PLATFORM              Alias for --target-platform
  --network default|host|none      Build network mode
  --push                           Push instead of loading into local Docker

Examples:
  ./DockerBuild.sh
  ./DockerBuild.sh --target-platform x86_64
  ./DockerBuild.sh --target-platform jetson
  ./DockerBuild.sh --target-platform arm64 --tag deepstream-arm64
EOF
}

if [[ "${1:-}" == "-h" ]] || [[ "${1:-}" == "--help" ]]; then
  print_help
  exit 0
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

exec "${PYTHON_BIN}" "${REPO_ROOT}/scripts/hstream_cuda_container.py" build "$@"
