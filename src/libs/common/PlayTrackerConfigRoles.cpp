#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"

#include <optional>
#include <string>

#include "absl/status/status.h"

namespace hm {

absl::StatusOr<PlayTrackerLiveBoxRoles> resolve_playtracker_live_box_roles(const YAML::Node& live_boxes) {
  if (!live_boxes.IsSequence() || live_boxes.size() == 0)
    return absl::InvalidArgumentError("play-tracker.live-boxes must be a non-empty sequence");

  std::optional<size_t> named_fast;
  std::optional<size_t> named_follower;
  for (size_t index = 0; index < live_boxes.size(); ++index) {
    const YAML::Node box = live_boxes[index];
    if (!box.IsMap())
      return absl::InvalidArgumentError("play-tracker.live-boxes entries must be maps");
    const YAML::Node name = box["name"];
    if (!name.IsDefined() || name.IsNull())
      continue;
    if (!name.IsScalar())
      return absl::InvalidArgumentError("play-tracker.live-boxes names must be scalars");
    const std::string parsed = name.as<std::string>();
    if (parsed == "current_roi") {
      if (named_fast.has_value())
        return absl::InvalidArgumentError("play-tracker.live-boxes contains duplicate current_roi boxes");
      named_fast = index;
    } else if (parsed == "current_roi_aspect") {
      if (named_follower.has_value())
        return absl::InvalidArgumentError("play-tracker.live-boxes contains duplicate current_roi_aspect boxes");
      named_follower = index;
    }
  }

  size_t fast = named_fast.value_or(0);
  size_t follower = named_follower.value_or(live_boxes.size() - 1);
  if (live_boxes.size() > 1 && fast == follower) {
    if (named_fast.has_value() && !named_follower.has_value()) {
      for (size_t index = live_boxes.size(); index-- > 0;) {
        if (index != fast) {
          follower = index;
          break;
        }
      }
    } else if (named_follower.has_value() && !named_fast.has_value()) {
      for (size_t index = 0; index < live_boxes.size(); ++index) {
        if (index != follower) {
          fast = index;
          break;
        }
      }
    }
  }
  return PlayTrackerLiveBoxRoles{fast, follower};
}

absl::StatusOr<std::vector<size_t>> normalized_playtracker_live_box_order(const YAML::Node& live_boxes) {
  auto roles = resolve_playtracker_live_box_roles(live_boxes);
  if (!roles.ok())
    return roles.status();
  std::vector<size_t> order;
  order.reserve(live_boxes.size());
  order.push_back(roles->fast_index);
  for (size_t index = 0; index < live_boxes.size(); ++index) {
    if (index != roles->fast_index && index != roles->follower_index)
      order.push_back(index);
  }
  if (roles->follower_index != roles->fast_index)
    order.push_back(roles->follower_index);
  return order;
}

} // namespace hm
