#include "PreviewRouteOwner.h"

#include <algorithm>
#include <utility>

namespace hm::preview {
namespace {
bool valid_channel(const std::string& channel) {
  return channel == "program" || channel == "stitched" || channel == "none" ||
      (channel.size() > 6 && channel.compare(0, 6, "source") == 0 &&
       std::all_of(channel.begin() + 6, channel.end(), [](char c) { return c >= '0' && c <= '9'; }));
}
} // namespace

bool RouteOwner::Session::operator==(const Session& other) const {
  return run == other.run && stage == other.stage && topology == other.topology;
}

bool RouteOwner::initialize_selection(std::string channel, std::uint64_t operation) {
  if (!routes_.empty())
    return false;
  explicitly_disabled_ = channel == "none";
  selected_ = explicitly_disabled_ ? std::string() : std::move(channel);
  operation_ = operation;
  return true;
}

bool RouteOwner::register_route(std::string channel, Route route) {
  if (channel.empty() || channel == "none" || !route.ingress || !route.drain || !route.active || !route.prepare ||
      !route.rearm || !route.quiesce)
    return false;
  return routes_.emplace(std::move(channel), std::move(route)).second;
}

void RouteOwner::forget_route(const std::string& channel) {
  routes_.erase(channel);
}

void RouteOwner::forget_routes() {
  routes_.clear();
}

void RouteOwner::reconcile_selection() {
  if (!selected_.empty() && !routes_.count(selected_)) {
    selected_.clear();
    explicitly_disabled_ = false;
  }
}

void RouteOwner::bind_session(Session session) {
  session_ = std::move(session);
}

std::vector<std::string> RouteOwner::channels() const {
  std::vector<std::string> result;
  result.reserve(routes_.size());
  for (const auto& entry : routes_)
    result.push_back(entry.first);
  return result;
}

RouteOwner::Result RouteOwner::select(const Request& request, const SelectionChanged& changed) {
  if (request.version != 1 || request.request_id.empty() || request.operation == 0 || !valid_channel(request.channel))
    return {Status::kInvalid, "invalid-request"};
  if (session_.run.empty() || session_.topology == 0 || !(request.session == session_))
    return {Status::kWrongSession, "stale-session"};
  return select_current(request.channel, request.operation, changed);
}

RouteOwner::Result RouteOwner::select_current(
    const std::string& channel,
    std::uint64_t operation,
    const SelectionChanged& changed) {
  if (routes_.empty())
    return {Status::kUnconfigured, "preview routes are not configured"};
  if (operation <= operation_)
    return {Status::kStale, "activation generation is not newer than " + std::to_string(operation_)};
  auto notify = [&] {
    if (changed)
      changed();
  };
  const auto previous = routes_.find(selected_);
  if (previous != routes_.end() && previous->first == channel) {
    const bool ready = previous->second.rearm(operation);
    operation_ = operation;
    if (!ready) {
      selected_.clear();
      explicitly_disabled_ = false;
      notify();
      return {Status::kFailed, "the selected GPU preview channel cannot be re-armed"};
    }
    return {Status::kActivated, "GPU preview branch re-armed"};
  }
  if (previous != routes_.end()) {
    previous->second.ingress(false, operation);
    previous->second.drain(false, operation);
    if (!previous->second.quiesce(operation)) {
      selected_.clear();
      explicitly_disabled_ = false;
      operation_ = operation;
      notify();
      return {Status::kFailed, "could not quiesce the previous preview channel"};
    }
  }
  selected_.clear();
  explicitly_disabled_ = false;
  operation_ = operation;
  if (channel == "none") {
    explicitly_disabled_ = true;
    notify();
    return {Status::kDeactivated, "all GPU preview branches are inactive"};
  }
  const auto target = routes_.find(channel);
  if (target == routes_.end()) {
    notify();
    return {Status::kUnavailable, "the requested camera source is unavailable"};
  }
  selected_ = channel;
  notify();
  target->second.prepare(operation);
  target->second.drain(true, operation);
  target->second.ingress(true, operation);
  if (!target->second.active()) {
    target->second.ingress(false, operation);
    target->second.drain(false, operation);
    target->second.quiesce(operation);
    selected_.clear();
    notify();
    return {Status::kFailed, "the requested GPU preview channel has failed locally"};
  }
  return {Status::kActivated, "GPU preview branch activated"};
}
} // namespace hm::preview
