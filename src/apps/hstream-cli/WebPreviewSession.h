#pragma once

#include "WebPreviewOptions.h"
#include "deepstream_app.h"
#include "hstream/src/apps/apps-common/PreviewRouteGraph.h"
#include "hstream/src/apps/apps-common/WebPreviewTarget.h"

#include <memory>

namespace hm::preview {

// One native graph generation. Prepare supplies the single WEBRTC sink's
// stable endpoint before audio construction; Attach installs native route taps
// after graph construction, then starts signaling. The owner calls Stop before
// graph teardown and destroys this object only after the pipeline is NULL.
class WebPreviewSession {
 public:
  static absl::StatusOr<std::unique_ptr<WebPreviewSession>> Prepare(
      AppCtx& app,
      const YAML::Node& resolved,
      RouteOwner::Session identity,
      const std::string& retained_selection,
      const std::string& game_dir);
  ~WebPreviewSession();
  absl::Status Attach(const std::function<bool()>& reconcile_overlays);
  void SetOverlays(pipeline_internal::PreviewOverlaySelection selection, uint64_t generation);
  // Called after publishing a matching query acknowledgment, including paused readback.
  void RefreshOverlayStatus(const RouteOwner::Session& session);
  void Stop();
  std::string Capability(const pipeline_internal::PreviewOverlayRuntimeState& overlays) const;
  std::string Command(const RouteOwner::Request& request);
  std::string MutationBlockReason() const;
  const RouteOwner::Session& identity() const {
    return routes_.session();
  }
  std::string selection() const;
  bool available() const {
    return !stopped_ && !channels_.empty();
  }

 private:
  struct PaintState;
  struct RouteState;
  WebPreviewSession(AppCtx& app, WebPreviewOptions options);
  void DrainOverlayStatus();
  guint overlay_status_source_{0};
  absl::Status AddRoute(const std::string& channel, RouteTap tap);
  AppCtx& app_;
  WebPreviewOptions options_;
  NvDsSinkEncoderConfig* sink_config_{nullptr};
  std::shared_ptr<PaintState> paint_;
  std::unique_ptr<WebPreviewTarget> target_;
  std::map<std::string, std::shared_ptr<RouteState>> channels_;
  std::shared_ptr<WebPreviewSignaling> signaling_;
  RouteOwner routes_;
  bool stopped_{false};
  bool overlays_ready_{false};
  std::function<bool()> reconcile_overlays_;
};

} // namespace hm::preview
