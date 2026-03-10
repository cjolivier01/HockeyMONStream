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
if "${SSH_BASE[@]}" "${JETSON_HOST}" "[ -d /usr/local/cuda/targets/aarch64-linux ]"; then
  copy_path "/usr/local/cuda/targets/aarch64-linux"
fi
copy_path "/usr/lib/aarch64-linux-gnu"
copy_path "/lib/aarch64-linux-gnu"

# Optional but needed for DeepStream-powered pipelines.
if "${SSH_BASE[@]}" "${JETSON_HOST}" "[ -d /opt/nvidia ]"; then
  copy_path "/opt/nvidia"
fi

# Create a local hybrid CUDA toolkit view for rules_cuda:
# - aarch64 target libs/headers from sysroot
# - host x86_64 nvcc tooling from local /usr/local/cuda
CUDA_ROOT="${SYSROOT}/usr/local/cuda"
if [[ -d "${CUDA_ROOT}/targets/aarch64-linux" ]]; then
  sudo mkdir -p "${CUDA_ROOT}/targets"
  if [[ ! -e "${CUDA_ROOT}/include" && -d "${CUDA_ROOT}/targets/aarch64-linux/include" ]]; then
    sudo ln -s "targets/aarch64-linux/include" "${CUDA_ROOT}/include"
  fi
  if [[ ! -e "${CUDA_ROOT}/lib64" && -d "${CUDA_ROOT}/targets/aarch64-linux/lib" ]]; then
    sudo ln -s "targets/aarch64-linux/lib" "${CUDA_ROOT}/lib64"
  fi
  if [[ ! -e "${CUDA_ROOT}/bin" && -d /usr/local/cuda/bin ]]; then
    sudo ln -s "/usr/local/cuda/bin" "${CUDA_ROOT}/bin"
  fi
  if [[ ! -e "${CUDA_ROOT}/nvvm" && -d /usr/local/cuda/nvvm ]]; then
    sudo ln -s "/usr/local/cuda/nvvm" "${CUDA_ROOT}/nvvm"
  fi
  if [[ ! -e "${CUDA_ROOT}/targets/x86_64-linux" && -d /usr/local/cuda/targets/x86_64-linux ]]; then
    sudo ln -s "/usr/local/cuda/targets/x86_64-linux" "${CUDA_ROOT}/targets/x86_64-linux"
  fi
fi

# Keep local x86_64 builds working when repositories point at the synced sysroot.
sudo mkdir -p "${SYSROOT}/usr/lib" "${SYSROOT}/lib"
if [[ ! -e "${SYSROOT}/usr/lib/x86_64-linux-gnu" && -d /usr/lib/x86_64-linux-gnu ]]; then
  sudo ln -s "/usr/lib/x86_64-linux-gnu" "${SYSROOT}/usr/lib/x86_64-linux-gnu"
fi
if [[ ! -e "${SYSROOT}/lib/x86_64-linux-gnu" && -d /lib/x86_64-linux-gnu ]]; then
  sudo ln -s "/lib/x86_64-linux-gnu" "${SYSROOT}/lib/x86_64-linux-gnu"
fi
if [[ -d /usr/include/glib-2.0 ]]; then
  sudo mkdir -p "${SYSROOT}/usr/include/glib-2.0"
  sudo rsync -a /usr/include/glib-2.0/ "${SYSROOT}/usr/include/glib-2.0/"
fi

echo "Jetson sysroot synced to ${SYSROOT}"
