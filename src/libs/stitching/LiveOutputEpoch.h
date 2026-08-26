#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace hm::stitching {

struct LiveOutputEpoch {
  double post_stitch_rotate_degrees{0.0};
  std::string authorization_id;
  std::string scoreboard_property_value;
};

// Length-prefixes epoch strings so they remain unambiguous when transported
// through the runtime property command protocol. Values without scoreboard
// geometry retain the legacy authorization+rotation representation.
std::string serialize_live_output_epoch(const LiveOutputEpoch& epoch);

absl::StatusOr<LiveOutputEpoch> parse_live_output_epoch(std::string_view value);

} // namespace hm::stitching
