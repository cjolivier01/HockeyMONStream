#pragma once

#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"
#include "hstream/src/apps/apps-common/PreviewTransportMonitor.h"

#include <json-glib/json-glib.h>

#include <atomic>
#include <functional>
#include <memory>

namespace hm::preview {

struct WebPreviewObserverConfig {
  RouteOwner::Session session;
  unsigned poll_interval_ms{0};
  std::size_t events_per_batch{0};
};

// Mutations belong to the signaling context. Its single periodic source
// polls the monitor; streaming threads never schedule context work. At most one
// frame batch awaits acknowledgment, bounding Soup's outgoing metadata backlog.
class WebPreviewObserver {
 public:
  WebPreviewObserver(std::shared_ptr<TransportMonitor> monitor, WebPreviewObserverConfig config);
  // Retire BEFORE any endpoint mutation, activate only after peer creation.
  bool Retire(std::uint64_t peer);
  bool Activate(std::uint64_t peer);
  std::optional<std::string> Poll();
  enum class Message { kUnrecognized, kRejected, kAcknowledged, kPresented };
  Message Handle(JsonObject* object);
  std::optional<PacketFrame> TakePresented();
  unsigned poll_interval_ms() const {
    return config_.poll_interval_ms;
  }
  std::uint64_t dropped_events() const {
    return dropped_events_;
  }
  std::uint64_t rejected_messages() const {
    return rejected_messages_;
  }
  // Thread-safe liveness only: accepted current-peer batch credit proves that
  // the receiver is responsive, never that a decoded frame was presented.
  std::uint64_t acknowledged_batches() const {
    return acknowledged_batches_.load(std::memory_order_relaxed);
  }

 private:
  bool Matches(JsonObject* object) const;
  std::shared_ptr<TransportMonitor> monitor_;
  const WebPreviewObserverConfig config_;
  std::uint64_t peer_{0}, next_batch_{0}, outstanding_batch_{0}, dropped_events_{0};
  std::uint64_t rejected_messages_{0};
  std::atomic<std::uint64_t> acknowledged_batches_{0};
  bool active_{false};
};

// Optional adapter for the existing signaling server. A preview target supplies
// these lifetime-fenced callbacks; signaling must never reset its endpoint
// directly. Ordinary static WebRTC sinks do not install this adapter.
struct WebPreviewSignaling {
  std::shared_ptr<WebPreviewObserver> observer;
  // Continue the target's generation sequence if it previously drained without
  // a browser. Signaling owns all subsequent peer mutations.
  std::uint64_t initial_peer_generation{0};
  std::function<GstElement*(std::uint64_t)> replace_peer;
  std::function<bool(std::uint64_t)> detach_peer;
};

} // namespace hm::preview
