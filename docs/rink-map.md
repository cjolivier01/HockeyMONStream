# Rink map and goal centres

Open **Stitched → Rink → Rink map and goals…** after stitching calibration.
The editor reads the two saved camera stills. It does not run the video pipeline
or copy live GPU frames to the CPU. It currently requires a NONA calibration;
RANSAC and MAGSAC++ maps do not provide the calibrated source rays it needs.

## Marking and review

**Find landmarks** detects painted lines and shows other recognized rink
features as coloured contours. Outlines alternate colour and black dashes:

| Feature | Colour |
| --- | --- |
| Centre line | Red |
| Blue line | Blue |
| Goal line | Orange |
| Centre ice circle | Yellow |
| Zone circle | Lavender |
| Crease | Light cyan |
| Faceoff dot | Pink |
| Goal | Magenta |
| Slot box | Lime |
| Trapezoid | Mint |

Hover over a detected outline to see its identity. Reviewed line marks keep
the colour of their assigned identity; disabled marks are grey. Centre lines
are assigned automatically. Blue and goal lines need a left/right identity
from the observation list. Left/right mean
the rink ends as viewed from the cameras; near/far mean the two side boards.
Detection scores describe the masks, not the accuracy of a goal position.

Choose a point or line above the image to add manual evidence. A point takes one
click; a line takes two well-separated clicks along the paint or the **straight
section** of the boards at ice level. Scroll to zoom and drag the image to pan.
Green anchors can be dragged. Scrolling over selectors or dimension fields
never changes their values, even when focused. Editing a detected line's endpoints converts it
to a manual line. Uncheck incorrect evidence or remove it from the list.

The dimensions are assumptions in metres. Defaults describe a 200 × 85 ft rink
with 25 ft from centre to either blue line, an 11 ft goal-line inset and 28 ft
corner radii. Set these for the actual rink before reviewing the result.
Goal-line/board anchors account for rounded corners. Goal centres mean the
middle of the goal mouth **on the ice**, not the centre of the elevated net.

A centre line alone, or even centre and both blue lines, leaves the map partial.
The observations can still be saved. No camera-space goal estimates or rink
outline are drawn until the evidence constrains the plane. The small overhead
drawing is explicitly labelled as a layout reference while the map is partial.

Four well-spaced point correspondences can constrain a map without any visible
goals: for example, the near/far board intersections of the centre line and one
blue line. Alternatively, centre/blue lines plus both straight board lines can
constrain it. Duplicate detections do not supply missing geometry. Nearly
degenerate fits, implausible plane axes, and contradictory marks are rejected.
Disable a rejected observation to save the usable partial evidence.

A fitted map draws a cyan rink outline and estimated goal centres in both
source views, with thinner projected rink markings in their feature colours.
Review both cameras before **Save reviewed map**. The displayed
angular residual measures agreement with the input marks, not metre accuracy;
four marks can have zero residual. Extrapolated ends depend on the dimensions,
mark selection, and the quality of the source calibration.

## Playback mini-map

After saving a reviewed, fitted map, leave **Show rink mini-map during playback**
checked under **Stitched → Rink**, then start playback. The Program and Stitched
desktop GPU previews show an overhead rink with neutral player circles. The
setting applies on the next playback and works independently of player boxes.
It currently uses the x86 desktop GPU preview renderer.

Each circle projects a tracked player's box bottom-centre onto the ice plane.
Positions are estimates: occlusion, loose boxes, calibration error and assumed
rink dimensions affect accuracy. Players outside the estimated boards are
omitted. A partial or stale map cannot place players; complete/review it before
playback. Recalibration requires reviewing and saving again.

A circle shows the jersey number when enabled jersey recognition supplies a
fresh result for that track. Leading zeros are retained; missing or expired
numbers leave a plain circle. There is currently no home/away classification,
so all players use the same neutral colour. Track colours and jersey numbers do
not identify teams.

