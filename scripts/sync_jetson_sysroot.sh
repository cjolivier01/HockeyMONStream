#!/usr/bin/env bash
set -euo pipefail

# Sync a Jetson rootfs and DeepStream install into a local sysroot for cross builds.
# Usage:
#   JETSON_HOST=user@jetson-ip [JETSON_SYSROOT=/opt/jetson-sysroot] ./scripts/sync_jetson_sysroot.sh [dest]

JETSON_HOST="${JETSON_HOST:-}"
SYSROOT="${1:-${JETSON_SYSROOT:-/opt/jetson-sysroot}}"
LOCAL_SSH_USER="${SUDO_USER:-${USER}}"
SSH_BASE=(sudo -n -u "${LOCAL_SSH_USER}" ssh -o BatchMode=yes -o ConnectTimeout=10)
RSYNC_SSH_CMD="sudo -n -u ${LOCAL_SSH_USER} ssh -o BatchMode=yes -o ConnectTimeout=10"

if [[ -z "${JETSON_HOST}" ]]; then
  echo "JETSON_HOST is required (e.g. ubuntu@192.168.55.1)" >&2
  exit 1
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required locally but was not found in PATH." >&2
  exit 1
fi

if ! command -v ssh >/dev/null 2>&1; then
  echo "ssh is required locally but was not found in PATH." >&2
  exit 1
fi

if ! sudo -n true >/dev/null 2>&1; then
  echo "Local sudo requires a password. Configure NOPASSWD or run sudo once before syncing." >&2
  exit 1
fi

if ! "${SSH_BASE[@]}" "${JETSON_HOST}" true; then
  echo "SSH auth to ${JETSON_HOST} failed in BatchMode (key-based auth required for non-interactive sync)." >&2
  exit 1
fi

if ! "${SSH_BASE[@]}" "${JETSON_HOST}" 'sudo -n true' >/dev/null 2>&1; then
  echo "Remote sudo on ${JETSON_HOST} requires a password. Ensure NOPASSWD is configured for this user." >&2
  exit 1
fi

# Use the same toolkit release for the host compiler and target headers.
REMOTE_CUDA_ROOT="$("${SSH_BASE[@]}" "${JETSON_HOST}" readlink -f /usr/local/cuda)"
HOST_CUDA_ROOT="${JETSON_HOST_CUDA_ROOT:-${REMOTE_CUDA_ROOT}}"
if [[ ! -x "${HOST_CUDA_ROOT}/bin/nvcc" ]]; then
  echo "Matching host CUDA compiler is missing: ${HOST_CUDA_ROOT}/bin/nvcc" >&2
  exit 1
fi
sudo mkdir -p "${SYSROOT}"

copy_path() {
  local remote_path="$1"
  local dest_path="${SYSROOT}${remote_path}"
  echo "Syncing ${remote_path} -> ${dest_path}"
  sudo mkdir -p "${dest_path}"
  sudo rsync -aHAX --numeric-ids --delete --human-readable --info=progress2,stats1 -e "${RSYNC_SSH_CMD}" --rsync-path="sudo -n rsync" \
    "${JETSON_HOST}:${remote_path}/" "${dest_path}/"
  echo "Finished ${remote_path}"
}

copy_path "/lib"
copy_path "/usr/include"
copy_path "/usr/lib"
copy_path "/usr/local/include"
copy_path "/usr/local/lib"
# CUDA 13 uses the SBSA toolkit layout on Jetson too.
if ! "${SSH_BASE[@]}" "${JETSON_HOST}" "[ -d /usr/local/cuda/targets/sbsa-linux ]"; then
  echo "Jetson Linux 39 CUDA target SDK is missing: /usr/local/cuda/targets/sbsa-linux" >&2
  exit 1
fi
copy_path "/usr/local/cuda/targets/sbsa-linux"
copy_path "/usr/lib/aarch64-linux-gnu"
copy_path "/lib/aarch64-linux-gnu"

# Optional but needed for DeepStream-powered pipelines.
if "${SSH_BASE[@]}" "${JETSON_HOST}" "[ -d /opt/nvidia ]"; then
  copy_path "/opt/nvidia"
fi

# Create a local hybrid CUDA toolkit view for rules_cuda:
# - ARM64 target libs/headers from sysroot
# - host x86_64 nvcc tooling from the matching local toolkit
CUDA_ROOT="${SYSROOT}/usr/local/cuda"
if [[ -d "${CUDA_ROOT}/targets/sbsa-linux" ]]; then
  sudo mkdir -p "${CUDA_ROOT}/targets"
  if [[ ! -e "${CUDA_ROOT}/include" && -d "${CUDA_ROOT}/targets/sbsa-linux/include" ]]; then
    sudo ln -s "targets/sbsa-linux/include" "${CUDA_ROOT}/include"
  fi
  if [[ ! -e "${CUDA_ROOT}/lib64" && -d "${CUDA_ROOT}/targets/sbsa-linux/lib" ]]; then
    sudo ln -s "targets/sbsa-linux/lib" "${CUDA_ROOT}/lib64"
  fi
  # nvcc resolves its SDK through bin/..; a directory symlink would escape
  # into the host toolkit. Keep bin real and link its individual host tools.
  if [[ -L "${CUDA_ROOT}/bin" ]]; then
    sudo rm -- "${CUDA_ROOT}/bin"
  fi
  sudo mkdir -p "${CUDA_ROOT}/bin"
  for host_tool in "${HOST_CUDA_ROOT}/bin/"*; do
    sudo ln -sfn "${host_tool}" "${CUDA_ROOT}/bin/$(basename "${host_tool}")"
  done
  # x86 nvcc still selects this name for an aarch64 cross compiler.
  sudo ln -sfn sbsa-linux "${CUDA_ROOT}/targets/aarch64-linux"
  sudo ln -sfn "${HOST_CUDA_ROOT}/nvvm" "${CUDA_ROOT}/nvvm"
  sudo ln -sfn "${HOST_CUDA_ROOT}/targets/x86_64-linux" "${CUDA_ROOT}/targets/x86_64-linux"
fi

# Preserve the target's GLib headers. Host builds select host repositories;
# mixing host headers into this sysroot breaks target ABI consistency.
sudo mkdir -p "${SYSROOT}/etc"
sudo rsync -a -e "${RSYNC_SSH_CMD}" \
  "${JETSON_HOST}:/etc/os-release" "${JETSON_HOST}:/etc/nv_tegra_release" "${SYSROOT}/etc/"

echo "Jetson sysroot synced to ${SYSROOT}"
