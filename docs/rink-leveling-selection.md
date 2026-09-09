HStream's **Algorithms** tab now has a rink selector, pitch and roll controls, and an optional **Level from posts**
dialog. Vallco inherits −35° pitch and Sharks Ice inherits −25° pitch. A saved game angle overrides the rink
profile, including when it happens to equal that profile's default. **Use rink default** removes the override.
Changing the rink while a game override is active keeps the override. No rink selection inherits zero rotation.

To measure the angle from a calibrated game:

1. Finish a NONA stitching calibration so the two source stills and calibrated project are available.
2. Open **Algorithms → Level from posts**. In each source image, click the top and bottom of upright wall or glass
   posts. Select at least three posts total, including both cameras, with good horizontal separation. Four or more
   long posts work better than short or clustered marks. Ceiling beams and rink corners are unsuitable references.
3. Click **Estimate from posts**. The dialog rejects poorly conditioned selections and reports how many posts agreed
   and their angular residual. Drag marks to refine them; scroll to zoom and drag the background to pan.
4. Click **Preview angles**. Inspect both rink ends and the walls. Adjust pitch or roll and preview again if desired.
   The still preview uses the saved projection and crop, so its framing may differ from unsaved projection controls.
5. Click **Use angles**, then **Save Preset**. The normal stitching invalidation flow rebuilds the GPU maps next run.

**Cancel**, Escape, and closing the dialog discard its changes, even after estimating or rendering a preview.
An accepted selection also remains staged until Save Preset (starting playback saves a staged selection first).
The dialog and preset save both check that the original calibration still matches before accepting an estimate.
Camera/FOV, matcher, and reference-frame changes require calibration before selecting posts. A staged estimate
cannot be saved with changed input controls. The snapshot also records the private config, so concurrent changes
to game settings reject the estimate; saved calibration marked pending or incomplete must finish first.
The original game images, project and maps are never overwritten by preview rendering.

![Selecting upright posts in the left camera image](images/rink-leveling/selection.jpg)

![Inspecting the estimated angles with the saved Sabercats framing](images/rink-leveling/preview.jpg)

The measurement uses exact Hugin lens calibration: `pano_trafo` maps source-image endpoint coordinates to rays
using a private equirectangular project. Each upright post defines a plane through the camera center. A robust fit
finds the shared vertical direction from those planes, estimates pitch and roll, and preserves yaw. It removes the
published project's previous rotation using matrices before fitting; it never subtracts Euler angles or rotates
the two registered cameras independently. Unsupported translated-camera projects and mismatched image sizes fail
with an explanation. Older NONA provenance (versions 2–7) predates this common rotation and implies zero.
The desktop selector requires camera metadata (version 7 or newer) to verify the selected model; older games
need one calibration with current camera settings first. The offline geometry helper can still read older projects.

The fit establishes physical level; the preferred visual framing can still benefit from manual adjustment.
Approximate marks on eight Sabercats posts produced pitch −30.810° and roll +0.159°, with six consistent posts
and 1.32° RMS angular residual. That differs from the visually tuned −35°/+3° preset; inspect the preview before
accepting a fit, especially with rough marks or posts that are not actually vertical.

The preview uses `pano_modify` and `nona` on temporary still files, at a maximum full-canvas width of 1600 pixels.
It preserves source bit depth so 16-bit camera stills display correctly, and bounds renderer execution to 60 seconds.
This is an offline calibration operation; playback continues to use the existing GPU remap path with no added
per-frame CPU readback. Hugin tools must be available on PATH. The repository's Qt frontend is currently excluded
from the Jetson build; the geometry helper is included in cross-platform validation.
The full x86 build and all four targets below passed. The geometry library and test cross-built for Jetson, and
the test ran successfully on `stubby`; its optional Hugin subprocess checks skipped there because those tools were
unavailable. The same subprocess checks and real-image preview ran successfully on the x86 workstation.

Validation targets:

```sh
bazelisk build --config=opt --cpu=k8 //...
bazelisk test --config=opt --cpu=k8 \
  //src/libs/stitching:rink_leveling_test \
  //src/apps/hstream-ui:rink_leveling_dialog_test \
  //src/apps/hstream-ui:scoreboard_selection_dialog_test \
  //src/apps/hstream-ui:hstream_ui_test
```

Geometry tests include synthetic known rotations, noisy and outlier marks, insufficient spread, repeated fits,
and installed Hugin round trips. Dialog tests cover preview/accept/cancel, cancellation during rendering, stale
source rejection, and invalid dimensions. Main-window tests cover rink changes, explicit overrides equal to defaults,
and returning to inheritance. A real-image integration and screenshot check can be run without changing the game:

```sh
QT_QPA_PLATFORM=offscreen bazel-bin/src/apps/hstream-ui/rink_leveling_dialog_test \
  "$HOME/Videos/sabercats-16a-preferred" /tmp/rink-leveling
```

This uses the documented approximate Sabercats marks and writes `/tmp/rink-leveling-selection.png` and
`/tmp/rink-leveling-preview.png`. It verifies the actual 16-bit still preview contains visible rink pixels.
