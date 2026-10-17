#pragma once

#include <gst/gst.h>

namespace hstream {
// External lifecycle operations run on the signaling main context; internal
// asynchronous EOS draining is serialized with them. Track registration finishes
// before PLAYING. Returned elements/pads are owned references, except
// create(), which follows the usual floating GstElement factory convention.
GstElement* webrtc_endpoint_create(const char* name, const char* stun_server);
GstPad* webrtc_endpoint_add_track(GstElement* endpoint, GstCaps* caps);
GstElement* webrtc_endpoint_reset_peer(GstElement* endpoint);
// Populate offer preferences from actual encoded input caps; false while waiting.
bool webrtc_endpoint_prepare_offer(GstElement* endpoint);
void webrtc_endpoint_detach_peer(GstElement* endpoint);
void webrtc_endpoint_set_active(GstElement* endpoint, bool active);
} // namespace hstream
