# Camera lens catalog

Leave **Auto (video metadata)** selected when the original recording contains a
supported lens prescription. The catalog supplies verified mode fallbacks for missing or unsupported metadata
and optional manual comparisons in Stitching Experiments.
Both camera menus read `stitching.camera_configs` from the shared baseline; no
network request or video decoding is needed to load the catalog.
The complete input-lens correction is implemented by native HStream. HockeyMON
keeps the shared baseline synchronized, but its Python stitcher currently only
resolves these presets' FOVs and does not apply `input_lens`.

The entries labeled **measured** are approximations of community calibrations
from the [Gyroflow lens database](https://github.com/gyroflow/lens_profiles/tree/886fc616c9c2c788856f54c21d451b9cb7fc4f44),
not manufacturer factory specifications. Each selects `optimization: refine`
so stitching can adjust the starting lens. Use the **NONA** mapping backend for
these input-lens corrections. Auto checks explicit `metadata_match` constraints before choosing a fallback.
Manually selecting a named profile remains an operator choice; choose the matching
recording mode and recalibrate. Existing experiments retain their saved optics. Changing the lens
of a completed game invalidates its calibration and stitched pixel coordinates,
including scoreboard selection.

## Included recording modes

| Camera | Resolution and frame rate | Digital lens / stabilization |
| --- | --- | --- |
| GoPro HERO11 Black | 3840×2160, 23.976 / 25 / 29.97 / 50 / 59.94 / 100 / 119.88 fps | Wide, HyperSmooth Off |
| GoPro HERO11 Black | 3840×2160, 23.976 / 25 / 29.97 / 50 / 59.94 / 100 / 119.88 fps | Wide, standard HS EIS |
| GoPro HERO11 Black | 5312×4648 (8:7), 23.976 / 25 / 29.97 fps | Wide, HyperSmooth Off |
| GoPro HERO11 Black | 5312×4648 (8:7), 23.976 / 25 / 29.97 fps | Wide, standard HS EIS |
| Original DJI Osmo Action | 3840×2160, 29.97003 fps | Wide, RockSteady Off, dewarp Off |
| Insta360 ONE RS | 5312×2988, 25 fps | UltraWide 110, EIS Off; source does not identify the lens module |
| iPhone 13 Pro | 1920×1080, 30.005781 fps | Built-in 13mm ultrawide, stabilization Off; recording app unspecified |

The four HERO11 records cover 20 explicit mode combinations without duplicating
coefficient sets. `equivalent_identifiers` names each pinned upstream recipe;
the converter verifies that every recipe produces the same effective matrix and
distortion. Metadata constraints include exact model, dimensions, frame rate
(three decimal places), Wide, stabilization, horizon Off, zoom 1, GPRO projection
and identity warp. Missing settings cannot match, and an explicit lens accessory
requires an explicitly qualified profile. The Mission 1 reference also matches
its recorded firmware. Frame-rate alternatives are not a general license to
resize a profile or substitute an unlisted mode. Firmware is unspecified by the
community HERO11 sources; their matches remain refinement seeds.

The original DJI profile, ONE RS and iPhone records remain manual choices:
this importer cannot establish all required recording-mode constraints for them.
A named profile can still provide baseline defaults independently of Auto FOV.

The ONE RS and iPhone entries are experimental starting points because those
source details are missing. Their menu names expose that uncertainty. Firmware
is unspecified in all seven source modes. Do not assume a profile applies after
changing aspect ratio, digital lens, zoom, stabilization, horizon control,
firmware or an interchangeable lens. The GoPro HS entries use the database's
explicit compatibility recipes, not an estimated stabilization crop. They do
not cover AutoBoost, HyperView, SuperView or lens mods. HERO12/13 are not silently
aliased to HERO11 here.

The existing **GoPro Mission 1 Factory (8K Wide, HyperSmooth On)** remains a useful
fallback when the original metadata is lost. It is derived from the actual
Mission 1 recording prescription, with its own
[documented settings and conversion](gopro-mission-1-factory-lens.md). The
empirical Vegas profile remains separate. The old generic HERO11 and Ace Pro 2
entries are labeled **legacy auto fit**: their neutral values were never measured
factory calibrations. Their identifiers and numerical values remain compatible
with existing games.

## Gaps in coverage

There is no verified profile in this catalog for the user's **Action 5 Pro with
RockSteady** or **Ace Pro 2 8K with FlowState and Mega FOV**. The pinned upstream
database has neither camera body. Its older Ace Pro UltraWide NO-EIS calibration
also converts poorly to the current Hugin model (over 20 pixels at an equivalent
7680-pixel width), so it was excluded. ONE RS is not a substitute for Ace Pro 2,
and original Osmo Action is not a substitute for Action 5 Pro. For these gaps,
keep Auto's explained automatic-fitting fallback or use a calibration measured
from the exact mode.

Manufacturer FOV/focal-length specifications alone are insufficient to populate
the missing distortion coefficients. Metadata availability is also different
from applicability to final stabilized pixels:

- GoPro's supported `FOVL` prescription can describe the recording's effective
  lens geometry. The Vegas MP4s include `XFOV`, `YFOV`, and `ZFOV`; their actual
  horizontal/vertical coverage is about 120.00°/70.37°. Hugin's corresponding
  horizontal parameter is 125.12° and must be used with its distortion values.
- Some DJI modes provide digital focal length and distortion coefficients from
  which axis FOVs can be derived. The inspected Action 5 Pro RockSteady files
  omit those fields. The current importer requires supported unstabilized modes.
- Insta360 files can contain raw calibration and a FOV mode without describing
  the final FlowState/dewarped output. The inspected Ace Pro 2 files have this
  limitation.
- Apple's capture APIs expose FOV/intrinsics, but that does not establish that
  ordinary Camera-app MOV files retain a complete usable lens prescription.
  HStream does not currently import iPhone lens metadata.

See [Auto input lens](auto-input-lens.md) for the actual importer support matrix.

## Provenance and reproducible conversion

`configs/camera-lenses/catalog.json` records each source path, SHA-256, upstream
revision, selected compatibility identifier and recording-mode limitations.
Unmodified source JSON files preserve the calibrator, calibration date, image
count, camera matrix, distortion coefficients and reported RMS. They are
redistributed under the accompanying `LICENSE-CC0`. The database's `official`
flag means Gyroflow-maintained; it does not mean manufacturer-provided data.

The offline standard-library Python converter emits the marked catalog block
inside `configs/baseline.yaml`. Playback only reads those ordinary presets; the
converter and original profile database are not runtime dependencies.

Debian packages install the baseline, catalog manifest, original profiles and
CC0 license under `/opt/hstream/configs/`. Packaging checks that the staged
camera data matches the checkout. The Windows installer installs this same
Debian payload inside WSL and checks that the baseline, manifest and license
are present before reporting success. Playback does not download the catalog
or run its Python converter on either platform.

```bash
python3 scripts/convert_camera_lens_catalog.py --check
python3 scripts/convert_camera_lens_catalog.py --write --report /tmp/lens-catalog-report.json
```

When updating profiles, synchronize the resulting baseline byte-for-byte with
HockeyMON's `hmlib/config/baseline.yaml`. Update the pinned sources and their
hashes deliberately; do not automatically download whichever upstream revision
is newest.

Conversion follows the effective profile geometry: Gyroflow centers the optical
axis when `asymmetrical` is false or absent, then divides focal scales by the
explicit compatibility crop. The two HERO11 HS recipes use 0.928 (16:9) and
0.918 (8:7). Output dimensions in the source profile are stabilization export
settings, not the recorded input dimensions. All included variants retain their
source calibration dimensions.

For an OpenCV fisheye model, normalized image radius is
`theta * (1 + k1*theta² + k2*theta⁴ + k3*theta⁶ + k4*theta⁸)`.
The converter fits Hugin's four-term radial polynomial using the mean focal
scale. It uses equidistant base radius for action cameras and rectilinear
`tan(theta)` for this already corrected iPhone output. Hugin's focal scale and
`a/b/c` must be converted together; copying OpenCV coefficients into Hugin would
be wrong. Vertical FOV is retained as a coverage estimate; Hugin does not enforce
it independently.

The report separates source calibration RMS from conversion error. Conversion
checks include monotonicity through the image corners, a dense radial sample,
full two-dimensional image sampling, and a conservative allowance for unequal
horizontal/vertical focal scales. The catalog rejects a conversion bound above
8 pixels at an equivalent 7680-pixel width. This is a limit for optional
**refinement seeds**, not the stricter one-pixel limit used by Auto for a fixed
metadata prescription. Neither error measures accuracy on a different camera
body or recording mode, and neither justifies rescaling a profile to a new mode
without evidence.

Sources:

- [Pinned Gyroflow database and CC0 license](https://github.com/gyroflow/lens_profiles/tree/886fc616c9c2c788856f54c21d451b9cb7fc4f44).
- [Effective Gyroflow matrix/crop semantics](https://github.com/gyroflow/gyroflow/blob/f6811242087e25fe9cd7f52b4c056047229215ed/src/core/lens_profile.rs#L275).
- [Hugin lens model](https://hugin.sourceforge.io/docs/manual/Lens_correction_model.html).
- [GoPro prescription specification](https://github.com/gopro/gpmf-parser#dvid-fovl-large-fov---lens-distortion).
- [DJI supported metadata modes](https://github.com/gyroflow/docs.gyroflow.xyz/blob/main/getting-started/supported-cameras/dji.md).
- [Insta360 Ace Pro 2 digital FOV modes](https://onlinemanual.insta360.com/acepro2/en-us/operation-tutorials/shoot-preview/shooting-parameters/fov).
- [Apple capture FOV](https://developer.apple.com/documentation/avfoundation/avcapturedevice/format/videofieldofview) and [intrinsic-matrix delivery](https://developer.apple.com/documentation/avfoundation/avcaptureconnection/iscameraintrinsicmatrixdeliveryenabled).
