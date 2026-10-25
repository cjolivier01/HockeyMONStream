# WebRTC preview lifecycle

The remaining work to share Qt's selectable preview routes and playback controls
with the browser is described in [web-preview-parity-design.md](web-preview-parity-design.md).
The route activation owner is shared native code; the browser route consumer and
presentation contract remain unimplemented. The transport lifecycle below still
describes the current static WebRTC output.

`PreviewRouteOwner` owns logical route selection and its operation generation.
The desktop CLI registers its existing gates and renderer callbacks, retaining
the legacy `HSTREAM_PREVIEW` acknowledgments. Selection closes ingress before
waiting at the drain barrier, quiesces the old consumer, enables immutable
metadata, then opens the new drain and ingress. Same-channel recovery only
re-arms readiness; it must not wait behind first-frame negotiation. Retiring a
graph forgets borrowed route callbacks before destroying their native objects.

The owner is independent of XIDs and GPU APIs. Its typed request additionally
binds a run, stage, topology, request ID and operation generation, with validation
before mutation. This C++ admission API is not yet a stdin/HTTP capability: the
CLI still constructs production routes only for `--ui-preview-windows`, and no
headless selectable WebRTC target is registered. `preview_route_owner_test`
exercises the owner without a display, including an in-flight drain barrier;
`hm_gpu_preview_test` retains the actual GStreamer gate/failure tests.

`SelectedPreviewStream` isolates inactive route events, preserves original NVMM
memory and source-buffer pool ownership, and assigns a monotonic presentation
clock without changing source PTS. `PreviewGpuSurface` bounds cadence and pool
ownership, fits differing source shapes into one GPU RGBA canvas, and lends only
that owned surface to a painter. All dimensions, rates and pool sizes must be
passed explicitly; no new default set is defined by these APIs. Its GPU fixture
uses CPU patterns only at source creation, then keeps raw frames on the GPU
through conversion and hardware encoding. It verifies route changes, Render off,
native metadata, source PTS, owned surface separation and pool exhaustion without
stalling source progress. A four-byte test-only readback checks cleared bars.
`PreviewTransportFrames` separately bounds frame/encoded/RTP identity and fences
route and peer epochs. These primitives are not yet connected to production
headless routes or authenticated browser presentation acknowledgments. Shared
overlay preparation alone does not implement GPU drawing of all Qt layers.

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
a newer operation generation. Production signaling is not wired to this API yet.

`WebPreviewTarget::ReplacePeer` and `DetachPeer` own compressed-output retirement;
callers must not reset or detach its borrowed endpoint directly. `PreviewRtpGate`
holds a barrier from RTP identity admission through the complete downstream
push. Replacement first closes that gate without waiting, then detaches/NULLs
the old endpoint to release downstream waits. Only after accepted pushes return
does it retire metadata and construct the new peer under the barrier. Closing
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
This target remains unexposed: no production graph registration, defaults,
browser messages, audio timeline, complete overlay painter, seek support or
connected-browser qualification is added by the adapter.

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
