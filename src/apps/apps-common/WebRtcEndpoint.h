#pragma once

#include <gst/gst.h>
#include <cstdint>

namespace hstream {
// External lifecycle operations run on the signaling main context; internal
// asynchronous EOS draining is serialized with them. Track registration finishes
// before PLAYING. Returned elements/pads are owned references, except
// create(), which follows the usual floating GstElement factory convention.
// Managed readiness snapshots/application are thread-safe nonstructural updates;
// the readiness worker may apply them after draining its upstream RTP boundary.
// RTP and RTCP sender reports share pipeline running time across Pause/Resume.
// Replacing the pipeline clock origin requires retiring the peer first.
GstElement* webrtc_endpoint_create(const char* name, const char* stun_server);
GstPad* webrtc_endpoint_add_track(GstElement* endpoint, GstCaps* caps);
GstElement* webrtc_endpoint_reset_peer(GstElement* endpoint);
// Populate offer preferences from actual encoded input caps; false while waiting.
bool webrtc_endpoint_prepare_offer(GstElement* endpoint);
void webrtc_endpoint_detach_peer(GstElement* endpoint);
void webrtc_endpoint_set_active(GstElement* endpoint, bool active);
// Opt in before peer creation. Managed connection notifications close valves;
// only a revision-matched owner may reopen them after draining its RTP boundary.
bool webrtc_endpoint_manage_readiness(GstElement* endpoint);
bool webrtc_endpoint_connection_snapshot(
    GstElement* endpoint,
    GstElement* peer,
    std::uint64_t* revision,
    bool* connected);
bool webrtc_endpoint_apply_readiness(GstElement* endpoint, GstElement* peer, std::uint64_t revision, bool ready);
} // namespace hstream
