# WebRTC preview lifecycle

The remaining work to share Qt's selectable preview routes and playback controls
with the browser is described in [web-preview-parity-design.md](web-preview-parity-design.md).
The route activation owner is shared native code. An explicit private
`--web-preview-run-id=<UUID>` now installs selectable native routes and the
presentation contract; ordinary `WEBRTC` launches retain the static output.
This opt-in is not full Qt workflow parity: archived replay-mask ownership and
audio/seek timeline qualification remain unfinished.

`PreviewRouteOwner` owns logical route selection and its operation generation.
The desktop CLI registers its existing gates and renderer callbacks, retaining
the legacy `HSTREAM_PREVIEW` acknowledgments. Selection closes ingress before
waiting at the drain barrier, quiesces the old consumer, enables immutable
metadata, then opens the new drain and ingress. Same-channel recovery only
re-arms readiness; it must not wait behind first-frame negotiation. Retiring a
graph forgets borrowed route callbacks before destroying their native objects.

The owner is independent of XIDs and GPU APIs. Its typed request additionally
binds a run, stage, topology, request ID and operation generation, with validation
before mutation. Qt uses `--ui-preview-windows`; the private browser path uses
the same native taps and gates without XIDs. `preview_route_owner_test`
exercises the owner without a display, including an in-flight drain barrier;
`hm_gpu_preview_test` retains the actual GStreamer gate/failure tests.

`hstream-cli/WebPreviewSession.*` prepares one target before graph construction.
The selected WEBRTC sink slot adopts its bin and stable endpoint, so audio can
attach its existing Opus/RTP track before signaling starts. The sink builder
does not create the old video encoder, a duplicate Program branch, or a fake
fallback for that slot. After graph construction, the session attaches Program,
Stitched and available source branches, chooses the initial route and starts
signaling. Teardown retires signaling, closes every route gate, stops/disposes
the native graph, then releases the target. Calibration reconstruction creates
a fresh target and topology; local seeking remains rejected for WEBRTC outputs.

The launch requires exactly one untiled native context and one non-demux
WEBRTC sink on source 0, with an explicit signaling port. It cannot share Qt
window targets or offline sampling. `configs/baseline.yaml` owns every new
`web_preview` budget and initial selection. Shared `preview_reference` dimensions
size the Qt GPU rendering surfaces and browser overlay coordinate extent; neither
interface supplies independent geometry defaults. The hardware encoder requires
at least eight conversion-pool buffers to avoid startup starvation; the native
configuration rejects smaller pools and bounds total surface memory.
The resolved browser receipt limits
are sent with the capability; the outer service must not supply independent
defaults or enable this launch before its own deployment qualification.

After the matching inspector session has been published, stdout emits:

```text
HSTREAM_PREVIEW_PRESENTATION {"version":1,"run":"UUID","stage":0,"topology":"1","operation":"1","channel":"program","channels":["program","stitched","source0","source1"],"overlays":{"generation":"0","players":false,"play":false,"rink":false},"width":1600,"height":900,"seek":false,"receiptLimits":{"maxFrames":512,"maxAgeMs":3000}}
```

`channels` contains only installed routes. Width/height are even and bounded
by 3840×2160; source indices are 0..127. The supervisor sends:

```text
@preview-route {"version":1,"run":"UUID","stage":0,"topology":"1","requestId":"1234","operation":"2","channel":"stitched"}
```

The `HSTREAM_PREVIEW_ROUTE` JSON reply echoes the request identity, operation
and channel, with `status` (`activated`, `deactivated`, `unavailable`, `stale`
or `failed`) and `reason`. Topology/operation are exact decimal strings;
request IDs contain at most 64 ASCII token characters. No route changes on a
wrong run/stage/topology or an older operation. An activation acknowledgment
does not establish browser presentation: the existing `preview-frames` /
`preview-presented` exchange must match an actual decoded callback and receiver
RTP timestamp/SSRC before uncovering the video.

