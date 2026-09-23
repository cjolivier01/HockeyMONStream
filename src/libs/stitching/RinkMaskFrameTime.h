#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/statusor.h"

namespace hm::stitching {

// nullopt denotes the existing automatic selection. Negative offsets are
// relative to the common synchronized recording end, never the playback limit.
absl::StatusOr<std::optional<int64_t>> ParseRinkMaskFrameTime(const std::string& value);
std::string FormatRinkMaskFrameTime(std::optional<int64_t> offset_ns);
absl::StatusOr<uint64_t> ResolveRinkMaskFrameTime(int64_t offset_ns, uint64_t recording_duration_ns);

} // namespace hm::stitching
