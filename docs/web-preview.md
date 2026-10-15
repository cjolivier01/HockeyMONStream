# WebRTC preview lifecycle

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
the GStreamer OpenH264, Opus, WebRTC and libnice plugins. Press Enter in the server
terminal to exercise graceful stop. This fixture is test-only and is not part of
the installed runtime.
