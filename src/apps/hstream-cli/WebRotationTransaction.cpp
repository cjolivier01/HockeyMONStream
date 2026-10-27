#include "WebRotationTransaction.h"

#include <utility>

namespace hm::preview {

WebRotationTransaction::WebRotationTransaction(
    std::string game_dir,
    program::RotationControlValues controls,
    bool suppressed,
    Hooks hooks)
    : game_dir_(std::move(game_dir)),
      controls_(std::move(controls)),
      ranges_(controls_),
      suppressed_(suppressed),
      hooks_(std::move(hooks)) {}

WebRotationTransaction::~WebRotationTransaction() {
  (void)Shutdown();
}

void WebRotationTransaction::Reply(
    const WebRotationControlsRequest& request,
    const std::string& status,
    const std::string& reason) {
  const bool settled = status == "state" || status == "applied";
  hooks_.reply(WebRotationControlsReply(
      request,
      status,
      reason,
      failed_ ? std::nullopt : std::optional<program::RotationControlValues>(controls_),
      settled ? std::optional<uint64_t>(hooks_.operation()) : std::nullopt,
      suppressed_));
}

bool WebRotationTransaction::Handle(const WebRotationControlsRequest& request) {
  const auto blocked = hooks_.blocked(request.session, !request.query);
  if (failed_ || stopping_) {
    Reply(request, "failed", failed_ ? "retire-required" : "stopping");
    return false;
  }
  if (!blocked.empty()) {
    Reply(request, "failed", blocked);
    return false;
  }
  if (pending_) {
    Reply(request, "failed", "rotation-pending");
    return false;
  }
  if (request.query) {
    Reply(request, "state");
    return true;
  }
  const uint64_t identity = std::stoull(request.request_id);
  if (identity <= last_mutation_) {
    Reply(request, "failed", "stale-request");
    return false;
  }
  last_mutation_ = identity;
  if (suppressed_) {
    for (const auto& [id, value] : request.controls) {
      (void)value;
      if (id != "Stitch_Rotate_Degrees") {
        Reply(request, "failed", "fixed-edge-suppressed");
        return false;
      }
    }
  }
  const auto merged = program::MergeRotationControls(controls_, request.controls, &ranges_);
  if (!merged.ok()) {
    Reply(request, "failed", "invalid-controls");
    return false;
  }
  Pending pending{request, *merged, {}, std::nullopt, std::nullopt};
  for (const auto& [id, value] : *merged)
    if (controls_.at(id) != value)
      pending.changed.insert(id);
  pending_ = std::move(pending);
  if (!pending_->changed.count("Stitch_Rotate_Degrees")) {
    Apply();
    return true;
  }
  Reply(request, "authorizing");
  const double degrees = 90 - merged->at("Stitch_Rotate_Degrees");
  const std::string epoch =
      request.session.run + ":" + std::to_string(request.session.topology) + ":" + request.request_id;
  const bool queued =
      disk_.Authorize(game_dir_, degrees, epoch, [this](stitching::LiveRotationConfigOwner::Result result) {
        if (!pending_ || stopping_)
          return;
        if (!result.status.ok()) {
          Finish("failed", "authorization-failed");
          return;
        }
        if (result.authorization && result.authorization->value)
          pending_->authorization = std::move(result.authorization);
        const auto blocked = hooks_.blocked(pending_->request.session, true);
        if (!blocked.empty()) {
          Rollback(blocked);
          return;
        }
        Apply();
      });
  if (!queued)
    Finish("failed", "stopping");
  return queued;
}

void WebRotationTransaction::Apply() {
  if (!pending_ || stopping_)
    return;
  const auto blocked = hooks_.blocked(pending_->request.session, true);
  if (!blocked.empty()) {
    Rollback(blocked);
    return;
  }
  Reply(pending_->request, "applying");
  const auto applied = hooks_.apply(pending_->controls, pending_->changed, pending_->authorization);
  if (!applied.ok()) {
    // A failed property/route transaction may have reached the producer. Retire
    // the complete graph before rolling back the authorization on disk.
    RequireRetirement();
    return;
  }
  pending_->operation = *applied;
  if (!pending_->authorization) {
    controls_ = pending_->controls;
    Finish("applied");
    return;
  }
  Reply(pending_->request, "committing");
  const bool queued = disk_.Commit(*pending_->authorization, [this](stitching::LiveRotationConfigOwner::Result result) {
    if (!pending_ || stopping_)
      return;
    if (!result.status.ok()) {
      RequireRetirement();
      return;
    }
    controls_ = pending_->controls;
    Finish("applied");
  });
  if (!queued)
    RequireRetirement();
}

void WebRotationTransaction::Rollback(const std::string& reason) {
  if (!pending_)
    return;
  if (!pending_->authorization) {
    Finish("failed", reason);
    return;
  }
  Reply(pending_->request, "rolling-back");
  const bool queued =
      disk_.Rollback(*pending_->authorization, [this, reason](stitching::LiveRotationConfigOwner::Result result) {
        if (!pending_ || stopping_)
          return;
        if (!result.status.ok()) {
          RequireRetirement();
          return;
        }
        Finish("failed", reason);
      });
  if (!queued)
    RequireRetirement();
}

void WebRotationTransaction::Finish(const std::string& status, const std::string& reason) {
  if (!pending_)
    return;
  const auto request = pending_->request;
  pending_.reset();
  Reply(request, status, reason);
}

void WebRotationTransaction::RequireRetirement() {
  failed_ = true;
  Finish("failed", "retire-required");
  hooks_.retire();
}

void WebRotationTransaction::Poll() {
  disk_.Poll();
}

absl::Status WebRotationTransaction::Shutdown() {
  stopping_ = true;
  const auto status = disk_.Shutdown();
  pending_.reset();
  return status;
}

} // namespace hm::preview
