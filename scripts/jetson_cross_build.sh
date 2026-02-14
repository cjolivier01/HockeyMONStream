#!/usr/bin/env bash
# Cross-build for Jetson (aarch64) using an arm64 Docker image and Bazel.

set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-hstream/jetson-cross}"
IMAGE_TAG="${IMAGE_TAG:-latest}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CUDA_ROOT_HOST="${CUDA_ROOT_HOST:-/usr/local/cuda-12.8}"
DEEPSTREAM_ROOT_HOST="${DEEPSTREAM_ROOT_HOST:-/opt/nvidia/deepstream/deepstream-8.0}"

SSH_ARGS=()
if [[ -n "${SSH_AUTH_SOCK:-}" && -S "${SSH_AUTH_SOCK}" ]]; then
  SSH_ARGS+=("-v" "${SSH_AUTH_SOCK}:/ssh-agent" "-e" "SSH_AUTH_SOCK=/ssh-agent")
fi
if [[ -d "${HOME}/.ssh" ]]; then
  SSH_ARGS+=("-v" "${HOME}/.ssh:/root/.ssh:ro")
fi

if [[ ! -d "${CUDA_ROOT_HOST}" ]]; then
  echo "CUDA root not found at ${CUDA_ROOT_HOST}. Set CUDA_ROOT_HOST to your CUDA 12.x install." >&2
  exit 1
fi

if [[ ! -d "${DEEPSTREAM_ROOT_HOST}" ]]; then
  echo "DeepStream root not found at ${DEEPSTREAM_ROOT_HOST}. Set DEEPSTREAM_ROOT_HOST accordingly." >&2
  exit 1
fi

echo "Building x86 cross Bazel image (${IMAGE_NAME}:${IMAGE_TAG})..."
docker buildx build --platform=linux/amd64 \
  -t "${IMAGE_NAME}:${IMAGE_TAG}" \
  -f "${REPO_ROOT}/docker/jetson-cross/Dockerfile" \
  "${REPO_ROOT}"

echo "Running Bazel build inside the x86 container (cross-compiling to aarch64)..."
docker run --rm --platform=linux/amd64 \
  -v "${REPO_ROOT}:/workspace" \
  -v "${HOME}/miniforge3/envs/ubuntu:/home/colivier/miniforge3/envs/ubuntu:ro" \
  -v "${CUDA_ROOT_HOST}:/usr/local/cuda:ro" \
  -v "${CUDA_ROOT_HOST}:/usr/local/cuda-12.8:ro" \
  -v "${DEEPSTREAM_ROOT_HOST}:/opt/nvidia/deepstream/deepstream-8.0:ro" \
  -e BAZELISK_BASE_URL \
  -e CUDA_HOME=/usr/local/cuda \
  -e CUDA_PATH=/usr/local/cuda \
  -e CC=aarch64-linux-gnu-gcc \
  -e CXX=aarch64-linux-gnu-g++ \
  -e GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/root/.ssh/known_hosts -F /dev/null" \
  "${SSH_ARGS[@]}" \
  "${IMAGE_NAME}:${IMAGE_TAG}" \
  bash -lc 'set -euo pipefail; if [ -f /root/.ssh/config ]; then chmod 600 /root/.ssh/config || true; fi; cd /workspace && bazelisk build --config=jetson //src/apps/pipeline-app:all'
