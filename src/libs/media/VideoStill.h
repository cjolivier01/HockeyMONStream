#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "absl/status/statusor.h"

namespace hm::media {
struct VideoStill {
  int source_width, source_height, width, height;
  int64_t time_ns;
  std::vector<uint8_t> rgba;
};
// One explicitly requested UI still, never a playback path. Software decoding
// keeps this independent of the Program GPU; only the bounded proxy is returned.
// The caller runs this off the UI/HTTP thread and owns cancellation/publication.
absl::StatusOr<VideoStill> ReadVideoStill(
    const std::string& source,
    bool live,
    int64_t time_ns,
    int maximum_dimension,
    int64_t maximum_source_pixels,
    int64_t timeout_ns,
    const std::function<bool()>& cancelled);
} // namespace hm::media
