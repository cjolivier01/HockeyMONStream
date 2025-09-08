#!/usr/bin/env bash
set -euo pipefail

# Installs dual-recordd, dualctl, and gopro_remote_bridge to /usr/local/bin,
# and installs+enables systemd units for the recorder and BLE bridge.

SOCK_PATH="/run/dual-record.sock"
START_SERVICES=1

usage() {
  echo "Usage: $0 [--no-start] [--sock PATH]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-start) START_SERVICES=0; shift ;;
    --sock) SOCK_PATH="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

# Build binaries
bazelisk build --config=jetson \
  //src/apps/dual-record:dual-recordd \
  //src/apps/dual-record:dualctl \
  //src/apps/dual-record:gopro_remote_bridge

sudo install -m 0755 bazel-bin/src/apps/dual-record/dual-recordd /usr/local/bin/dual-recordd
sudo install -m 0755 bazel-bin/src/apps/dual-record/dualctl /usr/local/bin/dualctl
sudo install -m 0755 bazel-bin/src/apps/dual-record/gopro_remote_bridge /usr/local/bin/gopro_remote_bridge

# Install systemd units
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cp configs/systemd/dual-recordd.service "$tmpdir/dual-recordd.service"
cp configs/systemd/gopro-remote-bridge.service "$tmpdir/gopro-remote-bridge.service"

# Inject socket path into ExecStart
sudo sed -i "s#--sock /run/dual-record.sock#--sock ${SOCK_PATH//#/\\#}#g" "$tmpdir/dual-recordd.service"
sudo sed -i "s#--sock /run/dual-record.sock#--sock ${SOCK_PATH//#/\\#}#g" "$tmpdir/gopro-remote-bridge.service"

sudo install -m 0644 "$tmpdir/dual-recordd.service" /etc/systemd/system/dual-recordd.service
sudo install -m 0644 "$tmpdir/gopro-remote-bridge.service" /etc/systemd/system/gopro-remote-bridge.service

sudo systemctl daemon-reload
sudo systemctl enable dual-recordd.service
sudo systemctl enable gopro-remote-bridge.service

if [[ "$START_SERVICES" -eq 1 ]]; then
  sudo systemctl restart dual-recordd.service || sudo systemctl start dual-recordd.service
  sudo systemctl restart gopro-remote-bridge.service || sudo systemctl start gopro-remote-bridge.service
fi

echo "Installed dual-recordd + gopro-remote-bridge. Socket: $SOCK_PATH"

