# Rink warp still editor

Open **Stitched → Rink warp…** in `hstream-ui`. It opens a dedicated browser
editor with a frozen copy of the selected game's `s.png` and a matching
`rink_mask_0.png`, if available. No playback is needed. Without a saved panorama,
the desktop asks for a local still. A stale mask produces a notice and does not
prevent opening the image. The web workspace exposes the same button beside
Stitched; it freezes the currently displayed saved preview. Use **Open image**
there for a full-resolution local still and **Open ice mask** for its mask.
The browser never uploads those chosen local files.

The source pane has draggable upper (blue) and lower (amber) guides. The result
pane redraws on edits using WebGL2. At 100% flattening, both guides reach common
horizontal rows. **Flattening** blends back to identity, and **Preserve
proportions** controls the horizontal expansion. Both target rows and horizontal
crop bounds are editable. **Mirror guide edits** first symmetrizes the curves,
then mirrors point moves/additions/removals. Double-click to add a point; select
one and use Delete or arrow keys to edit it. Endpoints remain at the image edges.
**Draw guide** replaces the active curve with a broad fit to a freehand stroke;
**Smooth active guide** refits existing marks. Undo/redo covers geometry edits.

A supplied mask is sampled at up to 2048 pixels per side. The initial lower guide
uses a robust quadratic/quartic/sextic fit to the lower boundary, rejecting small
isolated columns and downweighting obstruction outliers. The initial upper guide
is an estimate proportional to the lower guide: adjust it to the actual upper
rail. The initial lower target moves upward just enough to retain the sampled
boundary, with a small margin. Later manual target edits remain exactly as
entered. Green dots show the raw mask boundary, amber shows the editable fitted
line, and red dots/status identify sampled ice outside the output. This is an
advisory sampled check, not proof that every ice pixel or unmarked rink feature
is preserved.

Scroll over an image to zoom, right-drag to pan, and maximize either pane for
close inspection. Wheel gestures over value controls scroll the controls pane;
they never change a value. **Save guides** downloads a JSON document; **Load
guides** restores it. **Export PNG** renders the frozen source at the derived
full resolution in GPU tiles, without guide overlays. The preview uses at most
the baseline's 8192-pixel texture dimension; export uploads the full image.
Export is limited to 96 megapixels and the GPU's source-texture limit (shown in
an error when exceeded); save the guides and use the offline renderer beyond
that texture limit. Closing/reloading the editor discards unsaved edits.

The exact same HTML/JS is embedded in the native executable and generated into
HockeyMONWeb by `sync_rink_warp_editor.py`. There is no local HTTP server and no
new native runtime command. Native launch writes a private, content-addressed
HTML snapshot beneath the application's cache directory (`rink-warp/`); snapshots
can be removed after their browser tabs are closed. The editor cannot save to
calibration, alter tracking geometry, or change a running pipeline. Its JSON
checks source dimensions, not calibration identity: use it with its original
panorama. All shipped editor choices come from `hstream_ui.rink_warp` in the
shared baseline.

A standalone launcher uses the same editor:

```sh
python scripts/rink_warp_editor.py /path/to/s.png \
  --mask /path/to/rink_mask_0.png --output /tmp/rink-editor.html
```

The output file must not already exist. `--guides` optionally loads measured
marks; `--no-open` only writes the self-contained page.

`scripts/constrained_rink_warp.py` straightens two measured front rails in an
already stitched panorama. Both rails have common horizontal destination rows,
including at the ends. Upright posts stay upright. The output is a PNG at the
source height with an expanded or reduced width, a labeled before/after JPEG,
copied guides and a JSON measurement report.
Python 3, NumPy and OpenCV are required; SciPy is not.

The editor and command-line renderer are still-image tools, not saved playback settings. Game settings,
Hugin projects, camera maps, masks and calibration generations stay unchanged.
The CPU remap reads only the explicitly supplied still. NVIDIA and AMD playback
are unaffected. Live integration would need both GPU map implementations,
transformed panorama-to-ray/ice geometry, generation identities and matching
Qt/web controls. Do not replace `s.png` with the result.

