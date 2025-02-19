#!/bin/bash
# Usage: ./scripts/configure_game.sh --game-id=<game-id>
set -e
SCRIPT_DIR=$(dirname $(realpath $0))
ROOT_DIR=$(realpath "${SCRIPT_DIR}/..")
HM_DIR="${ROOT_DIR}/external/hm"

cd $HM_DIR
./perf
./stitch.sh --configure-only --force $@
cd -
echo "Game videos have been configured"