Startup validates the saved calibration, refits its source-camera evidence,
and prepares a bounded output-pixel-to-ray grid through Hugin, using the
published mapping TIFF origins and extents (including capped canvases). Playback maps
only compact player metadata; it handles panorama crops, preview resizing and
post-stitch rotation without reading video back to the CPU. Every frame must
match the prepared calibration and output dimensions. Missing metadata hides
the map for that frame rather than retaining old player positions.

The mini-map is a desktop preview overlay. It does not appear in encoded
archives or change the camera policy, ice mask or leveling.

## Storage and scope

`rink-map.yaml` stores the versioned evidence, dimensions, original source
pixels, calibrated rays, and calibration identity in the game directory. It is
written atomically. Reopening recomputes rays and fits; it never trusts a saved
matrix. Calibration changes or a competing editor's save block stale writes.
Unrelated game preferences do not invalidate the evidence.
To clear a saved map, remove all observations and save the empty partial map.

The editor runs offline and supplies the optional playback mini-map. It does
not change the ice mask, leveling angles, camera tracking or encoded Program output. Existing leveling selections are not retained
by the leveling flow; mark the physical rink features in this editor. Detection
uses the saved calibration pair, so occluded features may need manual help.

## Native model

First use downloads the 269 MiB ONNX export into
`~/.cache/hstream/models/rink-landmarks-mask2former-swin-s-76b96e34a9386267.onnx`.
AssetManager verifies SHA-256
`76b96e34a9386267c08e0edc6cb09b2196a94a933b27e2f92fcae5d9f0426a08`.
The model and export manifest are assets of the `pretrained-assets-v1` release.
Manual marking needs no model or network. `HM_RINK_LANDMARKS_ONNX_MODEL` may
select an already exported custom model with the same tensor contract.

The export comes from HockeyMON's `openmm` Mask2Former Swin-S landmark model,
iteration 97500 (validation segmentation mAP 0.773). It retains the eleven-class
ordering. Input is RGB/ImageNet-normalized, resized to a 1008-pixel long edge,
then padded to 1024. Raw heads have shapes `[1,100,12]` and `[1,100,256,256]`.
Native postprocessing bounds masks/contours, omits the whole-field mask, and
extracts line samples only from painted line classes. Area/net centroids are
not physical point correspondences. The generic `plot_rink_landmarks` baseline
keys remain HockeyMON-only; this editor has its own on-demand native model.

`scripts/export_rink_landmarks_onnx.py` performs offline export and numerical
comparison with PyTorch. Python is not used at runtime. Inference uses the
shared ONNX session, with its explicit CUDA allocation-failure CPU fallback.
The editor reads bounded PNG proxies once and runs only when requested.

## Checks

```sh
bazelisk test --config=opt --cpu=k8 \
  //src/libs/stitching:rink_map_test \
  //src/libs/stitching:rink_landmarks_test \
  //src/libs/stitching:playback_rink_map_test \
  //src/libs/draw_display:rink_mini_map_test \
  //src/apps/hstream-ui:rink_map_dialog_test --test_output=errors
```

The dialog test also supports a read-only real-game visual check. It runs native
inference and saves screenshots outside the checkout. An optional text file of
`camera feature x y` rows adds manual marks in original source pixels.

```sh
QT_QPA_PLATFORM=offscreen bazel-bin/src/apps/hstream-ui/rink_map_dialog_test \
  --inspect-game "$HOME/Videos/<game>" /tmp/rink-map
```

For an isolated copied game only, append a marks file and `--save-reviewed-map`
to save a fitted fixture. `playback_rink_map_test --load-game <copied-game>`
checks native playback preparation. The real UI E2E driver accepts
`HSTREAM_UI_E2E_RINK_MAP=1` to require successful preparation with player boxes
disabled; use `--x11-preview` to capture both GPU previews. See
[UI E2E testing](hstream-ui-e2e-testing.md).