## Reproduce the Littleton comparison

```bash
python3 scripts/constrained_rink_warp.py \
  "$HOME/Videos/ccmwi-littleton-hawks-1/_hockeymon/s.png" \
  --guides configs/ccmwi_littleton_rink_warp.json \
  --mask "$HOME/Videos/ccmwi-littleton-hawks-1/_hockeymon/rink_mask_0.png" \
  --output-dir /tmp/littleton-rink-warp \
  --comparison-width 1800
```

The output directory must not exist. Only 8-bit, three-channel images are
accepted. A supplied mask must be single-channel and match the image size.
If any marked ice would leave the output canvas, the command fails before
creating the output directory. This check includes the horizontal crop and
vertical displacement, not just the lower guide. A mask cannot establish
preservation of ice it failed to mark; inspect the actual rink ends too.

The example describes the 11830×2556 panorama inspected on 2026-11-10, with
SHA-256 `1c91d4a8502160978ac5729d7ef37b40f40d71106694f9c281aba15620c99815`.
Its guides follow the upper black glass rail and lower red board cap. The
central traces use broad quadratic/quartic fits, with explicit marks at the
physical ends. The lower central fit began with the ice-mask contour. It is
**not extrapolated through the right-hand gate**. These are image-specific,
editable measurements; the renderer does not automatically detect the rink.
Different calibration, leveling, crop or projection requires new guides.

The lower target is 96% of the image height, leaving about 102 native pixels
below the guide. The horizontal crop retains the left 96% of the source,
removing the right-hand access area and centering the rink. Horizontal spacing
matches the average vertical expansion between the rails.
The resulting canvas is 15455×2556; forcing it back to the original aspect
ratio would make the people too tall. Foreground below the boards continues
at the local scale and may leave frame; it is not compressed into a thin
strip. The complete supplied ice mask stays inside, with about 63 pixels
minimum clearance. Mask coverage changes from 41.2% to 45.5%. The comparison
fits each image to the same display width without changing its aspect ratio.

These measurements describe framing, not photographic quality. The far-side
corners move down and walls near the ends stretch. The example's sampled
maximum local vertical scale is about 2.67. Matching horizontal scale reduces changes in
proportion, but cannot remove shear or preserve every object exactly. Broad
guides also leave small real-world rail irregularities and existing stitch discontinuities. This is an artistic
rectification, not a new camera calibration or a distortion-free projection.
Inspect the full-resolution result, corners and player details before using
it as the basis for any future playback work.

## Explicit guide document

Every field is required. Schema `2` replaces the previous far/near guide
semantics; schema `1` documents are rejected rather than silently reinterpreted.
The editor writes schema `3`, which adds `strength` and `aspect_balance`, both
in `[0, 1]`. Schema `2` remains the exact original full-strength, full-balance
warp. Schema `3` permits the lower target above some lower marks. Its actual
column targets interpolate between the source guides and requested target rows
by `strength`; horizontal scale interpolates between 1 and the column expansion
by `aspect_balance`. At strength zero, no crop and any balance, the map is
identity. Both CPU and GPU evaluate the same rational quadratics.

- `source_size`: `[width, height]`, each in `[2, 32766]`.
- `guides`: ordered `[x, upper_y, lower_y]` marks, covering x=0 through x=1.
  Both y values lie inside the frame, with upper above lower. Adjacent x
  marks and y constraints need at least 0.0001 separation for conditioning.
- `target_rows`: `[upper_target, lower_target]`, ordered inside the frame.
  In schema 2, the lower target must be at or below every lower source mark.
- `horizontal_crop`: `[left, right]` in source coordinates. Use `[0, 1]` to
  retain the whole source width. Any other interval explicitly crops it.

Coordinates refer to pixel centers: `(0, 0)` is the top-left pixel and `(1, 1)`
is the bottom-right. `forward_y` and `inverse_y` both take **source x**;
`inverse_x` converts an output x to the corresponding source x first.

