# HStream UI end-to-end acceptance test

The UI acceptance test exercises the same Qt button handlers as an operator:

1. load the selected game;
2. configure display rendering for the selected test mode and enable **Archive File**;
3. click **Play**;
4. observe the private native scoreboard-selector URL and submit **No scoreboard**;
5. wait for positive pipeline FPS while recording several seconds;
6. click **Stop** and require a finalized encoded video;
7. compare sampled encoded frames with `panorama.tif` using SIFT features and a RANSAC homography.

Positive FPS alone is deliberately insufficient. The visual check requires at
least 20 geometrically consistent feature matches and a 35% inlier fraction,
so a healthy pipeline processing the wrong game fails.

For a fast UI-only regression run, the X11 interaction test uses real Qt mouse
events against native XCB widgets under a window manager. It verifies native
parent, geometry, and map state while exercising stopped double-click, live
focus/restore, resize, render-off-while-focused, stop-while-focused, and live render off/on. It also
saves composed X11 screenshots for inspection:

```bash
scripts/test_hstream_ui_x11_interactions.sh
```

## Run it

```bash
scripts/test_hstream_ui_e2e.sh short
```

On an x86_64 workstation with an active NVIDIA X11 display, add
`--x11-preview` to keep live rendering enabled. This variant clicks the
**Program**, **Stitched**, and **Camera 1** tabs, requires native render-target
acknowledgements, captures the Qt-painted preview surfaces, and fails if the
Program or Stitched surface is blank:

```bash
DISPLAY=:0 scripts/test_hstream_ui_e2e.sh --x11-preview short
```

`HM_GAME_DIR` selects the source game root, as it does for the application. An
optional second argument selects the artifact directory:

```bash
HM_GAME_DIR=/mnt/games scripts/test_hstream_ui_e2e.sh game-42 /tmp/hstream-ui-game-42
```

The script builds the x86_64 optimized UI, real CLI backend, and visual
verifier. It creates a sandbox game directory whose video inputs are symlinks
to the source game. Configured stitching artifacts are reflinked (or copied
when reflinks are not available), while `config.yaml`, UI state, locks, and
encoded output remain isolated. The source game is not modified.

On X11, the backend retains independent ordinary BGRx samples for the final
program render, the raw stitched canvas, and each camera source, then serves
bounded JPEG preview frames through HStream's private runtime-command channel.
The acceptance test requires all three distinct channel acknowledgements in
addition to non-blank pixels. Qt paints those frames in the tabs. This avoids relying on a video
sink painting directly into a child window, which is unreliable across Qt
backing stores, compositors, and mixed-DPI desktops. It does not alter the
full-resolution encode branch.

## Evidence retained

The artifact directory contains:

- `pipeline.log`: the complete, untruncated UI/backend log;
- `log-issues.txt`: warning/error/critical lines extracted for quick triage;
- `ui-running.png` and `ui-stopped.png`: Qt window screenshots;
- `program-preview-surface.png`, `stitched-preview-surface.png`,
  `camera1-preview-surface.png`, and `preview-report.txt`: live X11 evidence
  when `--x11-preview` is used;
- `backend-main-preview.jpg`, `backend-stitched-preview.jpg`, and
  `backend-source0-preview.jpg`: the first retained backend frames delivered
  to the Qt preview during an X11 run;
- `encoded-output/`: the actual archive written by the UI-selected sink;
- `encoded-frame.jpg`: the best sampled output frame;
- `panorama-reference.jpg`: a review-sized panorama;
- `panorama-feature-matches.jpg`: inlier feature links between output and panorama;
- `report.txt`, `visual-report.txt`, and `visual-verifier.log`: machine-readable pass/fail evidence and match counts.

The displayed Qt preview cannot be embedded when the test uses the offscreen
platform, so the encoded archive is the visual ground truth. Scoreboard
selection uses a modal Qt dialog in hstream-ui; the E2E test exercises its
native **No Scoreboard** action when scoreboard configuration is required.

For unusually sparse footage, `HSTREAM_UI_E2E_MIN_VISUAL_INLIERS` can adjust
the default of 20. Lowering it should be accompanied by inspection of
`panorama-feature-matches.jpg`; the 35% inlier-fraction requirement remains.
`HSTREAM_UI_E2E_RECORD_MS` can extend the post-FPS recording interval; the
default is 6000 ms.
