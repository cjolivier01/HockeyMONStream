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
// Connection notifications do not wait on downstream flow. Opening retires any
// encoded reference chain produced while disconnected; only a new IDR can pass.
// Stale notifications cannot open a replacement peer's gate.
bool rtp_gate_set_ready(GstElement* gate, std::uint64_t peer, bool ready);

} // namespace hm::preview
