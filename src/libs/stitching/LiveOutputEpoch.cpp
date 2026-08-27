#include "hstream/src/libs/stitching/LiveOutputEpoch.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

#include "absl/status/status.h"

namespace hm::stitching {

std::string serialize_live_output_epoch(const LiveOutputEpoch& epoch) {
  std::ostringstream rotation;
  rotation.imbue(std::locale::classic());
  rotation << std::setprecision(std::numeric_limits<double>::max_digits10) << epoch.post_stitch_rotate_degrees;
  std::string serialized = std::to_string(epoch.authorization_id.size()) + ":" + epoch.authorization_id;
  if (!epoch.scoreboard_property_value.empty()) {
    serialized += std::to_string(epoch.scoreboard_property_value.size()) + ":" + epoch.scoreboard_property_value;
  }
  return serialized + rotation.str();
}

absl::StatusOr<LiveOutputEpoch> parse_live_output_epoch(std::string_view value) {
  const size_t separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0) {
    return absl::InvalidArgumentError("Invalid stitched-output epoch authorization length");
  }
  size_t authorization_size = 0;
  const auto length = std::from_chars(value.data(), value.data() + separator, authorization_size);
  if (length.ec != std::errc() || length.ptr != value.data() + separator ||
      authorization_size > value.size() - separator - 1) {
    return absl::InvalidArgumentError("Invalid stitched-output epoch authorization length");
  }
  const size_t authorization_start = separator + 1;
  size_t rotation_start = authorization_start + authorization_size;
  if (rotation_start == value.size())
    return absl::InvalidArgumentError("Stitched-output epoch rotation is missing");

  LiveOutputEpoch epoch;
  epoch.authorization_id = std::string(value.substr(authorization_start, authorization_size));
  const size_t scoreboard_separator = value.find(':', rotation_start);
  size_t scoreboard_size = 0;
  if (scoreboard_separator != std::string_view::npos) {
    const auto scoreboard_length =
        std::from_chars(value.data() + rotation_start, value.data() + scoreboard_separator, scoreboard_size);
    if (scoreboard_length.ec != std::errc() || scoreboard_length.ptr != value.data() + scoreboard_separator ||
        scoreboard_size > value.size() - scoreboard_separator - 1) {
      return absl::InvalidArgumentError("Invalid stitched-output epoch scoreboard length");
    }
    const size_t scoreboard_start = scoreboard_separator + 1;
    epoch.scoreboard_property_value = std::string(value.substr(scoreboard_start, scoreboard_size));
    rotation_start = scoreboard_start + scoreboard_size;
    if (rotation_start == value.size())
      return absl::InvalidArgumentError("Stitched-output epoch rotation is missing");
  }
  std::istringstream rotation(std::string(value.substr(rotation_start)));
  rotation.imbue(std::locale::classic());
  rotation >> epoch.post_stitch_rotate_degrees;
  if (!rotation || !std::isfinite(epoch.post_stitch_rotate_degrees))
    return absl::InvalidArgumentError("Invalid stitched-output epoch rotation");
  rotation >> std::ws;
  if (!rotation.eof())
    return absl::InvalidArgumentError("Invalid stitched-output epoch rotation");
  if (epoch.post_stitch_rotate_degrees == 0.0)
    epoch.post_stitch_rotate_degrees = 0.0;
  return epoch;
}

} // namespace hm::stitching
