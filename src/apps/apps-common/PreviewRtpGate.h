#pragma once

#include "hstream/src/apps/apps-common/PreviewTransportMonitor.h"

#include <functional>
#include <memory>

namespace hm::preview {

// Serializes RTP admission through the entire downstream push. Pad probes alone
// cannot fence peer replacement after accepting a packet but before forwarding.
GstElement* rtp_gate_create(const char* name, std::shared_ptr<TransportMonitor> monitor);
// Lifecycle owner calls these in order: BeginTransition, release old downstream
// waits (NULL/detach), FinishTransition. Begin never waits on an in-flight push.
// Finish waits for it to return, retires metadata, then performs the replacement
// under the output barrier. A mutation must not wait for upstream frame progress.
bool rtp_gate_begin_transition(GstElement* gate, std::uint64_t peer);
bool rtp_gate_finish_transition(GstElement* gate, std::uint64_t peer, const std::function<bool()>& mutation);
// Connection notifications never wait on flow. The control closes the downstream
// endpoint immediately, then a single coalesced worker drains accepted pushes,
// renews IDR recovery and calls control(true) before reopening this gate. The
// control must be nonblocking and reject stale peer/connection revisions.
bool rtp_gate_set_ready(GstElement* gate, std::uint64_t peer, bool ready, std::function<bool(bool)> control);
bool rtp_gate_is_ready(GstElement* gate, std::uint64_t peer);
// After parent NULL, retire requests and join the bounded readiness worker.
void rtp_gate_stop(GstElement* gate);

} // namespace hm::preview
