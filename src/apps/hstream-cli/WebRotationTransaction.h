#pragma once

#include "WebPreviewOptions.h"
#include "hstream/src/libs/stitching/LiveRotationConfigOwner.h"

#include <functional>
#include <optional>
#include <set>

namespace hm::preview {

// The main-loop adapter supplies graph operations. This owner shares Qt's
// serial disk worker and retains a mutation through durable authorization,
// runtime application, finalization and exact route rearming.
class WebRotationTransaction {
 public:
  using Authorization = stitching::LiveRotationConfigOwner::Authorization;
  struct Hooks {
    std::function<std::string(const RouteOwner::Session&, bool mutation)> blocked;
    std::function<absl::StatusOr<uint64_t>(
        const program::RotationControlValues&,
        const std::set<std::string>&,
        const std::optional<Authorization>&)>
        apply;
    std::function<uint64_t()> operation;
    std::function<void(const std::string&)> reply;
    std::function<void()> retire;
  };

  WebRotationTransaction(std::string game_dir, program::RotationControlValues controls, bool suppressed, Hooks hooks);
  ~WebRotationTransaction();
  bool Handle(const WebRotationControlsRequest& request);
  void Poll();
  // The graph must be stopped before this final disk barrier.
  absl::Status Shutdown();
  bool pending() const {
    return pending_.has_value() || failed_;
  }
  const program::RotationControlValues& controls() const {
    return controls_;
  }
  const program::RotationControlValues& ranges() const {
    return ranges_;
  }
  bool suppressed() const {
    return suppressed_;
  }

 private:
  struct Pending {
    WebRotationControlsRequest request;
    program::RotationControlValues controls;
    std::set<std::string> changed;
    std::optional<Authorization> authorization;
    std::optional<uint64_t> operation;
  };
  void Reply(const WebRotationControlsRequest& request, const std::string& status, const std::string& reason = "");
  void Apply();
  void Rollback(const std::string& reason);
  void Finish(const std::string& status, const std::string& reason = "");
  void RequireRetirement();

  std::string game_dir_;
  program::RotationControlValues controls_, ranges_;
  bool suppressed_, failed_{false}, stopping_{false};
  uint64_t last_mutation_{0};
  Hooks hooks_;
  std::optional<Pending> pending_;
  stitching::LiveRotationConfigOwner disk_;
};

} // namespace hm::preview
