#pragma once

#include <map>
#include <string>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::tracking {
using ConfigLeafRanks = std::map<std::string, int>;

// Materialize the native camera document from canonical settings. Rank zero is
// the shared baseline; higher ranks are user, game and launch overrides. Native
// values win a same-layer tie. Missing baseline keys and malformed values fail.
// Input documents remain unchanged, including aliases of their nested nodes.
absl::StatusOr<YAML::Node> ResolveProgramCameraConfig(
    const YAML::Node& effective_config,
    const ConfigLeafRanks& canonical_value_ranks,
    int native_base_rank,
    const YAML::Node& base_playtracker_config);
} // namespace hm::tracking
