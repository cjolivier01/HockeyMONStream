#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace hm::stitching {

struct LiveOutputEpoch {
  double post_stitch_rotate_degrees{0.0};
  std::string authorization_id;
};

// Length-prefixes the authorization ID so the value remains unambiguous when
// transported through the runtime property command protocol.
std::string serialize_live_output_epoch(const LiveOutputEpoch& epoch);

absl::StatusOr<LiveOutputEpoch> parse_live_output_epoch(std::string_view value);

} // namespace hm::stitching
