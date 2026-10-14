# Automatic input lens

New games select **Auto (video metadata)** in the main Stitched → Projection
controls and Stitching Experiments. HStream reads the selected original camera
files when loading a game, selecting Auto or changing its imported videos.
Compatible metadata fills the shared FOV, projection, distortion and optical
center fields. It does not change the panorama's output projection.

Both cameras and every selected chapter must agree. A different resolution,
mode, stabilization setting or lens prescription produces a warning and keeps
the Auto profile's fallback (rectilinear, automatic lens fitting by default).
Missing or unsupported metadata also explains the fallback in the input-lens
controls and CLI log. A single camera's prescription is never silently copied
onto an unknown second camera. Equal camera bodies alone are insufficient. Live V4L2/CSI input uses the Auto
fallback; recordings in its game directory cannot supply unrelated live optics.

Auto's imported numbers are read-only. Choose a named camera preset or **Manual
lens settings** to edit values. Existing named profiles remain manual, and older
explicit FOV/lens overrides without a camera name are retained as manual values.
Explicit CLI lens/FOV overrides also retain their values. A missing or null
camera selector does not discard older explicit optics. The `auto` profile in
`stitching.camera_configs` supplies fallback defaults; the factory profile stays
available for comparing Vegas against its earlier empirical Mission 1 preset.
The [camera lens catalog](camera-lens-catalog.md) also provides mode-specific
community measurements as optional refinement starting points. It does not
automatically substitute a database profile when metadata is unavailable.

Saving or playing uses the existing calibration invalidation rules when the
resolved optics change. The input geometry, scoreboard selection and crop review
must correspond to the rebuilt stitched canvas. Opening a game only populates
the controls; it does not write the game configuration. The CLI resolves Auto
before comparing with a saved alignment or persisting its calibration settings
and generation claim. Changed optics trigger a new solve, including launches
that also change output geometry.

Experiments freeze the displayed optics at candidate creation using
`stitching.auto_input_lens_frozen`. Replays and promotion retain that snapshot,
even if the original files are unavailable. To import again after promotion,
select another camera profile, then Auto, or explicitly pass
`--options=stitching.camera_config=auto` on the CLI. A normal save of other controls keeps
the freeze intact. Releasing the snapshot remains saveable when the optics are
unchanged, and Play saves the same policy as Save Preset.

## Supported metadata

| Source | Automatic import | Limitations |
| --- | --- | --- |
| GoPro `moov/udta/GPMF`, `FOVL` device | Wide radial `POLY` + `ZMPL`, with identity `MAPX/MAPY` and matching aspect ratios | Rejects AutoBoost and nonradial digital warps. Missing FOVL is a visible fallback. |
| DJI `djmd` metadata track | `dvtm_ac204.proto` / `dvtm_wm169.proto`, stabilization off, digital focal length plus four OpenCV fisheye coefficients | Stabilized output is not inferred from raw lens coefficients. Other schemas fall back. |
| Insta360 trailer, single-lens `offset_v3` | Unstabilized, unwarped v3 radial unified-camera/Brown model with equal focal scales, no tangential terms and no recording crop | FlowState, baked dewarping, v6/360 rigs, cropped/asymmetric models and inaccurate Hugin approximations fall back. |

This is capability detection from the file, not a promise that every recording
from those brands contains usable optics. The tested Laroche **DJI Osmo Action 5
Pro** files have RockSteady enabled and omit the focal/distortion fields. The
original **Insta360 Ace Pro 2** 8K clips in `stockton-r3` include v3 factory lens
coefficients but also report FlowState and baked dewarping (Mega FOV); that raw
prescription alone cannot describe their final pixels. Both currently use the
explained automatic-fit fallback. Remuxed Insta360 clips in `test` have lost the
vendor trailer entirely. No universal coefficients are guessed for these cases.

The Vegas **MISSION 1 PRO**, 7680×4320 Wide with HyperSmooth On, provides the
supported effective radial prescription. Conversion reproduces the separate
[factory preset](gopro-mission-1-factory-lens.md): Hugin HFOV 125.12273265°,
`a/b/c = [-0.00493188921, 0.04057437656, -0.02182075784]`, centered principal point.
Its physical horizontal coverage is about 120°. No additional guessed
HyperSmooth crop is applied. Static lens initialization does not compensate for
relative camera movement or replace alignment/synchronization/rink calibration.

The importer fits Hugin's radial polynomial and checks monotonicity, valid
ranges and a maximum residual equivalent to one pixel at 8K. It rejects models
that cannot meet this bound. The error bound measures conversion accuracy
against the embedded model, not real-world calibration accuracy.

## I/O and references

Metadata reads seek over media payloads; they never decode video, map GPU frames
or invoke Python/ExifTool. GoPro uses bounded MP4 headers; DJI seeks directly to
the first metadata sample through its sample table; Insta360 seeks to its small
metadata record while skipping gyro and thumbnail payloads. MP4 processing is
limited to 4,096 atoms, 4 MiB per read and 32 MiB total. The Insta360 reader also
limits directory entries to 4 MiB per file and rejects repeated record offsets.
Explicit left/right playlists skip directory discovery. An import checks at
most 256 chapters. A bounded process-local cache keys results by device/inode,
file size, mtime and ctime, so later loads reuse results and changed files are
re-read. Files on a slow or disconnected filesystem can still delay metadata
reads; there is no steady-state playback work.

Format references:

- [GoPro GPMF FOVL prescription](https://github.com/gopro/gpmf-parser#dvid-fovl-large-fov---lens-distortion)
- [DJI metadata definitions](https://github.com/AdrianEddy/telemetry-parser/blob/master/src/dji/dvtm_library.proto), `DigitalFocalLength`, `LensDistortionCoefficients`, `EisStatus`
- [Insta360 metadata fields](https://github.com/AdrianEddy/telemetry-parser/blob/master/src/insta360/extra_info.rs) and [v3 model conversion](https://github.com/AdrianEddy/telemetry-parser/blob/master/src/insta360/mod.rs)
- [Hugin lens correction model](https://hugin.sourceforge.io/docs/manual/Lens_correction_model.html)
