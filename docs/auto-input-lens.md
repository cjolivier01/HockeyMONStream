# Automatic input lens

New games select **Auto (detect recording mode)** in Stitched → Projection
and Stitching Experiments. The **Input cameras — shared calibration** section
separates two independent controls:

- **Auto camera FOV** uses valid MP4 axis coverage, then a verified matching
  recording-mode profile, then the selected profile's baseline defaults.
- **Auto lens correction** uses a supported MP4 lens prescription, then a
  verified matching mode profile, then the selected profile's baseline lens
  defaults (automatic fitting for the Auto profile).

Either can be turned off to retain/edit that component manually. Selecting Auto
or a named camera profile enables both Auto controls; the selected profile supplies
fallback values when metadata is unavailable. Selecting Manual turns both off.
Opening a saved configuration preserves its existing independent choices.
Source descriptions report MP4, mode profile,
baseline, manual values or a saved experiment snapshot independently. Missing
lens coefficients no longer discard valid recorded FOV. Unsupported dynamic
warps or ambiguous metadata still cannot establish trustworthy static coverage.

Both windows show a **MP4 recording metadata** panel with the camera model,
dimensions, frame rate, mode and stabilization when available. A teal background
and a **Read from MP4** note identify successfully read metadata. Recognizing the
camera does not imply its optics can be imported: the FOV and lens-value panels
are highlighted independently, and a partially imported FOV highlights only the
imported axis. The FOV note shows recorded coverage and any converted calibration
FOV; the lens controls show the converted projection, distortion and center
offsets actually used. Profile/default/manual/snapshot values stay untinted and
name their source, with an explanation when metadata is unusable. Incomplete or
inconsistent camera/chapter reads are reported explicitly. Saved snapshots and
fully manual settings say that the current MP4 files have not been checked.

**Calibration behavior** sits outside the highlighted lens values. It controls
whether stitching may refit them; it is an application policy, not an MP4 field.

**Fit panorama to camera coverage** controls the combined stitched view. It is
separate from both input-camera controls; one camera's FOV never sets the width
of the whole panorama. Camera horizontal FOV is the Hugin calibration parameter.
The source description shows physical MP4 coverage separately when distortion
makes the values differ. A supported metadata model retains its jointly fitted
FOV/coefficients. When physical MP4 FOV is combined with another correction, the
resolver converts it to that correction's normalization, checking radial
invertibility through the image corners. A failed conversion explains its
fallback rather than applying incompatible numbers.

Each camera's selected chapters must agree on recording mode and geometry.
When both cameras have complete supported metadata but differ, Auto retains
**independent camera optics**: separate FOV, projection, radial distortion and
optical-center offsets. The summary shows each source's prescription; shared
numeric controls are hidden while independent values are active.

**Use Left for both**, **Use Right for both**, and **Average for both
(approximation)** remain available to deliberately replace the independent pair
with one manual prescription. Before orientation is known, Camera 1/2 choices
identify their first chapters by filename. These actions change controls; Save
Preset or Play applies normal calibration invalidation. A camera with missing
metadata or inconsistent chapters cannot supply an automatic prescription or
shared choice. Averaging requires complete metadata, matching dimensions and
projection, and an invertible averaged radial model.

`stitching.input_cameras` stores two complete prescriptions keyed by each
camera's first selected chapter path, relative to the game when possible. The
empty baseline map retains shared `camera_fov`/`input_lens` behavior. The pair
replaces an inherited pair atomically; selecting a shared choice clears it.
The solve binds prescriptions to resolved Left/Right playlists, so discovery
order cannot swap their optics. A snapshot for different recordings fails
visibly and requires refreshing Auto. Manual components remain independent
when the other automatic component refreshes or falls back.

The NONA path writes independent PTO `f/v/a/b/c/d/e` values and explicit
optimizer variables for each camera. Fixed preserves its intrinsics; Refine
fits FOV, b and center; Auto can fit all lens terms. Existing Hugin tools support
this without a fork. Other mapping backends and rectified AKAZE profiles reject
independent prescriptions. Provenance versions 16/17 retain both source-bound
prescriptions through generation checks, experiments and reframing; versions
2–15 remain readable. This supports differing cameras, not optics that change
between chapters of one camera.

Missing files never allow
one camera's prescription to be copied onto an unknown second camera. Live
V4L2/CSI input uses baseline defaults. Reads are bounded, cached metadata I/O;
there is no video decoding or steady-state playback work.

The independent policies are `stitching.camera_automation.fov` and `.lens`.
Booleans explicitly select automatic/manual sources. Missing/null policies
retain the selector's legacy behavior (Auto imports both; other profiles
import neither). Existing legacy numerical overrides without a selector retain
manual intent. Higher-layer CLI numerical overrides disable only their own
automatic component when independent policies are in use; an explicit higher-
layer selector or automation policy can request a fresh import. Native geometry
keys `camera_fov` and `input_lens` retain their existing meanings.

Saving or playing uses the existing calibration invalidation rules when resolved
optics change. Opening a game only fills controls; it does not write the game.
Changing source policy alone remains saveable without invalidating identical
geometry. Experiments store both policies and freeze their resolved optics with
`stitching.auto_input_lens_frozen`, including mixed automatic/manual selections
under named profiles. Replay and promotion retain those values without original
media. Toggling an Auto control or selecting a profile releases the snapshot;
ordinary saves preserve it. CLI resolution precedes calibration generation
claims. Recalibrated geometry requires reviewing the crop and scoreboard region.

See the [camera lens catalog](camera-lens-catalog.md) for match rules, provenance
and coverage gaps. Automatic matching requires every declared constraint to be
present and equal; ambiguous matches and unknown recording modes use baseline
fallbacks. Profiles with incomplete provenance remain manual choices.

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
