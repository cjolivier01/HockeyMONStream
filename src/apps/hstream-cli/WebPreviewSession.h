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
      const std::string& retained_selection);
  ~WebPreviewSession();
  absl::Status Attach();
  void Stop();
  std::string Capability() const;
  std::string Command(const RouteOwner::Request& request);
  const RouteOwner::Session& identity() const {
    return routes_.session();
  }
  std::string selection() const;

 private:
  struct PaintState;
  struct RouteState;
  WebPreviewSession(AppCtx& app, WebPreviewOptions options);
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
};

} // namespace hm::preview
