During desktop setup, the crop editor opens after panorama alignment and post leveling, before final stitching
maps and scoreboard selection. **Full canvas — no cropping** is selected by default. **Use crop** saves the
choice and continues setup; cancelling stops setup. The choice is remembered, including no cropping. The
editor is offered again when the projected camera geometry changes, such as a new alignment, projection,
projection parameters, or leveling angles. Changing only crop bounds or output resolution does not require
another confirmation. Games with an existing calibration and no recorded choice are prompted before playback.

Use **Adjust crop…** in the stitching controls to control how much of the projected rink is retained. Stop
playback before editing. This control is available with the Nona mapping backend.

- **Auto — crop valid pixels** lets Hugin choose its valid-image rectangle during calibration. The editor shows
  an estimate of that rectangle when a saved calibration is available.
- **Full canvas — no cropping** removes all cropping, including any saved manual trim.
- **Manual** lets you drag the rectangle's edges or corners, move it by dragging inside, or enter the percentage
  trimmed from each side. When selected, Manual starts with the rectangle currently shown by Auto or Full canvas.
  **Keep full width** is unchecked by default; selecting it sets left and right trim to zero while preserving the top
  and bottom. Turn it off again to restore the previous horizontal trim.

If Auto cuts off too much horizontally, switch to Manual and select Keep full width. Adjust the top and bottom
to taste. Manual starts from the estimated Auto rectangle currently on screen, including when Auto was selected only
after opening the dialog. If the estimate is still being calculated, it replaces the untouched initial rectangle when
ready.
Black corners can remain when keeping more coverage; the rectangle shows exactly which area will be retained.

![Manual crop retaining the full rink width](images/projection-crop/manual.jpg)

**Use crop** returns the selection to the main controls. **Save Preset** saves it in the game's private config and
requests an edited view from the saved optimized NONA alignment. The next calibration regenerates projection,
maps, seam, and panorama without repeating control-point matching or optimization. Published artifacts remain
available until the replacement commits, and cancellation preserves the request for retry. Legacy projects require
AUTO canvas; unsupported or missing source geometry produces an error while preserving the existing result.
**Cancel** discards the dialog's edits.
The existing Auto checkbox remains available in the main controls.

Percentages refer to the **full projected canvas**, not the previously cropped image. For example, left/right
trim of 0%, top trim of 20%, and bottom trim of 10% saves:

```yaml
stitching:
  projection_framing:
    auto_crop: false
    crop: [0, 1, 0.20, 0.90]  # retained left, right, top, bottom bounds
```

The still preview is rendered from saved camera images and calibration at no more than 1600 pixels on either
axis. It defaults to NONA's faster hard seam. Select **Blend preview seams (slower)** to rerender the full preview
with blended composition. NONA's verbose stages appear in the dialog; rendering remains cancellable and has a
20-minute safety limit, while other Hugin helpers retain their 60-second limit. It does not read back live GPU video
frames. The crop overlay changes immediately; Auto's final bounds are recomputed at the actual output resolution
during calibration. If calibration is missing, busy, or does not match the current camera/projection settings, the
editor explains why its preview is unavailable. Mode selection and numeric trims remain available; save and calibrate
the current geometry to enable the preview.
An incomplete or failed calibration is reported directly in the empty preview area, with instructions to finish
calibration and reopen Adjust crop. Missing calibration files and image-loading errors are reported separately.

Camera image references may be relative (`left.png`, `right.png`) or absolute paths into the same game folder,
as emitted by Hugin/HockeyMON. Crop and leveling previews validate the ordered references and rewrite them
only in their temporary projects so rendering always uses the copied snapshot images. Published projects and
calibration generation/revision identities remain unchanged.
Leveling also accepts the current version-9 canvas metadata, which adds the control-point resolution.

Run the preview path regression, or a read-only crop/leveling smoke against a configured game, with:

```sh
bazelisk test --config=opt --cpu=k8 //src/apps/hstream-ui:calibration_preview_test
QT_QPA_PLATFORM=offscreen bazel-bin/src/apps/hstream-ui/calibration_preview_test "$HOME/Videos/gse-16a"
```
