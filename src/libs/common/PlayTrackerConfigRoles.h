#pragma once

#include <cstddef>
#include <vector>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm {

struct PlayTrackerLiveBoxRoles {
  size_t fast_index;
  size_t follower_index;
};

// Resolve the native play-tracker roles without imposing a two-box layout.
// Named roles win. Otherwise the first box is fast and the last box is the
// follower. A one-box config validly uses the same box for both roles.
absl::StatusOr<PlayTrackerLiveBoxRoles> resolve_playtracker_live_box_roles(const YAML::Node& live_boxes);

// Return the stable native order: fast first, additional boxes in their
// original order, and follower last. The external tracker assigns those two
// roles positionally, so canonical materialization normalizes custom layouts
// before runtime while preserving every box and all of its fields.
absl::StatusOr<std::vector<size_t>> normalized_playtracker_live_box_order(const YAML::Node& live_boxes);

} // namespace hm
