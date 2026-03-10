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
  sudo mkdir -p "$(dirname "${dest_path}")"
  sudo rsync -aHAX --numeric-ids --delete --human-readable --info=progress2,stats1 -e "${RSYNC_SSH_CMD}" --rsync-path="sudo -n rsync" \
    "${JETSON_HOST}:${remote_path}" "${dest_path}"
  echo "Finished ${remote_path}"
}

copy_path "/lib"
copy_path "/usr/include"
copy_path "/usr/lib"
copy_path "/usr/local/include"
copy_path "/usr/local/lib"
copy_path "/usr/lib/aarch64-linux-gnu"
copy_path "/lib/aarch64-linux-gnu"

# Optional but needed for DeepStream-powered pipelines.
if "${SSH_BASE[@]}" "${JETSON_HOST}" "[ -d /opt/nvidia ]"; then
  copy_path "/opt/nvidia"
fi

echo "Jetson sysroot synced to ${SYSROOT}"
