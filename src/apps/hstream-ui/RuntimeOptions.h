#pragma once

#include <QtCore/QString>
#include <cstdint>
#include <functional>
#include <optional>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::ui_internal {
std::optional<unsigned> configured_pipeline_gpu(const YAML::Node& effective_config);
QString configured_gpu_memory_profile(const YAML::Node& baseline, const YAML::Node& user);
using MemoryQuery = std::function<absl::StatusOr<uint64_t>(unsigned)>;
// Resolve the same untouched combo used by Qt, without inventing a profile when
// an automatic device query fails. Explicit selections need no device query.
absl::StatusOr<QString> resolve_gpu_memory_profile(
    const QString& requested,
    std::optional<unsigned> gpu,
    MemoryQuery query = {});
} // namespace hm::ui_internal
