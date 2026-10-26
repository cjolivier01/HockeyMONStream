#pragma once

#include "hstream/src/apps/apps-common/PreviewGpuSurface.h"
#include "hstream/src/apps/apps-common/PreviewTransportMonitor.h"
#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"
#include "hstream/src/apps/apps-common/WebPreviewObserver.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace hm::preview {

struct WebPreviewConfig {
  std::string name, stream_id, eof_channel;
  GpuSurfaceConfig surface;
  std::uint32_t conversion_pool_frames{0}, bitrate{0}, idr_interval{0};
  std::optional<std::uint32_t> h264_profile;
  std::optional<std::uint32_t> encoder_tuning;
  std::uint32_t rtp_payload{0};
  std::size_t pending_frames{0}, pending_events{0}, presentation_history{0};
  // An explicitly empty string means no STUN server.
  std::optional<std::string> stun_server;
};

// GPU browser target. Its graph owner starts signaling, advertises capability
// and installs native taps; this component alone does not claim workflow parity.
// Every configurable value is supplied by the caller's resolved configuration.
// There is no software encoder or system-memory fallback.
//
// The graph owner serializes route/peer mutations, closes ingress and drain
// gates around route changes, and stops the parent pipeline before destruction.
// One instance is used for one pipeline lifetime. Borrowed endpoint identity
// remains stable so the audio owner can add its track before peer construction.
class WebPreviewTarget {
 public:
  static std::unique_ptr<WebPreviewTarget> Create(
      const WebPreviewConfig& config,
      SurfacePainter painter,
      std::string* error);
  static bool Validate(const WebPreviewConfig& config, std::string* error);
  ~WebPreviewTarget();
  WebPreviewTarget(const WebPreviewTarget&) = delete;
  WebPreviewTarget& operator=(const WebPreviewTarget&) = delete;

  // Borrowed references. bin() owns all elements; add it to the parent with
  // gst_bin_add(). This object retains its own strong reference.
  GstElement* bin() const {
    return bin_;
  }
  GstElement* endpoint() const {
    return endpoint_;
  }
  // Returned route pad is an owned reference; remove only with gates closed.
  // Selecting a different active channel requires a newer operation number.
  GstPad* AddRoute(const std::string& channel);
  void RemoveRoute(GstPad* route);
  bool Select(GstPad* route, std::uint64_t operation);
  bool Active(GstPad* route, std::uint64_t operation) const;
  bool SetFrameAdmission(FrameAdmission admission);
  // Sole peer lifecycle entry points, called on the signaling owner. Do not
  // detach/reset endpoint() directly. These close compressed output, release
  // old downstream waits, drain accepted pushes, then retire/replace the peer.
  // Replace returns an owned peer reference. It admits video only after the
  // connection is ready and a newly admitted IDR establishes decoder recovery.
  // Managed endpoint notifications defer reopening until accepted pushes drain;
  // destruction joins that bounded readiness worker after the parent is NULL.
  // Detach resumes the disconnected drain, which does not imply presentation.
  GstElement* ReplacePeer(std::uint64_t generation);
  bool DetachPeer(std::uint64_t generation);
  // Construct once, before signaling starts. Stop signaling before destroying
  // this target. Retained callbacks fail safely after target destruction.
  std::shared_ptr<WebPreviewSignaling> Signaling(const WebPreviewObserverConfig& config);
  std::vector<TransportEvent> TakeEvents();
  TransportCounters counters() const;

 private:
  explicit WebPreviewTarget(const WebPreviewConfig& config);
  bool Build(const WebPreviewConfig& config, SurfacePainter painter, std::string* error);
  bool Probe(GstElement* element, const char* pad, GstPadProbeType type, GstPadProbeCallback callback);
  GstElement* bin_{nullptr};
  GstElement* selected_{nullptr};
  GstElement* endpoint_{nullptr};
  GstElement* rtp_gate_{nullptr};
  struct Route {
    GstPad* selected;
    std::string channel;
  };
  std::map<GstPad*, Route> routes_;
  std::vector<std::pair<GstPad*, gulong>> probes_;
  std::shared_ptr<TransportMonitor> monitor_;
  struct PeerLifetime;
  std::shared_ptr<PeerLifetime> peer_lifetime_;
  std::weak_ptr<WebPreviewSignaling> signaling_;
  std::uint64_t next_route_{0};
  GstPad* active_route_{nullptr};
  std::uint64_t operation_{0};
  std::uint64_t peer_generation_{0};
};

} // namespace hm::preview
