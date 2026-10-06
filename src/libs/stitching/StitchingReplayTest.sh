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

# Raw saved points stay raw after a default change or a later lens-profile
# addition. Exercise both recorded neural points and uncalibrated AKAZE,
# without a source matcher override and with/without a preset override.
sed -i '/control_point_matcher:/d' "$fixture/count-source/config.yaml"
printf '%s\n' '{}' > "$fixture/count-source/left_calibration.json"
for matcher in superpoint-lightglue akaze-hamming; do
  fingerprint=not-applicable
  if [[ "$matcher" == akaze-hamming ]]; then
    fingerprint=absent
  fi
  sed -e "s/^control-point-matcher=.*/control-point-matcher=$matcher/" \
      -e "s/^akaze-calibration-fingerprint=.*/akaze-calibration-fingerprint=$fingerprint/" \
      "$fixture/source/stitching_canvas_provenance" > "$fixture/count-source/stitching_canvas_provenance"
  for preset in count-preset preset; do
    output="$fixture/raw-$matcher-$preset"
    if "$replay" "$fixture/count-source" "$output" "$fixture/$preset.yaml" > "$fixture/raw-log" 2>&1; then
      echo 'Replay unexpectedly succeeded without camera images' >&2
      exit 1
    fi
    grep -q 'left.png' "$fixture/raw-log"
    expected_matcher="$matcher"
    if [[ "$preset" == preset ]]; then
      expected_matcher=superpoint-lightglue
    fi
    grep -q "control_point_matcher: $expected_matcher" "$output/config.yaml"
  done
done

# Versions 2–5 predate AKAZE metadata and describe original-image points too.
# Their missing fingerprint must not revive the current-default/profile guess.
for version in 2 3 4 5; do
  case "$version" in
    2) lines=9 ;;
    3) lines=11 ;;
    4) lines=12 ;;
    5) lines=16 ;;
  esac
  head -n "$lines" "$fixture/source/stitching_canvas_provenance" |
    sed -e "s/^version=.*/version=$version/" -e 's/^mapping-backend=.*/mapping-backend=nona/' \
      > "$fixture/count-source/stitching_canvas_provenance"
  for preset in count-preset preset; do
    output="$fixture/legacy-$version-$preset"
    if "$replay" "$fixture/count-source" "$output" "$fixture/$preset.yaml" > "$fixture/legacy-log" 2>&1; then
      echo 'Replay unexpectedly succeeded without camera images' >&2
      exit 1
    fi
    grep -q 'left.png' "$fixture/legacy-log"
    test -f "$output/config.yaml"
  done
done
