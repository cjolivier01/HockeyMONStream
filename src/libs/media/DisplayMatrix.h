#pragma once

#include <cstdint>

#include "absl/status/statusor.h"
#include "hstream/src/libs/media/DisplayTransform.h"

struct AVStream;

namespace hm::media {
absl::StatusOr<DisplayTransform> ParseDisplayMatrix(const int32_t matrix[9]);
absl::StatusOr<DisplayTransform> StreamDisplayTransform(const AVStream& stream);
} // namespace hm::media