An authenticated local HockeyMONWeb run qualified this path against native
`9b43282a` and web `292ed06`: Program, Stitched, both source routes, Render
off/on, rapid tab changes, peer replacement and normal Stop passed. Native
counters reported 102 presented frames, zero rejected presentations and zero
probe failures. This used the production browser, authenticated signaling
gateway, Slurm supervisor and original camera media. Calibration inputs/maps
were reused after a precision change; their bytes and timestamps stayed the
same and no dependency invalidation ran. Audio synchronization, seek, calibration
reconstruction remain separate qualifications. Live diagnostics have their own
comparison described below; the earlier authenticated route run did not test them.

`RouteAttachment` in `PreviewRouteGraph.*` separates graph attachment from the
consumer. The current Qt path uses its existing Program output tee, inserts a
tracked Stitched tee after the selected tracking/analytics producer, uses the
calibration stitcher's output tee, and replaces each source's original fake
sink while retaining its queue and tee request pad. It does not add a second
Program tee or move processing, archive or overlay-producer stages.

Attachment is a transaction while the tap's parent is stopped: new nodes,
links, requested pads and any cross-bin ghost pads remain provisional until
configuration and route-callback publication succeed. Failure restores the
original tracked/source links and source queue settings, then removes partial
nodes/pads. Committed objects belong to their graph parents; owners still
retire borrowed route callbacks before destroying/replacing those parents.
The same factory accepts an external shared-target sink pad and bridges only
the necessary ancestors. Calibration uses runtime-only `preview_tap_only` to
request the existing stitcher output tee without allocating a Qt sink there;
legacy standalone YAML `ui-preview` continues to construct its own renderer.
CPU tests cover commit, rejected publication, broken consumer links, source
queue/pad identity, archive preservation and nested shared-target rollback.
Moving the final source drain also refreshes ancestor sink flags: already-parented
GStreamer bins otherwise retain a phantom EOS sink. A CPU graph test requires
actual pipeline EOS after that nested source replacement.
The NVIDIA hardware fixture exercises the same factory with shared-target
Program/source switches and natural EOF. Real Qt checks cover Program,
paused Stitched selection, source-only seek, Render off during seek, restart
and Program reactivation. The calibration-only tap still needs its own runtime
check; these attachment tests do not qualify AMD consumers.

`SelectedPreviewStream` isolates inactive route events, preserves original NVMM
memory and source-buffer pool ownership, and assigns a monotonic presentation
clock without changing source PTS. `PreviewGpuSurface` bounds cadence and pool
ownership, fits differing source shapes into one GPU RGBA canvas, and lends only
that owned surface to a painter. After fitting, `OpaqueVideoCanvas` clears bars
and forces alpha255 in one shared CUDA/HIP pass, replacing the preceding memset.
Qt displays RGB video without source transparency; preserving alpha0 panorama
corners would make a later translucent watermark incorrectly become full red.
Only the owned canvas changes, and source/archive alpha remains untouched.
All dimensions, rates and pool sizes must be
passed explicitly; no new default set is defined by these APIs. Its GPU fixture
uses CPU patterns only at source creation, then keeps raw frames on the GPU
through conversion and hardware encoding. It verifies route changes, Render off,
native metadata, source PTS, owned surface separation and pool exhaustion without
stalling source progress. Bounded test-only readbacks check cleared bars, opaque
owned pixels and unchanged alpha0 source pixels. `opaque_video_canvas_test` also
checks partial-alpha inputs, RGB/pitch preservation, invalid geometry and the
transparent-corner overlay blend. Its 100-frame 1080p GPU timing on 2026-10-25
was 0.0045 ms/frame on NVIDIA and 0.0400 ms/frame on AMD; these are primitive
measurements, not end-to-end browser latency.
`PreviewTransportFrames` separately bounds frame/encoded/RTP identity and fences
route and peer epochs. The opt-in session connects them to native headless
routes; authenticated gateway and browser deployment require their separate
behavior gate. Shared overlay preparation alone does not implement every Qt layer.

`PreviewOverlayPainter` now consumes that immutable scene on an owned RGBA
video subimage. It maps analytics/glyphs from their source coordinate extent,
draws native diagnostic paths through the shared Qt GL pass, samples a
generation/canvas-bound rink mask using the native inverse Program transform,
and draws the Stitched watermark last with the desktop converter's negotiated
dimensions. Optional overlay capacity cannot suppress the required watermark.
Static masks upload once per immutable asset; video never returns to the CPU.
The CUDA/HIP mask kernel uses bilinear sampling and a transparent border, with
work confined to the destination video rectangle. Test-only readback compares
these passes against Qt's unchanged OpenGL rendering (maximum channel error 2
for analytics and 1 for mask RGB), including rotated/cropped masks and extreme
panorama coordinate extents.

