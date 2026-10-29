# Scoreboard selector proposals

When `rink.scoreboard.perspective_polygon` is missing, the native selector
proposes a quadrilateral from the October 15 Mask2Former R50 scoreboard model
(best iteration 4000). The desktop dialog and browser show the same editable
draft. Only Save publishes it through the existing canvas-generation checks.
Existing selections, including the four-zero disabled sentinel, skip model
lookup, download and inference. This includes games that saved only a native
`scoreboard-perspective-polygon`/`scoreboard_perspective_polygon` CSV property
under `pipeline.hmplaycropper`, `properties`, or `private-properties`.
Reselecting a saved polygon starts from that polygon. No confident detection
leaves an empty editor; model/download errors are reported in the run log and
editor without disabling manual selection.

`ScoreboardSelector::Run` owns this one-shot work on the existing selector
worker before announcing its private URL. It uses the already pinned `s.png`
snapshot, not live video. A bounded color proxy feeds a fixed 2048x768 RGB
canvas with ImageNet normalization and normalized zero padding. The model
returns 100 class/mask queries. Only the ten best classification candidates
are expanded, one at a time, at canvas resolution. The best mask with combined
class/mask confidence at least 0.5 is fitted to a convex quadrilateral and
mapped back to the original panorama. The model may select a different
display from the operator in arenas with multiple boards; review is required.

CUDA is selected when both a visible CUDA device and the ONNX CUDA provider
exist. Otherwise inference uses CPU. `hm::onnx::Session` releases a failing
CUDA session and retries once on CPU after GPU allocation exhaustion, during
either session creation or inference. Other inference errors are surfaced;
they do not trigger a CPU retry. Stop cancels asset transfer and inference.
The session and its GPU arena are destroyed before the selector URL is shown.
Saved-polygon playback performs no new work, and no frame processing path
loads the model, copies video to the CPU, or polls for suggestions.

The native asset is downloaded on demand to
`~/.cache/hstream/models/scoreboard-mask2former-r50-3d7fc57ac7ab4513.onnx`.
The existing asset manager verifies its full SHA-256 and publishes it
atomically. `HM_SCOREBOARD_ONNX_MODEL` selects an explicit local model for
development/offline use; it must have the same tensor contract. HockeyMON
uses the matching portable TorchScript export with its existing PyTorch
runtime, so Python installations do not need ONNX Runtime or MMDetection.
Both exports come from `scripts/export_scoreboard_model.py`; the adjacent
`scoreboard-model.json` records source-checkpoint identity and export parity.

The authenticated `GET /initial?token=...` endpoint carries draft points and
status to Qt. A response cannot replace points after the operator starts
editing or submits. The browser embeds the same draft and fetches its status.
Neither response persists a polygon.

The desktop and browser display the same bounded still geometry from
`CalibrationImage::scoreboardPreviewSize`: at most 8192 pixels per side and
96 MiB of RGBA pixels, with both dimensions rounded down after proportional
scaling. Selection coordinates remain in the original `s.png` canvas. PNG
proxies use the existing row decoder and midpoint sampling, with one source
row plus the bounded output allocation; no full panorama bitmap or video
readback is required. Native input limits remain 65535 per side and 256 Mi
pixels. As in Qt, small PNGs use `QImageReader` (including interlaced images);
reduced PNGs use the row decoder, which rejects interlacing.

The CPU-only `hstream-web-job --scoreboard-preview REQUEST.json` entry point
advertises `scoreboard-preview-v1`. Its request is limited to 64 KiB and uses
schema `hstream-scoreboard-preview-request-v1`, a 32-character lowercase hex
`token`, absolute owned-run `root`, relative `source` ending in `s.png`, and
`source_identity` containing decimal-string `device`, `inode`, `size`,
`mtime_ns`, and `ctime_ns`. The helper opens every path component without
following symlinks, pins the original descriptor, and accepts sources up to
2 GiB. It verifies that both the descriptor and current pathname still match
the request before publishing.

The helper publishes `scoreboard-preview-TOKEN.png` and then
`scoreboard-preview-TOKEN.json` atomically without replacing existing files.
The receipt schema `hstream-scoreboard-preview-v1` repeats the token, root,
source and source identity; `source_size` holds original coordinates and
`preview_size` holds proxy dimensions. It binds the image basename, exact
`image_identity` and encoded PNG `sha256`. Encoded images are bounded to
128 MiB. A failed publication removes files created by that attempt; the
outer supervisor owns cancellation, deadline and remaining run cleanup.
Consumers must reject stale tokens, changed source/image identities and
dimension/hash mismatches before exposing editor readiness. Image HTTP reads
serve an already validated proxy and never start this helper. This mode does
not read saved-calibration completion metadata or substitute a TIFF for the
current selector's PNG.

Validation on an RTX 4090, `2025-2026/chicago-3`: native model load plus
inference/postprocessing took 1.5 seconds on CUDA and 4.2 seconds on CPU.
The Python path, including panorama preparation and loading, took 4.5 seconds.
These are one-shot setup measurements, not video-throughput claims. Export
parity against the original raw heads had maximum absolute error 0.00056.
The proposal, selector protocol, Qt edit-race, and ONNX OOM tests cover the
runtime contracts without downloading models during ordinary unit tests.
