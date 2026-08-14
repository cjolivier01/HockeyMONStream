#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
timestamp=$(date +%Y%m%d-%H%M%S)
artifact_dir=${1:-${repo_root}/test-artifacts/hstream-ui-x11/${timestamp}}
mkdir -p "${artifact_dir}"

cd "${repo_root}"
bazelisk build --config=debug //src/apps/hstream-ui:hstream_ui_test

export HSTREAM_UI_X11_ARTIFACT_DIR=${artifact_dir}
export HSTREAM_UI_X11_TEST_BINARY=${repo_root}/bazel-bin/src/apps/hstream-ui/hstream_ui_test
xvfb-run -a -s "-screen 0 1600x1000x24 -nolisten tcp" bash -c '
  xfwm4 --replace --compositor=on --vblank=off >/dev/null 2>&1 &
  wm_pid=$!
  trap '\''kill -TERM "${wm_pid}" 2>/dev/null || true'\'' EXIT
  QT_QPA_PLATFORM=xcb "${HSTREAM_UI_X11_TEST_BINARY}"
'

echo "HStream X11 interaction test passed. Screenshots: ${artifact_dir}"
