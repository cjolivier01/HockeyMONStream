#pragma once

#include <yaml-cpp/yaml.h>
#include "hstream/src/libs/program/PreviewOverlayRuntime.h"
#include "absl/status/statusor.h"

namespace hm::pipeline_internal {
// Required resolved baseline settings; an explicit launcher selection can
// override these after validation, but there is no second default set.
absl::StatusOr<PreviewOverlaySelection> ReadPreviewOverlaySelection(const YAML::Node& resolved);
} // namespace hm::pipeline_internal
