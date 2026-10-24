#include "hstream/src/libs/common/ConfigLayers.h"

#include <algorithm>
#include <iostream>

#include "absl/strings/str_split.h"
#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::config {
YAML::Node MergeNodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest) {
  if (!overlay.IsMap()) {
    return base;
  }
  if (!base.IsMap()) {
    return overlay;
  }
  YAML::Node result = base;
  for (const auto& pair : overlay) {
    const std::string& key = pair.first.as<std::string>();

    // Check if key exists in base
    if (warn_if_key_not_in_dest && !base[key]) {
      std::cerr << "Warning: Key '" << key << "' in overlay does not exist in base config\n";
    }

    // If both are maps, recursively merge
    if (pair.second.IsMap() && base[key].IsDefined() && base[key].IsMap()) {
      result[key] = MergeNodes(base[key], pair.second, warn_if_key_not_in_dest);
    } else {
      result[key] = pair.second;
    }
  }

  return result;
}

void RecordLeafRanks(const YAML::Node& overlay, const std::string& prefix, int rank, LeafRanks& ranks) {
  if (!overlay.IsMap()) {
    if (!prefix.empty())
      ranks[prefix] = rank;
    return;
  }
  if (overlay.size() == 0 && !prefix.empty()) {
    ranks[prefix] = rank;
    return;
  }
  for (const auto& entry : overlay) {
    const std::string key = entry.first.as<std::string>();
    const std::string path = prefix.empty() ? key : prefix + "." + key;
    RecordLeafRanks(entry.second, path, rank, ranks);
  }
}

absl::StatusOr<std::filesystem::path> ResolveReference(
    const std::filesystem::path& configured,
    int rank,
    const ReferenceRoots& roots,
    const std::string& key) {
  namespace fs = std::filesystem;
  std::vector<fs::path> candidates;
  if (configured.is_absolute()) {
    candidates.push_back(configured);
  } else {
    const auto append = [&](const fs::path& root) {
      if (root.empty())
        return;
      const auto candidate = root / configured;
      if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        candidates.push_back(candidate);
    };
    if (rank == 3)
      append(roots.working);
    else if (rank == 2)
      append(roots.game);
    else if (rank == 1)
      append(roots.user);
    else {
      append(roots.application);
      append(roots.baseline);
    }
    append(roots.application);
    append(roots.baseline);
    append(roots.game);
    append(roots.working);
    append(roots.working / "configs");
  }
  for (const auto& candidate : candidates) {
    std::error_code error;
    if (fs::is_regular_file(candidate, error) && !error)
      return candidate;
  }
  std::string searched;
  for (const auto& candidate : candidates) {
    if (!searched.empty())
      searched += ", ";
    searched += candidate.string();
  }
  return absl::NotFoundError("Could not locate " + key + "; searched: " + searched);
}

absl::StatusOr<ProgramConfiguration> ResolveProgramConfiguration(
    const YAML::Node& baseline,
    const YAML::Node& user,
    const YAML::Node& game,
    const YAML::Node& native_app,
    const std::vector<std::string>& option_groups) {
  if (!baseline.IsMap() || !user.IsMap() || !game.IsMap() || !native_app.IsMap())
    return absl::InvalidArgumentError("Program configuration layers must be YAML maps");
  try {
    ProgramConfiguration result{YAML::Clone(baseline), {}};
    result.values = MergeNodes(result.values, YAML::Clone(user), false);
    RecordLeafRanks(user, {}, 1, result.ranks);
    result.values = MergeNodes(result.values, YAML::Clone(game), false);
    RecordLeafRanks(game, {}, 2, result.ranks);
    result.values["pipeline"] = MergeNodes(YAML::Clone(native_app), result.values["pipeline"], false);
    for (const auto& group : option_groups) {
      std::map<std::string, std::string> options;
      for (const auto item : absl::StrSplit(group, ',')) {
        const std::vector<std::string> pair = absl::StrSplit(item, '=');
        if (pair.size() != 2 || pair[0].empty())
          return absl::InvalidArgumentError("Pipeline options require key=value: " + std::string(item));
        options.emplace(pair[0], pair[1]);
      }
      for (const auto& [path, value] : options) {
        result.values = hm::set_node_value(result.values, path, value);
        result.ranks[path] = 3;
      }
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Cannot resolve Program configuration: ") + error.what());
  }
}
} // namespace hm::config
