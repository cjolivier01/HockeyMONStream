# Rink exclusion controls

Open **Stitched Controls → Rink extents**. Top, bottom, left and right insets
move the mask edges in native stitched pixels. Positive values exclude more
ice; negative values expand the allowed region. All four default to zero and
accept integers from -4096 to 4096.

Controls work before playback and apply live during Program playback. **Save
Preset** keeps the values in that game's private `config.yaml`:

```yaml
ice_boundaries:
  mask_top_inset: 20
  mask_bottom_inset: -10
  mask_left_inset: 0
  mask_right_inset: 0
```

The settings use normal baseline → user → game → CLI precedence. Explicit
native `pipeline.ds-fieldmask.properties.mask-*-inset` properties win a
same-layer tie. Saving an edited control replaces its game-local native alias
with the canonical key. The four controls are independent of stitching:
changing them does not invalidate anything, and calibration cleanup preserves
them. The original `rink_mask_0.png`, centroid, and geometry remain unchanged.

The adjusted mask uses one-sided erosion (positive inset) or dilation (negative
inset), applied in top/bottom/left/right order. Rounded edges and interior holes
follow the same morphology; the operation is not a rectangular crop. Filtering
and the overlay share the exact same immutable adjusted mask. An excessive
inset can remove all ice and therefore all detections; zero restores the
original mask.

Turn on **Show mask and test points** (also available as the
existing **Rink mask** checkbox). Green shows the original mask; magenta shows
the adjusted mask and its outline. Tracked-player boxes also show:

- a blue triangle at the adjusted foot sampling point;
- a yellow circle at the adjusted center sampling point;
- a filled marker for the active test, and an outline for the other test.

The existing sample-point rules are preserved. Above the original mask
centroid, the adjusted feet must be on ice; below it, the adjusted box center
must be on ice. The baseline foot point is bottom-center minus 10% of player
height, nudged toward the rink center by 20% of half-width. The baseline center
point is box-center plus 10% of player height. Left/right edges use the same
foot point in the far half; they have no additional test point. The canonical
`lower_bbox_bottom_by_height_ratio`, `raise_bbox_center_by_height_ratio`, and
`left_bbox_by_half_width_ratio`/`right_bbox_by_half_width_ratio` retain these
sampling settings. All four are declared in the synchronized baseline.

Markers show where the displayed tracked boxes would be tested. The filter
itself runs on detections before tracking, so a smoothed/predicted tracked box
can differ from the detection that was originally accepted. Untracked/rejected
detections are not drawn. Scaled metadata coordinates use a centroid in the
same coordinate space. Only the Stitched preview gets the new overlays;
Program and recorded pixels are unchanged. Calibration-only playback does not
run the player filter. The existing CUDA/OpenGL preview backend is x86-only;
this change also builds native filtering/configuration on Jetson, without
introducing another preview backend.

The plugin snapshots live settings under a lock once per input batch. It
rebuilds the adjusted mask and packed lookup only when settings or the original
mask change, retaining old masks for any frames still being displayed. Zero
insets share the original CPU calibration mask. Steady-state detection lookup
has the same packed-bit cost as before.

Visible overlays cache a mask texture and contours, with the diagnostic texture
bounded to 2048 pixels on its longest side. That display approximation can be a
few native pixels on large panoramas; filtering uses the full native mask.
Hidden overlays do no texture, contour, marker, or drawing work. The original
and adjusted masks are already CPU-resident; no video surfaces are mapped or
copied from GPU to CPU by this feature.

For manual validation, change each inset during Program playback with the
Stitched tab selected and the mask shown. Observe the two masks and markers,
hide the mask, save, stop, and reopen the game. Verify the values persist and
existing panorama/maps/mask remain present. Recalibration should preserve the
insets. A rejected live command is shown in the rink status and runner log;
the editable value can still be saved for the next run.