The opt-in route painter draws analytics and the Stitched watermark on the
owned subimage, using the actual source CAPS and desktop reference dimensions.
Player boxes, Play tracking and Rink mask use the same immutable scene builder
and producer-selection transaction as Qt. The three startup values come from
required `preview_overlays.players/play/rink` settings. The explicit launcher
environment `HSTREAM_UI_PREVIEW_OVERLAYS=players,play,rink` replaces that selection;
an empty value disables all three, while an absent value preserves the resolved
baseline. Qt's top status bar reads the same settings and sends its explicit
selection on launch. AMD reads the same required settings but still rejects
unsupported Play/Rink diagnostics.

Qt defers an overlay change made during startup, including Render-on, until
`HSTREAM_PREVIEW_RUNTIME status=ready`. It preserves the latest checkbox choices
without claiming success or starting an acknowledgment timeout early; readiness
starts the ordinary native transaction and timeout.

The private browser supervisor changes all three selections atomically:

```text
@preview-overlays {"version":1,"run":"UUID","stage":0,"topology":"1","requestId":"overlay-1","generation":"1","players":true,"play":false,"rink":true}
HSTREAM_PREVIEW_OVERLAY_STATE {"version":1,"run":"UUID","stage":0,"topology":"1","requestId":"overlay-1","generation":"1","status":"applied","reason":"","state":{"generation":"1","players":true,"play":false,"rink":true}}
@preview-overlays-query {"version":1,"run":"UUID","stage":0,"topology":"1","requestId":"query-1"}
HSTREAM_PREVIEW_OVERLAY_STATE {"version":1,"run":"UUID","stage":0,"topology":"1","requestId":"query-1","status":"state","reason":"","state":{"generation":"1","players":true,"play":false,"rink":true}}
```

Both commands require the exact run/stage/topology and a bounded request token.
Mutation generations are positive decimal uint64 strings, independent of route
operations; initial committed generation is `"0"`. The parser rejects extra,
missing, duplicate or mistyped fields. A stale session/generation returns
`stale`; an absent/stopped session returns `unavailable`; producer/readback
failure returns `failed`. Every correlated reply contains the complete last
committed `state`; failures never advance it. Queries do not mutate or replay
anything. Malformed commands have no correlated reply. The legacy unscoped
`set-preview-overlays` command is rejected in opted-in browser sessions.

Qt and the browser register the same dsplaytracker producer or bounded fallback
snapshot probe. Route selection reconciles producer flags before opening gates;
source/Render-off routes stop optional snapshot production while retaining the
operator's selected flags. Only a successful producer transaction publishes the
browser painter's atomic three-flag snapshot. An applied selection acknowledges
configuration, not a displayed frame: masks still require the live frame's valid
crop, stitched canvas and calibration generation. The bounded saved PNG loader
runs under the same artifact ownership checks as Qt and retains immutable alpha
bytes per identity; retries use the existing bounded backoff. It reads only the
static mask, never video. The GPU painter validates that identity on every frame
and writes only the owned preview surface. Archived replay masks remain separate.

`PreviewOverlayGlPaths` is the unchanged desktop `GL_POLYGON`/`GL_LINE_*`
drawing pass. `GpuGlSurface` runs it in a private headless EGL compatibility
context on the selected CUDA/HIP device, preserving mixed fill/stroke order and
display-pixel line widths. Two RGBA textures flip GL/video row orientation with
GPU blits; two D2D transfers move only the owned preview subimage. No source or
archive surface is writable through this API. The textures total at most
64 MiB (8,388,608 pixels); a route-size change replaces the owner after the
caller completes the previous stream. Serialized producer threads may migrate;
concurrent/reentrant access is rejected, and the prior EGL/GLX context is restored.
Interop capability is checked before mask, analytics or watermark writes.
Unavailable interop returns `kInteropUnavailable`, with unchanged video and no
software fallback. Other GPU/GL failures retire the owner; its caller discards
the unpublished preview and completes the stream before destruction.

