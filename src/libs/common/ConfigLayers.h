#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::config {
using LeafRanks = std::map<std::string, int>;

// Native configuration merge semantics: maps recurse; other overlay values
// replace leaves. Like YAML::Node assignment, returned nodes may alias inputs.
// Clone documents at an ownership boundary before calling this primitive.
YAML::Node MergeNodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest);
void RecordLeafRanks(const YAML::Node& overlay, const std::string& prefix, int rank, LeafRanks& ranks);

struct ReferenceRoots {
  std::filesystem::path application, baseline, game, user, working;
};
// Relative files resolve first in the directory belonging to their explicit
// layer (launch=working, game, user, or application for structural defaults),
// followed by the native compatibility search order. Absolute paths stand alone.
absl::StatusOr<std::filesystem::path> ResolveReference(
    const std::filesystem::path& configured,
    int rank,
    const ReferenceRoots& roots,
    const std::string& key);

struct ProgramConfiguration {
  YAML::Node values;
  LeafRanks ranks;
  // Independent snapshots in precedence order for mappings with aliases.
  // Native structural defaults share the first layer with the baseline;
  // all option groups form one launch layer, matching native alias precedence.
  std::vector<YAML::Node> layers;
};

// Immutable input snapshots. Baseline -> user -> game, then native application
// structural underlay in pipeline, then CLI --options groups. Every group uses
// first-wins duplicate keys, while later groups override earlier groups, as in
// the NVIDIA CLI. Native file resolution and canonical camera mapping follow
// this merge and use the returned leaf ranks.
absl::StatusOr<ProgramConfiguration> ResolveProgramConfiguration(
    const YAML::Node& baseline,
    const YAML::Node& user,
    const YAML::Node& game,
    const YAML::Node& native_app,
    const std::vector<std::string>& option_groups);
} // namespace hm::config
