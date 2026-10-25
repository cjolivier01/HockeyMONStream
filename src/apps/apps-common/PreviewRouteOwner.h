#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hm::preview {

// The pipeline owner serializes all calls, including topology replacement.
// Routes borrow their native gates/consumer; detach them before those objects
// are retired. No Qt types, window IDs, video memory or peer state live here.
class RouteOwner {
 public:
  struct Route {
    std::function<void(bool, std::uint64_t)> ingress;
    std::function<void(bool, std::uint64_t)> drain;
    std::function<bool()> active;
    std::function<void(std::uint64_t)> prepare;
    // Rearming must not drain a route that is waiting on first-frame negotiation.
    std::function<bool(std::uint64_t)> rearm;
    std::function<bool(std::uint64_t)> quiesce;
  };

  struct Session {
    std::string run;
    int stage{0};
    std::uint64_t topology{0};
    bool operator==(const Session& other) const;
  };

  struct Request {
    unsigned version{1};
    Session session;
    std::string request_id;
    std::uint64_t operation{0};
    std::string channel;
  };

  enum class Status { kActivated, kDeactivated, kUnavailable, kStale, kFailed, kUnconfigured, kWrongSession, kInvalid };
  struct Result {
    Status status;
    std::string reason;
  };

  // Startup/reconstruction supplies the saved selection; graph construction
  // creates its matching initial gates. Operation generations survive graph
  // replacement and are independent of topology and WebRTC peer generations.
  bool initialize_selection(std::string channel, std::uint64_t operation);
  bool register_route(std::string channel, Route route);
  void forget_route(const std::string& channel);
  void forget_routes();
  void reconcile_selection();
  void bind_session(Session session);

  // Notification precedes opening a new route, so immutable overlay producers
  // are armed before its first admitted frame. Empty means no active route.
  using SelectionChanged = std::function<void()>;
  Result select(const Request& request, const SelectionChanged& changed);
  // Existing desktop protocol is bound to its current pipeline by its owner.
  Result select_current(const std::string& channel, std::uint64_t operation, const SelectionChanged& changed);

  const std::string& selected_channel() const {
    return selected_;
  }
  bool explicitly_disabled() const {
    return explicitly_disabled_;
  }
  std::uint64_t operation_generation() const {
    return operation_;
  }
  const Session& session() const {
    return session_;
  }
  std::vector<std::string> channels() const;

 private:
  std::map<std::string, Route> routes_;
  std::string selected_;
  bool explicitly_disabled_{false};
  std::uint64_t operation_{0};
  Session session_;
};

} // namespace hm::preview
