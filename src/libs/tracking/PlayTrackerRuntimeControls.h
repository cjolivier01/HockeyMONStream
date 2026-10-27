#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/tracking/CameraControlSpecs.h"
#include "yaml-cpp/yaml.h"

namespace hm::tracking {
using CameraControlValues = std::map<std::string, double>;

// Read the same canonical controls and optional explicit UI overrides as Qt.
absl::StatusOr<CameraControlValues> ReadPlaytrackerControlValues(const YAML::Node& resolved);

bool ValidPlaytrackerControlValue(const CameraSliderSpec& spec, double value);

// Every default comes from the caller's resolved baseline. Unknown, missing
// and out-of-range values fail before producing any native runtime document.
absl::StatusOr<std::vector<CameraSliderSpec>> PlaytrackerControlSpecs(const CameraControlValues& defaults);

// Shared by desktop publication and private web commands. The immutable
// document preserves the selected native config, replaces any stale sparse
// delta, and changes only the explicitly published controls and target roles.
absl::StatusOr<YAML::Node> BuildPlaytrackerRuntimeControls(
    const YAML::Node& base,
    const CameraControlValues& values,
    const CameraControlValues& defaults,
    const std::set<std::string>& changed,
    bool force_all_targets);
} // namespace hm::tracking
