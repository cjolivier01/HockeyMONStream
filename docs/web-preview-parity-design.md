# Proposed Qt and browser preview parity

**Status: production browser route transport is unimplemented; shared native routing, GPU surface and identity primitives exist.** This document describes the remaining native
transport work needed for HockeyMONWeb to reproduce Qt preview routing and
playback behavior. It is not a description of shipped capabilities. The current
browser workspace and generated controls do not establish full workflow parity.

This is a substantial change to native presentation and runtime ownership. It
must preserve one processing graph, the existing Qt behavior, archive contents,
and bounded GPU memory. It should be implemented as shared native mechanisms with
presentation adapters, not a second processing implementation in the web service.

The outer service remains in [HockeyMONWeb](https://github.com/cjolivier01/HockeyMONWeb),
locally `../HockeyMOMWeb`. Its `docs/qt-web-parity.md` records the broader remaining
UI requirements. This design concerns live Program/Stitched/source routing,
preview resource enablement, seek/jump, restart, and the presentation boundary
needed by experiment frame stepping. It does not complete the rink, highlights,
experiment-authoring, defaults, or alternate-backend ports.

## Current implementation and constraints

| Responsibility | Current owner and behavior |
| --- | --- |
| Logical preview branches | `src/apps/hstream-cli/PipelineApp.cpp`, `configure_source_preview_sinks()`, builds them only when `--ui-preview-windows` supplies X11 window IDs. Program taps the output tee, tracked Stitched taps the graph before Program crop, and source previews tap the native source tees. Calibration-only Stitched uses the stitcher's preview branch. |
| Route activation | `src/apps/apps-common/PreviewRouteOwner.*` owns the active channel and a monotonically increasing operation generation. `PipelineApplication::set_preview_active_runtime()` delegates to it using the existing gates/renderers and retains Qt's acknowledgments. Typed C++ admission can additionally fence run/stage/topology; this is not yet exposed as a browser runtime capability. |
| Selected stream | `SelectedPreviewStream.*` isolates each route's caps, segment, flush and EOF and supplies one presentation clock. It retains the exact original GPU memory and source buffer, including native metadata and source PTS. The main-output route owns EOF. This element is tested independently but is not registered by the production CLI. |
| Owned browser surface | `PreviewGpuSurface.*` fits the selected NVMM frame into a fixed RGBA GPU surface, applies a supplied painter, and bounds cadence and surface ownership using explicit caller configuration. Pool exhaustion drops preview frames while sources continue. CUDA-device input is currently required; Jetson surface-array and AMD consumers are not implemented. |
| Encoded identity | `PreviewTransportFrames.*` bounds frame-to-RTP bookkeeping, independently fences peer and route operations, and rejects stale/delta output until an epoch's first keyframe. A separate bounded `PreviewPresentationHistory` admits exact client-reported receipts through the optional observer. Browser matching and production signaling integration remain unimplemented. |
| Qt presentation | `src/apps/apps-common/HmGpuPreview.*` owns `hmpreviewisolation` and the x86 NVIDIA/OpenGL `hmgpupreviewsink`. The renderer reports readiness after presentation. `HStreamWindow` waits for matching channel/generation messages and handles retries and failures. |
| Browser presentation | `create_webrtc_sink_bin()` in `src/apps/apps-common/deepstream_sink_bin.cpp` creates a separate H.264 encoder, RTP payloader, and `WebRtcEndpoint`. This sink is connected to the normal output tee. It is not a consumer of the desktop's selectable preview routes. |
| Unexposed GPU target | `WebPreviewTarget.*` assembles the selector, owned CUDA surface, hardware H.264 encoder, RTP identity probes and stable endpoint. `PreviewTransportMonitor.*` bounds polled sender events. The GPU fixture uses this adapter; an optional `WebPreviewObserver` connects bounded metadata/receipt handling to the existing signaling server. Production graph taps, browser matching, complete overlay drawing and audio timeline remain unimplemented. |
| WebRTC lifecycle | `WebRtcEndpoint.*` retains stable RTP input pads while replacing the inner peer transport. Signaling callbacks are fenced by peer generation and run on their owning main context. Disconnected inputs drain without blocking processing. See [web-preview.md](web-preview.md). |
| Main-window seek | `PipelineApp.cpp` permits local-render-only playback. It rejects network/file outputs, active telemetry capture, unavailable source duration, unsupported sources, and conflicting operations. Its source-replacement path retains the GPU processing graph and resets source/tracker state. Its failure and shutdown paths assume no network output. |
| Main-window restart | `HStreamWindow::restartStage()` stops the current process, waits for cleanup/finalization, verifies the same game/run generation, then starts again. It is a supervisor operation, not a CLI stdin restart command. |
| Frame stepping | The main Qt window has seek/jump controls. Exact frame stepping belongs to `CameraExperimentPreview`, which seeks a recorded frame index and confirms generation-matched presentation. It is not implemented by rounding an arbitrary seek to a nominal frame duration. |
| Web runtime bridge | HockeyMONWeb's `tools/webapp/django_app/stream/interactive.py` validates owner/session-bound inspector commands and forwards finish/pause/resume. It does not yet admit preview-route/seek commands or consume their native acknowledgments. |
| AMD | Native AMD Program preview exists, but source/Stitched channels and WebRTC output remain gaps. The web must report the selected backend's actual capabilities. See [amd-gpu.md](amd-gpu.md). |

Two tempting short changes are insufficient. Allowing `@set-preview-active`
through the web bridge does not create routes in a headless WebRTC run. Adding
WebRTC to the current local-only seek allowlist leaves transport timestamps,
signaling ownership, and reconstruction teardown unresolved. Raw uploaded-file
playback does not reproduce the native source route's timing or ownership.

## Shared route and presentation ownership

`PreviewRouteOwner` now owns activation independently of Qt window types, with
borrowed native gate and consumer callbacks. Its CPU tests cover registration
without XIDs, generation/session admission, switching, metadata ordering,
consumer failure, graph replacement and in-flight drain barriers.
`RouteAttachment` in `PreviewRouteGraph.*` now owns transactional attachment at
those exact native taps, without window IDs. The Qt path uses this factory,
and the GPU fixture uses it to bridge Program/source taps into the shared
`WebPreviewTarget`. Production browser route registration remains unimplemented.
Presentation adapters supply the final consumer:

- Qt uses the existing `hmgpupreviewsink` targets and X11 readiness contract.
- Browser sessions use one route selector and one hardware encoder/WebRTC target.

The existing Program, tracked Stitched, calibration-only Stitched, and source tap
positions remain authoritative. Route selection must not create another decoder,
tracker, stitching graph, or source synchronization implementation. Preserve the
native immutable overlay snapshots and source/output geometry provenance at each
tap. Missing source channels report unavailable instead of selecting another
camera silently.

Preserve both preview isolation gates. Closing the ingress prevents additional
queued frames; closing the post-queue barrier waits for already-admitted work.
The common route owner must additionally control which route's sticky caps,
segments, flushes, and EOS reach the shared browser consumer. The current isolation
element forwards downstream events even when inactive, so it cannot by itself
prevent an inactive route's EOS from ending a selected route.

The browser branch should be:

```text
selected native GPU route
  -> bounded preview conversion/letterbox on the GPU
  -> preview-only overlay composition on an owned GPU surface
  -> one hardware H.264 encoder
  -> RTP payloader
  -> existing WebRtcEndpoint
```

`SelectedPreviewStream` supplies the routing primitive. A stock `input-selector`
allowed inactive EOF to block behind a selected stream, so local event ownership
is explicit. CPU regression tests cover inactive EOS during an admitted blocked
frame, authoritative EOF, source replacement, exact memory references and source
pool retention. The native owner owns switching and drain ordering. Keep output caps
fixed for the encoder so changing source dimensions/aspect does not require a new
SDP negotiation. Preserve aspect through GPU letterboxing. Force an IDR after a
route or transport epoch change. Gate inactive routes before GPU conversion, and
bound the active queue and encoder backlog.

The current WebRTC sink can fall back to software encoding. The new GPU preview
path must require a compatible hardware encoder and report unavailable when one
cannot be created. It must not silently introduce system-memory raw-video caps,
CPU image conversion, screen capture, or a continuous snapshot path.

All new shipped preview dimensions, rates, queue budgets and policies must be
resolved from `configs/baseline.yaml`, synchronized with HockeyMOM's baseline.
Do not create an independent web or compiled default set. Existing sink settings
must be reconciled with that source while preserving unrelated launch behavior.

## Shared preview drawing

`HmGpuPreview.cpp` currently prepares preview paths, analytics, and rink-map
commands and draws them in GL. Those final GL overlays are not present in the
ordinary WebRTC encoder input.

Extract transport-independent overlay preparation from
`collect_preview_overlays()` into shared native code. Reuse
`src/libs/common/PreviewOverlayMeta.*`, `src/libs/draw_display/PlayerOverlays.*`,
`RinkMiniMap.*`, and the portable `AnalyticsOverlay` compositor. The desktop GL
adapter and browser GPU compositor must consume the same prepared drawing and
coordinate transforms, including baked-layer suppression and stale geometry
checks.

The browser adapter must compose into an owned bounded preview surface. It may
not mutate a borrowed processing/archive surface shared across a tee. Immutable
metadata work is permitted; full-video readback is not required. Preserve the
current generation fences, producer enablement, and backend-specific drawing
availability. No archive diagnostics should appear merely because a preview tab
or checkbox changed.

## Runtime and presentation contract

Use a typed native command/reply contract shared by Qt and the web bridge. Extend
existing handlers rather than duplicating activation and seek state machines.
Commands must bind:

- the supervisor run identity;
- native stage and topology generation;
- request ID;
- a monotonically increasing operation generation;
- the validated action and logical channel or seek target.

Topology, preview, seek, and WebRTC peer generations are different identities.
Do not conflate them. Web request IDs are currently random, so they cannot serve
as the preview generation's monotonically increasing counter. Native validation
must occur when a command is applied, not only when an HTTP request is admitted.
The bridge should revalidate queued work and retire pending commands on native
session replacement. Browser clients never submit raw stdin, paths, XIDs,
signaling addresses, encoder settings, or arbitrary element names for this API.

Return versioned structured events for capability/availability, activation,
deactivation, failure, seek completion, and playback position. Keep compatibility
with existing desktop commands while migrating both clients to the shared
contract. A capability response must describe the running executable/backend
and current run constraints, not infer availability from installed source files.

There are distinct completion points:

1. **Accepted:** a request passed session and argument validation.
2. **Activated:** the native route switch completed its gates and barriers.
3. **Encoded:** a current-generation frame reached the preview encoder output.
4. **Presented:** the client displayed a frame from that generation.

An activated route is not yet visible video. Keep the browser preview covered
until a current-generation frame is confirmed, so delayed old-route frames cannot
appear under the new tab label. One possible browser presentation adapter uses a
bounded mapping from native frame identity to encoded RTP timestamp, then matches
`requestVideoFrameCallback()` metadata. This mechanism still needs implementation
and browser qualification. Feature-detect the required metadata; an ordinary
video `playing` event is insufficient evidence of frame identity. Sender readiness
and client presentation must remain separately observable.

Use the existing authenticated signaling gateway. Any presentation metadata added
to its protocol needs bounded schemas, run/peer fencing, and explicit admission.
Keep one active encoder and peer per preview session. Peer replacement remains
owned by `WebRtcEndpoint` and its signaling context.

## Seek, clocks and cancellation

First extend the source-only seek path for a monitor-only browser run. Reuse
`seek_runtime_sources()`, the exact-pair/chapter planners, tracker seeding/reset,
and `AsyncInferenceSeekRecovery`. Keep rejection of archives, publishing network
outputs, and telemetry capture. A browser monitor must be distinguishable from a
network publication sink in admission policy.

The replacement source epoch starts at PTS zero. The preview transport therefore
needs its own continuous video/audio output timeline while retaining source time
in metadata and progress reports. Route switching, pause/resume, and seek must
not cause RTP time to move backward or leave audio on a previous epoch. Native
unrestricted processing can sample source frames much faster than source-time
playback; the browser presentation timeline must represent delivered preview
cadence rather than accumulate playback delay from skipped source time.

The Qt **Play at normal speed** option also controls processing pace. Separate
that shared playback policy from X11 target existence when adding the browser
adapter. Keep next-run versus live behavior identical and do not accidentally
pace archive-only jobs through a new preview branch.

Preserve the browser encoder/endpoint across source-only replacement when their
ownership permits it. Rebind recreated source routes under the same route owner.
Before allowing full-reconstruction fallback, update every timeout, worker-start
failure, interruption, EOS, and shutdown path that currently assumes local-only
sinks. Retire signaling on its owning context before destroying affected graph
state. Do not wait on that context from a reconstruction worker that prevents it
from making progress.

Until a reconstruction case is implemented and tested, advertise it as unavailable
with the actual reason. That is an interim limitation, not completion of full
parity. Match Qt's disabled states and paused-seek policy; do not silently execute
a different seek while the UI implies the native operation succeeded.

## Restart and experiment stepping

Implement **Restart Stage** as a durable web-supervisor transaction. Capture the
owner-bound restart intent and resolved launch snapshot, request the current run's
appropriate stop, wait for every descendant and required publication/cleanup,
then admit one replacement. Bind the intent to the project and old run generation,
and make duplicate requests/retries idempotent. Cancellation or a changed project
must retire the intent. Do not launch the next process from an optimistic browser
callback. When archive routing is enabled, preserve Qt's required finalization
semantics before restart.

For Camera Experiments, attach the same browser presentation target to
`CameraExperimentPreview`. Expose its existing `Seek(index, play)` and confirmed
frame identity through the workflow supervisor. Preserve range/loop, original
versus trial comparison, telemetry/geometry binding, and paused one-frame
presentation. A nominal frame-duration jump in the main CLI is not a replacement
for recorded-frame stepping.

## Implementation order and acceptance

| Increment | Required result before proceeding |
| --- | --- |
| Shared route owner | Qt routes, inactive resource use, readiness, overlays and seek regressions remain unchanged. Native route registration works without XIDs. |
| Browser target | One hardware encoder consumes selected GPU routes; switching and `none` are fenced; no CPU raw-frame path; browser presentation identity is checked. |
| Shared runtime contract | Qt and web use matching capability, generation, acknowledgment and failure behavior. Old or wrong-run commands cannot mutate state. |
| Browser source-only seek | Continuous preview audio/video transport and exact native source placement survive forward/backward and repeated seeks, including cancellation and disconnected browsers. |
| Reconstruction and restart | Required fallback cases have safe signaling ownership; restart waits for containment and publication/cleanup and launches exactly once. |
| Experiment transport | Recorded-frame step, paused inspection, range/loop and trial comparison use the existing native engine and matching presentation receipts. |
| Backend qualification | Supported NVIDIA/AMD capabilities work on hardware; unsupported native channels remain explicitly disabled in both clients. |

## Validation matrix

| Layer | Existing fixtures and required extensions |
| --- | --- |
| Native route lifecycle | Extend `hm_gpu_preview_test` and overlay tests with shared-owner admission, gate/drain ordering, unavailable routes, stale generations, inactive EOS/caps, zero admitted frames after disable, and failed-consumer isolation. |
| WebRTC transport | Extend `webrtc_endpoint_test` for route/seek epoch changes while retaining stable RTP pads, peer replacement, disconnected EOS, incomplete negotiation, and pause/replacement teardown. |
| Actual browser decoding | Extend `webrtc_preview_test_server` with two distinguishable test patterns and route/epoch messages. Verify pointer-independent route identity, old-frame suppression, audio, repeated switches, disconnect/reconnect, and Stop. CPU test media is a fixture, not a production fallback. |
| Native runtime seek | Extend `pipeline_runtime_control_test` for monitor-only browser admission and retained-transport source recreation. Retain callback-race, timeout, worker failure, interruption, tracker reset, source chapter, and first-completed-frame checks. Verify archives and telemetry still reject seek. |
| Qt regressions | Run `hstream_ui_test`, affected preview/overlay tests, and `camera_experiment_dialog_test`. Compare the same native revision and matching source/configuration fixtures. Use X11 for tests requiring native window IDs. |
| Web service and browser | Cover ownership, bounded command schemas, queued stale requests, session/peer replacement, acknowledgment timeouts, unavailable controls, duplicate restart, restart cancellation, and cleanup before replacement admission. Run real browser JavaScript, not only template assertions. |
| NVIDIA hardware | On `meshy` or `monster`, exercise Program/Stitched/source routes, rapid switching, Pipeline/Render-off, paused transitions, calibration, source chapter changes, seek, reconnect, and Stop during seek. Compare source/output frame identities and audio continuity. Measure GPU memory, conversion/encoder work, and inactive-route cost. |
| Archive independence | Run the same processing/output recipe with preview off and with repeated route/overlay changes. Compare decoded frame content, timestamps and audio duration within documented encoder determinism limits; verify publication identities and no preview diagnostics in archives. |
| AMD hardware | On `ripper`, use VA/HIP surfaces and VA hardware H.264 into the shared RTP endpoint when implemented. Qualify Program behavior and shared protocol tests. Source/Stitched/WebRTC and drawing gaps must remain explicit until their actual native implementations pass. |
| Platform builds | Build optimized x86, native ARM64/SBSA, and Jetson targets appropriate to the new shared components. Do not treat `--config=arm64` on x86 as ARM64 validation. Record unavailable hardware separately from successful compilation. |

Hardware validation must confirm that raw video remains GPU-resident. Inspect
negotiated memory caps and conversion/encoder paths, and measure the new preview
branch with the existing archive graph active. Readback for an explicitly requested
bounded diagnostic image remains separate from steady-state presentation.

Update [web-preview.md](web-preview.md), [amd-gpu.md](amd-gpu.md), the repository
architecture map, the generated web contract, and HockeyMONWeb's comparison gate
as each increment is implemented. Keep this document marked proposed until its
owners and behavior actually exist; move verified ownership details into the
implementation documentation rather than silently changing the status label.
