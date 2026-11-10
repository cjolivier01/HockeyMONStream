# Constrained rink warp preview

`scripts/constrained_rink_warp.py` is an offline still renderer for evaluating
rink-specific straightening. It accepts an already stitched panorama and
explicit image-space guides. It writes a new directory containing the warped
PNG, a labeled before/after JPEG with identical framing, copied guides and a
JSON measurement report. Python 3, NumPy and OpenCV are required; SciPy is not.
The command accepts 8-bit, three-channel input images and refuses other formats
instead of silently dropping alpha or reducing precision.

This is an experiment tool, not a live projection setting. It does not modify
game settings, Hugin projects, camera maps, masks or calibration generations.
The CPU image remap is confined to this offline command. NVIDIA and AMD
playback remain unchanged. Wiring the warp into playback would require
composing the inverse field into both cameras' GPU maps, transforming all
panorama-to-ray/ice geometry, updating generation identities and exposing
matching desktop/web controls. Do not substitute the result for `s.png` or
publish it as a calibrated panorama.

## Reproduce the Littleton comparison

From the repository root:

```bash
python3 scripts/constrained_rink_warp.py \
  "$HOME/Videos/ccmwi-littleton-hawks-1/_hockeymon/s.png" \
  --guides configs/ccmwi_littleton_rink_warp.json \
  --mask "$HOME/Videos/ccmwi-littleton-hawks-1/_hockeymon/rink_mask_0.png" \
  --output-dir /tmp/littleton-rink-warp \
  --comparison-width 1800
```

The output directory must not exist. The example describes the 11830×2556
panorama inspected on 2026-11-10. Its near guide was sampled from a robust,
global degree-four fit of the ice mask's lower boundary, using the central
6–94% of image width and eight Huber-reweighting passes to suppress jagged
edges and obstructions. This fit was prepared for the example; the renderer
itself consumes explicit guide points and does not detect the rink. The far
guide protects the far-side boards. These are measured choices for that
image, not defaults or automatic rink detection. A changed camera calibration,
projection, leveling or crop requires new guides, even if the dimensions match.
The report records the hash of the exact image bytes decoded for the render.

The saved mask occupied 41.2% of the original canvas and 55.8% of the result.
The example allows at most 3× **vertical** stretching and brings the near guide
within one output pixel of its horizontal target over approximately 76% of
the width. The extreme ends retain curvature. The mask is approximate evidence
of ice coverage, not ground truth, and an increased area does not mean new
detail was recovered. Use the report's source-mask out-of-canvas count to check
whether any marked ice was displaced beyond the frame; the example displaced
zero marked ice pixels outside the canvas.

## Guide format and transform

Every document field is required; the tool has no independent projection
defaults. `schema` is `1`; `source_size` is `[width, height]`. `guides` is an
ordered list of `[x, far_y, near_y]` marks. Coordinates are fractions of the
pixel-center extent: `(0, 0)` is the top-left pixel and `(1, 1)` the bottom-right.
The marks must cover x=0 through x=1 and satisfy `0 <= far_y < near_y < 1`.

- `target_y` is the requested horizontal near-boundary position, at or below
  all near marks, and no farther than the bottom pixel.
- `max_vertical_stretch` bounds the local derivative `dy_out / dy_in`. A value
  of 1 is identity. It is not a bound on all distortion or on shear.
- `transition_fraction` sets each smooth shoulder's fraction of the distance
  between the guides, in `(0, 0.5]`.
- `limit_transition_fraction`, in `(0, 1)`, softens the switch into the stretch
  limit. It sacrifices a small amount of straightening around that threshold
  to avoid an abrupt horizontal change in slope.

Shape-preserving cubic interpolation defines the near curve and the ratio of
far to near, preventing crossing between marks. At each column, let `a` be the
far guide, `b` the near guide and `d` the admitted downward displacement. The
forward map is `x_out = x_in`, `y_out = y_in + d * F((y_in-a)/(b-a))`.
`F` is zero above the far guide and one below the near guide. Its derivative
ramps smoothly from zero to a constant and back to zero. Thus the forward
vertical derivative is at least one, the Jacobian stays positive, and the
transform cannot fold. Upright verticals remain vertical; the region above the
far guide stays exactly fixed. The continuation below the near guide is a
translation, so some foreground can leave the canvas.

The displacement is capped by the requested stretch limit before inversion.
The C1 soft cap never exceeds either the requested displacement or the budget.
The inverse has an analytic middle section and monotone quartic shoulders,
solved by bounded bisection. Rendering uses inverse sampling in row tiles to
avoid holes and avoid allocating full-resolution coordinate maps.

Perfectly flattening the entire front while fixing the far side becomes
singular where the visible ice narrows to zero at the rink ends. The renderer
reports the remaining guide error instead of silently violating the stretch
limit. Players inside the warped band can still change proportions. Judge
that tradeoff on multiple frames before considering playback integration.

Run the geometry and artifact regressions with:

```bash
python3 scripts/constrained_rink_warp_test.py
bazelisk test //scripts:constrained_rink_warp_test
```
