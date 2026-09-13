#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::stitching {
enum class SynchronizationMethod { kAudio, kImu, kAuto };
absl::StatusOr<SynchronizationMethod> parse_synchronization_method(const std::string& name);

namespace imu {
struct Sample {
  double seconds;
  double x, y, z;
};
struct Recording {
  std::vector<Sample> gyro;
  double fps{0};
};
// Setup-only metadata reads, bounded to a prefix of the first physical chapter.
absl::StatusOr<Recording> Read(const std::string& video);
// Positive seconds means skip the left recording; negative means skip the right.
absl::StatusOr<double> EstimateOffset(const std::vector<Sample>& left, const std::vector<Sample>& right);
// GPMF packet timing is relative to the video track. Exposed for parser tests.
absl::StatusOr<std::vector<Sample>> ParseGpmf(const unsigned char* data, size_t size, double seconds, double duration);
} // namespace imu

absl::StatusOr<std::pair<double, double>> synchronize_by_imu(const std::string& left, const std::string& right);
} // namespace hm::stitching
