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

# A saved general calibration with ten points must get past replay's count
# check; nine must fail before creating an output. Missing images deliberately
# stop the ten-point case before invoking external calibration tools.
mkdir -p "$fixture/count-source"
cat > "$fixture/count-source/config.yaml" <<'EOF'
game:
  videos: {left: [], right: []}
stitching:
  control_point_matcher: superpoint-lightglue
  projection: rectilinear
  camera_fov: {horizontal_fov: 120, vertical_fov: 90}
EOF
printf '%s\n' '{}' > "$fixture/count-preset.yaml"
for count in 9 10; do
  {
    printf '%s\n' 'i w64 h48 v120'
    for ((index=0; index<count; ++index)); do
      printf 'c n0 N1 x%d y%d X%d Y%d t0\n' "$index" "$index" "$((index+1))" "$((index+1))"
    done
  } > "$fixture/count-source/hm_project.pto"
  if "$replay" "$fixture/count-source" "$fixture/count-output-$count" "$fixture/count-preset.yaml" > "$fixture/count-log" 2>&1; then
    echo 'Replay unexpectedly succeeded without camera images' >&2
    exit 1
  fi
  if [[ "$count" == 9 ]]; then
    grep -q 'insufficient control points' "$fixture/count-log"
    test ! -e "$fixture/count-output-$count"
  else
    grep -q 'left.png' "$fixture/count-log"
    test -f "$fixture/count-output-$count/config.yaml"
  fi
done

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