The NVIDIA GLX-versus-EGL oracle compares 3,720 paths across 60 scenes, including
clipped strokes of widths 1–8, filled paths, alpha/order, extreme panorama
coordinates and offset letterbox viewports: RGB matches byte-for-byte. A 1080p
100-frame measurement took 356 microseconds per shared-GL pass versus 33 for
the portable rounded-line compositor; the exact native path uses 16,588,800
texture bytes and copies that many device bytes per frame. These timings include
stream synchronization and no video readback. AMD compilation succeeds, but
`ripper`'s ROCm/Mesa stack rejects `hipGraphicsGLRegisterImage`; the expected
unavailable regression checks that video and every other overlay stay unchanged.
See `docs/amd-gpu.md` for this explicit runtime gap.

Live NVIDIA qualification compared Player boxes → Player boxes + Play tracking →
all three layers → all off on both Program and Stitched. Qt required complete live
acknowledgments and bounded framebuffer captures; Chromium required matching set
and query state plus new decoded-frame receipts before each capture. The browser
run also rejected stale topology/generation and the unscoped legacy command,
then switched source/off/on routes, replaced its peer and stopped cleanly:
153 native presented receipts, zero rejected receipts and zero probe failures.
The shared GPU painter oracle passed its native GL path, immutable-mask identity,
crop mapping and letterbox guard checks. The media captures are different source
frames, so they demonstrate live behavior and geometry, not frame-aligned lossy
encode equivalence. CPU protocol tests run on NVIDIA and AMD configurations;
x86/AMD CLI and Jetson CLI cross-builds pass. No Jetson runtime or AMD browser
consumer is qualified by these checks.

Archived replay-mask ownership and exhaustive encoded-browser overlay comparisons
remain unfinished. Live control/capture checks do not establish frame-aligned
lossy-encoder equality for arbitrary media. These pixel
and generation-fencing tests do not establish full browser preview parity.

`WebPreviewTarget` assembles those primitives into an independently owned bin:
one selected stream, bounded CUDA-device RGBA surfaces, GPU NV12 conversion,
hardware H.264 with B frames disabled, Annex-B access units, RTP, and a stable
`WebRtcEndpoint`. All dimensions, cadence, pools, metadata budgets, bitrate,
profile, tuning, payload and STUN settings are explicit constructor inputs.
Missing hardware elements fail construction; there is no software fallback.
The endpoint is borrowed through the target for the audio owner's later track
attachment. Its initial drain must exist before parenting the endpoint so the
target participates in its parent's EOS aggregation.

`PreviewTransportMonitor` owns bounded sender events and counters. Route off,
active-route removal and peer replacement retire delayed output immediately.
Events distinguish encoded access units from completed RTP frames; neither is a
browser presentation receipt. Streaming probes only queue metadata for polling,
with observable overflow; they do not call signaling or schedule work per frame.
The caller closes ingress/drain gates around route mutations and stops the
parent pipeline before destroying the target. A different active channel needs
a newer operation generation. The private opt-in session creates its signaling
adapter only after every native route is attached.

`WebPreviewTarget::ReplacePeer` and `DetachPeer` own compressed-output retirement;
callers must not reset or detach its borrowed endpoint directly. `PreviewRtpGate`
holds a barrier from RTP identity admission through the complete downstream
push. Replacement first closes that gate without waiting, then detaches/NULLs
the old endpoint to release downstream waits. Receipt history and the peer
generation retire before touching the endpoint. After accepted pushes return,
it also discards work admitted during teardown and constructs the new peer
under the barrier. Closing
raw ingress or clearing the identity ledger alone cannot protect a packet that
already passed admission but has not reached its destination.

The target opts into managed endpoint readiness: connection notifications close
the endpoint valves and request one coalesced readiness worker. That worker
drains the output barrier, retires every frame encoded before readiness, then
opens the endpoint and target together. A newly admitted IDR is required. This
also prevents a delta accepted before a same-peer disconnect from crossing a
reopened valve. Deferred requests carry both the peer identity and endpoint
connection revision; stale requests cannot reopen either gate. Legacy static
WebRTC outputs retain their automatic connection gating.

