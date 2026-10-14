# GoPro Mission 1 Factory input lens

`stitching.camera_configs.gopro-mission-1-factory` is a fixed Hugin approximation
of the GoPro lens prescription embedded in the original `vegas-kings-1`
recordings `GX010046.MP4` and `GX010078.MP4`. Both headers contain identical
values. It is separate from the empirical `gopro-mission-1` preset and does not
replace the empirical preset. New games default to Auto (video metadata).
Both desktop camera selectors load the factory preset from the baseline;
restart an already open desktop app to reload the preset list.

The name "Factory" identifies the source of the lens model. GoPro publishes the
metadata format, not these converted Hugin coefficients or a guarantee of
calibration accuracy on every camera body.

## Recording and metadata

| Field | Value |
| --- | --- |
| Camera / firmware | MISSION 1 PRO / H26.01.02.02.70 |
| Resolution / frame rate | 7680×4320 (16:9) / 60000÷1001 fps |
| Digital lens / projection | Wide / GPRO |
| Stabilization | Enabled, HS EIS (HyperSmooth On) |
| Horizon control / digital zoom | Off / 1 |
| AutoBoost score (`ABSC`) | 0 (use the large-FOV prescription) |
| Horizontal / vertical / diagonal FOV | 120.000427246094° / 70.3657608032227° / 135.511917114258° |
| Zoom normalization (`ZMPL`) | 0.476107776165009 |
| Unwarped / warped aspect ratios | Both 1.77777779102325 |
| Digital warp (`MAPX`, `MAPY`) | Identity |

`POLY`, in ascending powers of radius, is:

```text
[0, 2.67053532600403, 0.22922171652317, -2.08499050140381,
 1.64104437828064, -7.24889386797406e-13, 4.47891317133589e-13, 0]
```

## Conversion to Hugin

For pixel radius `rho` measured from image center, let
`D = hypot(width, height) / 2`, `R = min(width, height) / 2`, and
`theta = POLY(ZMPL * rho / D)` in radians. Wide has no additional digital warp.
Hugin full-frame fisheye uses `t = width * theta / (radians(hfov) * R)` and
`rho / R = a*t^4 + b*t^3 + c*t^2 + (1-a-b-c)*t`.

Fit `rho/R = q1*theta + q2*theta^2 + q3*theta^3 + q4*theta^4` by least squares
over 20,001 equally spaced pixel radii from zero through `D`. Solve the fitted
polynomial for `theta0` where `rho/R = 1`; then `hfov = degrees(width*theta0/R)`,
`a = q4*theta0^4`, `b = q3*theta0^3`, and `c = q2*theta0^2`.
This gives the baseline's 125.12273264898167° Hugin FOV parameter and a/b/c
coefficients. The centered radial prescription supplies no measured principal
point offset, so the preset uses `[0, 0]`.

Against 100,001 radii, the fit has 0.1553-pixel RMS and 0.6523-pixel maximum
radial error at 8K. Checks through `pano_trafo` agree with that mapping. This is
conversion error relative to the embedded polynomial, not measured scene error.
The polynomial predicts 120.0144° horizontal, 70.3814° vertical, and 135.5303°
diagonal coverage, within 0.019° of the camera's reported FOV fields. The Hugin
horizontal parameter is not the final physical horizontal coverage; its radial
coefficients must be used with it. The preset retains the reported vertical FOV,
which the Hugin path does not independently enforce.

## HyperSmooth and automatic selection

GoPro documents `POLY`, `ZMPL`, FOV, aspect-ratio and digital-warp fields as the
lens prescription. These describe the recording mode's effective lens geometry;
do not apply an additional guessed HyperSmooth crop. `ABSC` selects or blends
large/small FOV prescriptions for AutoBoost. A single static preset cannot model
varying AutoBoost prescriptions or the cameras' relative orientation changes.
`CORI` and `IORI` separately describe camera and image orientation over time.

The original MP4 metadata is enough to initialize this mode's input lens without
asking the operator to remember recording settings. It does not replace camera
alignment, synchronization, rink leveling or crop selection. HStream's Auto
camera selection now imports this prescription, checking every selected camera
chapter before applying one shared calibration. Missing/unsupported metadata
uses automatic fitting with a visible explanation; different camera settings
produce a warning. See [automatic input lens](auto-input-lens.md) for support
limits, manual overrides and experiment snapshots.

To compare in Stitching Experiments, select the factory camera profile and add a
new candidate. Existing candidates retain their original optics. Changing a main
game's input lens requires recalibration and a new scoreboard selection because
stitched pixel coordinates change.

Sources:

- [GoPro GPMF metadata specification](https://github.com/gopro/gpmf-parser#dvid-fovl-large-fov---lens-distortion)
- [GoPro Mission mode controls](https://gopro.github.io/labs/control/mission/): Off, On and AutoBoost
- [Gyroflow telemetry parser's GoPro lens model](https://github.com/AdrianEddy/telemetry-parser/blob/master/src/gopro/lens_profile.rs): radius normalization and centered principal point
- [Hugin lens correction model](https://hugin.sourceforge.io/docs/manual/Lens_correction_model.html)