The source lower curve uses shape-preserving cubic interpolation. Interpolating
the upper/lower ratio prevents crossing between marks. At each source x, the
inverse y map interpolates these three anchors:

| Output y | Source y |
| --- | --- |
| 0 | 0 |
| upper target | measured upper rail |
| lower target | measured lower rail |

Each segment is a monotone rational quadratic. The endpoint derivatives use
the adjacent interval scales, with their harmonic mean at the shared upper
rail. This avoids a narrow region of excessive stretching between unit-slope
anchors. Positive interval lengths and derivatives guarantee a positive
Jacobian. The vertical joins have matching derivatives. Below the lower rail,
the local scale continues until the image bottom; if that would sample beyond
the source, a final monotone segment reaches its boundary instead. That last
boundary clamp is continuous in x, but can have a horizontal derivative change
in the foreground where it activates.

Horizontal expansion at each source column equals the average forward vertical
scale between the rails. Trapezoidal integration on a source-width sampling
grid gives an ordered x map and the required output width. The map uses linear
interpolation between these samples. Vertical lines remain vertical. The
output retains the source height; width is derived, rounded to a pixel and
must remain below OpenCV's 32767-pixel remap limit. Dimensions are never silently
clamped or stretched back to the input aspect ratio.

The forward y transform uses the analytic inverse of the same quadratic,
rather than fitting an unrelated reverse curve. Row-tiled inverse sampling
produces no holes and bounds temporary memory. The report includes guide
residuals, sampled vertical scales and horizontal interval scales. These do
not bound shear or imply that every scene line remains straight.

## Research and implementation scope

[He, Chang and Sun, Rectangling Panoramic Images via Warping (SIGGRAPH 2013)](https://people.csail.mit.edu/kaiming/sig13/index.html)
shows why line, shape and boundary constraints matter for panorama warps.
This renderer uses explicit line constraints and verifies rink containment;
it does not implement that paper's seam carving or global mesh optimization.

The monotone interpolation and analytic inversion follow the rational-quadratic
form in [Durkan et al., Neural Spline Flows (2019), equation 4 and appendix A.3](https://proceedings.neurips.cc/paper/2019/file/7ac71d433f282034e088473244df8c02-Paper.pdf),
with positive knot derivatives from the adjacent interval scales. Only that
interpolation formula is used; there is no neural network, synthesized content
or model dependency.

Run the geometry, crop-containment and artifact regressions with:

```bash
python3 scripts/constrained_rink_warp_test.py
bazelisk test //scripts:constrained_rink_warp_test
```

## Editor validation

The rendered interaction suite exercises dragging, mirror edits, freehand
redraw, point insertion/removal, undo/redo, wheel scrolling without value
changes, robust mask fitting, JSON round trips and GPU/CPU PNG agreement:

```sh
node src/apps/rink-warp-editor/warp-geometry-test.js
python scripts/rink_warp_editor_browser_test.py
RINK_WARP_BROWSER_GPU=1 python scripts/rink_warp_editor_browser_test.py
bazelisk test --config=opt //src/apps/hstream-ui:rink_warp_editor_test \
  //src/apps/hstream-ui:shared_backend_window_test
```

The browser suite requires Playwright and system Chrome. The default exercises
software WebGL; the hardware mode requires native Vulkan and rejects software
renderers. It passed on an NVIDIA RTX 5090 and AMD RX 9060 XT. Both validate
rendered pixels, not just JavaScript geometry. The float lookup sampler explicitly
uses high precision; reduced sampler precision caused errors on hardware that
software rendering did not reproduce.

On the referenced Littleton still, the automatic quartic fit produced a
14911×2556 image with lower target 0.923886. The complete original mask mapped
inside that output with about 5 pixels minimum clearance. Full-resolution GPU
versus CPU output had mean channel error 0.052/255 and 99th-percentile error 1.
This checks sampling and containment for that still; it does not claim that
automatic rail placement or all photographic proportions are exact.
