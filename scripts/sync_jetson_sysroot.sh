#!/usr/bin/env bash
set -euo pipefail

# Sync a Jetson rootfs and DeepStream install into a local sysroot for cross builds.
# Usage:
#   JETSON_HOST=user@jetson-ip [JETSON_SYSROOT=/opt/jetson-sysroot] ./scripts/sync_jetson_sysroot.sh [dest]

JETSON_HOST="${JETSON_HOST:-}"
SYSROOT="${1:-${JETSON_SYSROOT:-/opt/jetson-sysroot}}"

if [[ -z "${JETSON_HOST}" ]]; then
  echo "JETSON_HOST is required (e.g. ubuntu@192.168.55.1)" >&2
  exit 1
fi

sudo mkdir -p "${SYSROOT}"

copy_path() {
  local remote_path="$1"
  local dest_path="${SYSROOT}${remote_path}"
  sudo mkdir -p "$(dirname "${dest_path}")"
  sudo rsync -aHAX --numeric-ids --delete --rsync-path="sudo rsync" \
    "${JETSON_HOST}:${remote_path}" "${dest_path}"
}

copy_path "/lib"
copy_path "/usr/include"
copy_path "/usr/lib"
copy_path "/usr/local/include"
copy_path "/usr/local/lib"
copy_path "/usr/lib/aarch64-linux-gnu"
copy_path "/lib/aarch64-linux-gnu"

# Optional but needed for DeepStream-powered pipelines.
if ssh "${JETSON_HOST}" "[ -d /opt/nvidia ]"; then
  copy_path "/opt/nvidia"
fi

echo "Jetson sysroot synced to ${SYSROOT}"