Connection callbacks never wait on the output barrier, since transport teardown
may be joining them. After the parent reaches NULL, target destruction retires
readiness requests and joins the bounded worker; deferred endpoint/peer bindings
are weak and refuse reopening during teardown. CPU regressions hold admitted
packets across replacement and same-peer readiness changes, and negotiate real
in-process WebRTC peers to check legacy/managed valve behavior. The GPU fixture
also replaces a peer during active output. These checks do not establish browser
presentation parity.

The GPU fixture now uses this target directly. It covers live source-binding
replacement, Render off, natural main-output EOF, incomplete peer teardown,
surface-pool exhaustion and event-queue overflow alongside native metadata and
exact source/presentation/RTP identity. CPU monitor tests exercise compressed
reference loss, delayed RTP markers, route/peer retirement and bounded events.
A real Chromium run against the opt-in native CLI additionally checks Program →
Stitched → source0 → source1 → none → Program, rejects a stale topology, replaces
the peer, and requires exact decoded presentation receipts plus clean EOS/Stop.
The x86 NVIDIA run on 2026-10-25 produced 31 accepted presentation receipts, zero
rejected receipts and zero probe failures. It observed the shared Opus track,
but does not establish audio synchronization, paced playback, calibration
reconstruction, Jetson runtime, authenticated gateway behavior or full Qt parity.

The optional `WebPreviewTarget::Signaling` adapter delegates peer changes through
those lifecycle methods. `start_webrtc_signaling_for_preview` uses the existing
Soup server and one periodic source attached to its signaling context. The
source drains bounded `PreviewTransportMonitor` events; no streaming thread
creates per-frame idle callbacks. `WebPreviewObserver` emits at most one
unacknowledged metadata batch. While a client withholds credit, subsequent
sender events are discarded with a counter, bounding the outgoing metadata
backlog as well as the native queue. Poll cadence and event budget are explicit
constructor inputs resolved from the opt-in session baseline.

`PreviewPresentationHistory` retains completed RTP identities independently of
the pending encoder/RTP ledger. Its explicit capacity evicts oldest entries.
A presentation receipt must match the current run, stage, topology, peer,
channel, operation, sequence, RTP timestamp and SSRC. Run/topology/peer fencing
is checked on the signaling context; the monitor atomically checks history and
route/recovery retirement. Duplicate and backward receipts are rejected. A
single latest accepted observation can be consumed through `TakePresented`;
route off, replacement and recovery also clear this pending observation. An
accepted receipt is a client's claim matched to a packetized frame, not an
independent measurement of displayed pixels.

The version-1 optional metadata protocol is:

- Server `preview-frames`: `version`, `run`, `stage`, `topology`, `peer`, `batch`,
  and a bounded `events` array. Each event has `boundary` (`encoded` or
  `packetized`), `channel`, `operation`, `sequence`, `sourcePts` and
  `presentationPts`. Packetized events additionally carry `rtpTimestamp` and
  `ssrc`.
- Client `preview-frames-ack`: exactly `type`, `version`, `run`, `stage`,
  `topology`, `peer` and `batch`. This returns one batch of signaling credit;
  it does not report video presentation.
- Client `preview-presented`: exactly `type`, `version`, `run`, `stage`,
  `topology`, `peer`, `channel`, `operation`, `sequence`, `rtpTimestamp`, `ssrc`
  and `observation: "video-frame-callback"`. The browser must first match
  `requestVideoFrameCallback` RTP metadata to a signaled packetized identity.
  `ontrack`, `playing`, or a successful credit acknowledgment are insufficient.

Unsigned 64-bit identities/timestamps use decimal strings to avoid JavaScript
number truncation; 32-bit RTP fields and the signed stage use JSON integers.
Receipts are bounded to 4096 bytes; the optional connection's overall incoming
limit is 256 KiB for SDP. Unknown receipt fields/types and stale identities fail
admission. Native admission tests do not implement the browser matcher or its
covered-preview behavior. The authenticated outer gateway has not admitted this
optional protocol yet. Keep that integration and real decoded-frame comparison
as explicit gates before claiming presentation or Qt parity.

