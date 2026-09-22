#pragma once

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::pipeline {

inline absl::StatusOr<std::vector<uint64_t>> Int8SampleTimes(uint64_t duration, size_t count) {
  if (count < 16 || count > 256 || duration < count * 100000000ULL || duration > 24ULL * 3600 * 1000000000)
    return absl::InvalidArgumentError(
        "INT8 sampling requires 16..256 frames and a recording between 0.1s per sample and 24h");
  std::vector<uint64_t> result;
  // Centers of equal timeline bins avoid duplicating the opening frame or
  // requesting an undecodable timestamp at permanent camera EOS.
  for (size_t index = 0; index < count; ++index)
    result.push_back(duration * (2 * index + 1) / (2 * count));
  return result;
}

} // namespace hm::pipeline
