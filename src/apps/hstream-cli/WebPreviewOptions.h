#pragma once

#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"
#include "hstream/src/libs/program/PreviewOverlayRuntime.h"

#include <yaml-cpp/yaml.h>
#include "absl/status/statusor.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace hm::preview {

struct PreviewReferenceGeometry {
  unsigned program_width, program_height, source_width, source_height;
};
// Qt render surfaces and browser overlay coordinates use the same resolved
// geometry. A browser transport resolution is a separate output constraint.
absl::StatusOr<PreviewReferenceGeometry> ReadPreviewReferenceGeometry(const YAML::Node& resolved);

// All settings come from the resolved canonical baseline/overlay. This type
// deliberately has no usable defaults and is shared with the CPU admission test.
struct WebPreviewOptions {
  unsigned width, height, rate_numerator, rate_denominator;
  unsigned surface_pool_frames, conversion_pool_frames, bitrate, audio_bitrate, idr_interval;
  unsigned h264_profile, encoder_tuning, rtp_payload;
  unsigned pending_frames, pending_events, presentation_history;
  unsigned poll_interval_ms, events_per_batch, queue_frames;
  unsigned browser_max_frames, browser_max_age_ms;
  unsigned authored_connect_timeout_ms, authored_disconnect_timeout_ms;
  unsigned program_reference_width, program_reference_height;
  unsigned source_reference_width, source_reference_height;
  std::string initial_channel;
  std::string stun_server;
};

absl::StatusOr<WebPreviewOptions> ReadWebPreviewOptions(const YAML::Node& resolved);
bool ValidWebPreviewRun(const std::string& run);
absl::StatusOr<RouteOwner::Request> ParseWebPreviewRoute(const std::string& json);
std::string WebPreviewRouteReply(const RouteOwner::Request& request, const RouteOwner::Result& result);
struct WebPreviewSeekRequest {
  RouteOwner::Session session;
  uint64_t request_id, position_ns;
  std::optional<int64_t> delta_ns;
};
absl::StatusOr<WebPreviewSeekRequest> ParseWebPreviewSeek(const std::string& json);
std::string WebPreviewSeekReply(
    const WebPreviewSeekRequest& request,
    const std::string& status,
    const std::string& reason,
    uint64_t resulting_topology,
    std::optional<uint64_t> position_ns = std::nullopt,
    std::optional<uint64_t> source_pts_ns = std::nullopt);
struct WebPreviewOverlayRequest {
  RouteOwner::Session session;
  std::string request_id;
  pipeline_internal::PreviewOverlayCommand command;
  bool query{false};
};
absl::StatusOr<WebPreviewOverlayRequest> ParseWebPreviewOverlays(const std::string& json, bool query = false);
// Validates session and generation before invoking the same native transaction
// used by Qt. Failed/stale requests never mutate or advance committed state.
std::string ApplyWebPreviewOverlays(
    const WebPreviewOverlayRequest& request,
    const RouteOwner::Session& session,
    pipeline_internal::PreviewOverlayRuntimeState& state,
    const std::string& mutation_blocked_reason,
    const std::function<bool(const pipeline_internal::PreviewOverlaySelection&)>& apply);
struct WebPreviewOverlayArtifact {
  RouteOwner::Session session;
  uint64_t operation, generation;
  std::string channel, status, reason;
};
std::string WebPreviewOverlayArtifactEvent(const WebPreviewOverlayArtifact& artifact);
// Bounded producer-to-main-context status. Readback can replay the last status
// after an acknowledgment was lost, without waiting for another painted frame.
class WebPreviewOverlayArtifactMailbox {
 public:
  void Publish(WebPreviewOverlayArtifact artifact);
  void Replay(const RouteOwner::Session& session);
  std::optional<WebPreviewOverlayArtifact> Take(
      const RouteOwner::Session& session,
      uint64_t operation,
      const std::string& channel,
      const pipeline_internal::PreviewOverlayCommand& overlays);

 private:
  std::mutex mutex_;
  std::optional<WebPreviewOverlayArtifact> last_, pending_;
};

std::string WebPreviewCapability(
    const WebPreviewOptions& options,
    const RouteOwner& owner,
    const pipeline_internal::PreviewOverlayRuntimeState& overlays,
    bool seek = false);

} // namespace hm::preview
