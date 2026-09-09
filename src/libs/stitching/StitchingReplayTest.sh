#!/usr/bin/env bash
set -euo pipefail
replay="$(realpath "$1")"
fixture="${TEST_TMPDIR}/replay-rectified-source"
mkdir -p "$fixture/source"
printf '%s\n' 'stitching: {control_point_matcher: akaze-hamming}' > "$fixture/source/config.yaml"
printf '%s\n' '{}' > "$fixture/source/left_calibration.json"
printf '%s\n' 'stitching: {control_point_matcher: superpoint-lightglue}' > "$fixture/preset.yaml"
if "$replay" "$fixture/source" "$fixture/output" "$fixture/preset.yaml" > "$fixture/log" 2>&1; then
  echo 'Replay accepted calibrated AKAZE points' >&2
  exit 1
fi
grep -q 'calibrated AKAZE rectified control points' "$fixture/log"
test ! -e "$fixture/output"

# Provenance must remain authoritative even after the source matcher/profile
# files have been edited or removed.
rm "$fixture/source/left_calibration.json"
printf '%s\n' 'stitching: {control_point_matcher: superpoint-lightglue}' > "$fixture/source/config.yaml"
cat > "$fixture/source/stitching_canvas_provenance" <<'EOF'
version=6
max-output-width=0
max-canvas-dimension=0
source-canvas-width=100
source-canvas-height=100
canvas-width=100
canvas-height=100
max-output-width-applied=0
max-canvas-dimension-applied=0
mapping-backend=opencv-magsac
projection=rectilinear
projection-parameters=none
projection-auto-fov=0
projection-horizontal-fov=120
projection-auto-canvas=1
projection-auto-crop=0
control-point-matcher=akaze-hamming
akaze-calibration-fingerprint=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
if "$replay" "$fixture/source" "$fixture/output" "$fixture/preset.yaml" > "$fixture/log" 2>&1; then
  echo 'Replay accepted rectified source provenance' >&2
  exit 1
fi
grep -q 'calibrated AKAZE rectified control points' "$fixture/log"
test ! -e "$fixture/output"
