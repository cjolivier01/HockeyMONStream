#!/usr/bin/env bash
set -euo pipefail

print_help() {
  cat <<'EOF'
DockerRun.sh: run hstream commands inside the DeepStream Docker image

Usage:
  ./DockerRun.sh [global options] <command> [command args...]
  ./DockerRun.sh --help

Main commands:
  run                  Build pipeline-app if needed, then run ./run.sh
  pipeline-app         Build and run bazel-bin/src/apps/pipeline-app/pipeline-app
  video-player         Build and run bazel-bin/src/apps/video-player/video-player
  dual-record          Build and run bazel-bin/src/apps/dual-record/dual-record
  dual-recordd         Build and run bazel-bin/src/apps/dual-record/dual-recordd
  dualctl              Build and run bazel-bin/src/apps/dual-record/dualctl
  gopro_remote_bridge  Build and run bazel-bin/src/apps/dual-record/gopro_remote_bridge
  build                Run bazelisk build inside the container

Other commands:
  bash                 Open an interactive shell in the repo mount
  console              Same as bash

Global options:
  --tag TAG                    Docker image tag
  --target-platform PLATFORM   One of: x86_64, arm64, jetson
  --platform PLATFORM          Alias for --target-platform
  --gpus VALUE                 Docker --gpus value (default: auto)
  --no-gpus                    Disable GPU flags
  --videos-mount DIR           Host videos dir (default: ~/Videos)
  --no-videos-mount            Disable videos mount
  --workdir DIR                Host work dir mounted as /workspace/workdir
  --no-dev-mount               Do not bind-mount this repo
  --name NAME                  Container name
  --network NET                Container network (default: bridge/host auto)
  --shm-size SIZE              Shared memory size (default: 8g)
  --no-privileged              Do not run the container in privileged mode

Examples:
  ./DockerBuild.sh --target-platform x86_64
  ./DockerRun.sh bash
  ./DockerRun.sh run --game-id stockton-r3 -t=60
  ./DockerRun.sh pipeline-app -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE
  ./DockerRun.sh dual-record --out-dir /Videos/captures --duration-sec 30
  ./DockerRun.sh --target-platform jetson run --game-id stockton-r3 -t=60
EOF
}

if [[ $# -eq 0 ]] || [[ "${1:-}" == "-h" ]] || [[ "${1:-}" == "--help" ]]; then
  print_help
  exit 0
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"
PYTHON_BIN="${PYTHON_BIN:-python3}"

exec "${PYTHON_BIN}" "${REPO_ROOT}/scripts/hstream_cuda_container.py" run "$@"