`web_preview_observer_test` checks history after marker retirement, loss/overflow,
RTP wrap, exact 64-bit identities, strict receipts, Render off, peer retirement
and reconnect recovery. `web_preview_signaling_test` connects real Soup
WebSocket clients to the production handlers, checks one outstanding metadata
batch, delegated replacement ordering and removal of the single drain source
on stop. Its frame identities and receipts are synthetic; it is a signaling
ownership test, not browser decoding qualification.

`WebRtcEndpoint` exposes stable RTP ghost pads for the video and audio branches.
Each browser WebSocket receives a new inner `webrtcbin`, including fresh SDP,
ICE and DTLS state. Reusing a previous transport after a browser reload is not
supported. Disconnected inputs drain to nonblocking sinks; connecting inputs
remain gated until the new transport reports connected. This does not copy or
map video frames: the endpoint only handles already-encoded RTP.

The native sender initiates SDP negotiation. After all encoded input caps are
available, it sends `{"type":"offer","sdp":"..."}`; the browser applies it and
returns `{"type":"answer","sdp":"..."}`. ICE messages retain `type: candidate`,
`candidate`, and `sdpMLineIndex`. Both sides queue candidates until the remote
description is installed. Offering the actual encoder payload IDs and H.264
profile/level avoids browser-selected RTP mappings and packet repayloading. A
browser connecting during calibration waits for real caps before negotiation.
The web client and native sender must be deployed together; browser-initiated
offers are no longer the signaling protocol.

Signaling and explicit peer replacement run on the signaling main context.
Promise and ICE callbacks retain their origin and dispatch back to that context;
callbacks from a retired generation cannot answer or send candidates to its
replacement. The existing runtime-seek worker accepts local render sinks only.
`prepare_webrtc_sinks_for_stop(pipeline)` retires the matching signaling servers
before source retirement or EOS. Natural EOF is handled by the endpoint's
serialized asynchronous drain transition, retaining ended-track state when audio
and video finish at different times.

The CPU regression test exercises stable pads, repeated replacement, video/audio
codec preferences, detached EOS, and EOF during incomplete negotiation:

```bash
bazelisk test --config=opt //src/apps/apps-common:webrtc_endpoint_test
```

For real browser decoding and reconnect acceptance without allocating a GPU:

```bash
bazelisk run --config=opt //src/apps/apps-common:webrtc_preview_test_server -- 18080
```

Open `http://127.0.0.1:18080/`, verify moving video and audio, then reload or open a
new browser session and verify playback again. The fixture generates CPU-only
H.264 and Opus test media and uses the production signaling handlers. It requires
the GStreamer OpenH264, Opus, WebRTC, DTLS/SRTP and libnice plugins. A missing
`libsrtp2.so` can leave the WebRTC plugin discoverable while negotiation fails;
verify `gst-inspect-1.0 srtpenc` and `gst-inspect-1.0 srtpdec` on the compute host.
After repairing a missing library, an old plugin registry may still blacklist
SRTP. Use a fresh private `GST_REGISTRY` for verification; Bazel tests accept it
through `--test_env=GST_REGISTRY=/tmp/hstream-webrtc-test-registry.bin`.
Press Enter in the server
terminal to exercise graceful stop. This fixture is test-only and is not part of
the installed runtime.

For exact browser frame matching, add
`--presentation --run RUN_UUID --stage 0 --topology 1` after the fixture port.
This mode sends video only, admits identities from the actual raw encoder-input
PTS, and monitors the actual H.264 access units and RTP packets. It uses the
managed peer/readiness barrier and optional observer protocol described above.
Match browser `requestVideoFrameCallback` RTP timestamps with receiver
`getSynchronizationSources()` RTP timestamp/SSRC and the packetized event;
accepted native receipts appear as `WEBRTC_TEST_PRESENTED` JSON on stdout.

The fixture's stdin accepts `route program`, `route stitched`, `off`, `recover`
and `quit`. Route commands change the identity attached to this one test-pattern
stream and increment its operation; they do not implement native Program or
Stitched processing. Use this mode to qualify browser decoding, identity
matching, stale-frame rejection and peer replacement. Production route ordering,
overlay composition, audio and Qt workflow parity require their own tests.
