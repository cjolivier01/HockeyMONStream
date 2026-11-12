# Canonical camera and playback settings

`configs/baseline.yaml` owns the shipped runtime defaults. The CLI resolves them
through baseline, user Defaults, game and launch layers; opening the desktop is
not required. The browser contract and Qt controls read those same keys.

| Canonical setting | Meaning |
| --- | --- |
| `rink.camera.motion_overrides.max_speed_x/y` | Absolute translation speed limit; zero keeps configured camera/arena tuning. |
| `rink.camera.motion_overrides.max_accel_x/y` | Absolute translation acceleration limit; zero keeps configured tuning. |
| `rink.camera.motion_overrides.apply_to_fast_box` / `apply_to_follower_box` | Select the boxes receiving the override set. |
| `video_out.tone.shadow_lift_percent` | Native shadow lift percentage. |
| `video_out.tone.lift_shadow_black_point` | Include the black point in shadow lifting. |
| `video_out.tone.exposure` | Native exposure adjustment, 0 through 1.3; this is not exposure stops. |
| `playback.start_seconds` | Nonnegative synchronized playback start in seconds. |
| `playback.realtime` | Pace interactive playback; archive-only jobs remain unpaced. |
| `telemetry.enabled` | Initial Program capture selection and managed-job default. |

The four `rink.camera.max_speed_ratio_*` / `max_accel_ratio_*` keys remain
multipliers. They affect translation and resizing before absolute per-box native
limits. Motion overrides affect translation only, on the selected fast/follower
roles. Explicit native tracker-file values retain same-layer precedence; a
higher-layer canonical limit overrides them. Zero inherits the native value.

`common/RuntimeControlConfig` normalizes old UI and AMD aliases within each input
layer before merging. A legacy game preset therefore overrides canonical user
defaults. An explicit canonical key wins a same-layer tie. Reads do not rewrite
saved files. Qt saves canonical values; motion-only edits no longer generate a
tracker sidecar, and tone edits no longer duplicate native plugin properties.
Old sidecar references remain supported for other tracker tuning.

Widget IDs and the live protocol stay unchanged. Speed/acceleration sliders use
ten times the native value; exposure uses one hundred times the native value.
The shared contract carries the canonical path and divisor. Browser saved layers
migrate before inheritance and validation, and acknowledged live values convert
back to native units when Save is requested. Live edits still require the
existing run/generation acknowledgments and do not rewrite admitted snapshots.

`--start-time` and `--render-realtime` remain explicit launch overrides. The UI
also sends the session's pacing choice explicitly, so an unchecked control can
override a true baseline preference. Separate Camera Experiments preferences,
preview/encoded rink maps, dialog policy and control timing remain independent.

HockeyMON shares the byte-identical baseline, layer migration and native-unit
motion overrides. Its existing tone adapter converts native exposure to its
Python EV representation with the established factor of one half.

Validation covers layer precedence, native units, saved presets, launch arguments,
legacy tone snapshots and calibration-reference inheritance. The full x86 build,
Jetson CLI cross-build and focused ROCm configuration/GPU tests passed. Jetson
hardware was unreachable, and the full AMD application build remains blocked by
the toolchain include failure recorded in `amd-gpu.md`. These checks do not claim
new backend workflow support.
