#include "hstream/src/libs/common/OutputRootConfig.h"

#include <cstdlib>
#include <map>

#include "absl/status/status.h"
#include "absl/strings/str_split.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"

namespace hm::user_config {

absl::StatusOr<std::filesystem::path> output_root_for_game(
    const std::filesystem::path& game,
    const std::vector<std::string>& option_groups) {
  // Preserve the explicit environment override, including HOME-less jobs.
  if (const char* root = std::getenv("HM_OUTPUT_WORK_DIR"); root && *root)
    return output_root(YAML::Node(YAML::NodeType::Map));
  const auto baseline = baseline_config::load();
  if (!baseline.ok())
    return baseline.status();
  const auto user = load_or_create();
  if (!user.ok())
    return user.status();
  try {
    // Only this leaf is needed before the runner starts. Respect replacement
    // of its parent map without duplicating the CLI's general config merger.
    YAML::Node config(YAML::NodeType::Map);
    const auto overlay = [&](const YAML::Node& layer) {
      if (!layer.IsMap())
        return;
      const auto paths = layer[kPathsKey];
      if (!paths.IsDefined())
        return;
      if (!paths.IsMap()) {
        config[kPathsKey] = YAML::Clone(paths);
      } else {
        if (!config[kPathsKey].IsMap())
          config[kPathsKey] = YAML::Node(YAML::NodeType::Map);
        if (paths[kOutputRootKey].IsDefined())
          config[kPathsKey][kOutputRootKey] = YAML::Clone(paths[kOutputRootKey]);
      }
    };
    overlay(baseline->values);
    overlay(*user);
    const auto game_config = game / "config.yaml";
    if (std::filesystem::exists(game_config))
      overlay(YAML::LoadFile(game_config.string()));
    for (const auto& group : option_groups) {
      // CLI groups use first-wins maps, applied in argument order.
      std::map<std::string, std::string> options;
      for (const auto item : absl::StrSplit(group, ',')) {
        const std::vector<std::string> pair = absl::StrSplit(item, '=');
        if (pair.size() != 2)
          return absl::InvalidArgumentError("Pipeline options should use key/value pairs: " + std::string(item));
        options.emplace(pair[0], pair[1]);
      }
      for (const auto& [key, value] : options) {
        if (key == "paths") {
          config[kPathsKey] = value;
        } else if (key.rfind("paths.", 0) == 0) {
          if (!config[kPathsKey].IsMap())
            config[kPathsKey] = YAML::Node(YAML::NodeType::Map);
          if (key == "paths.output-root")
            config[kPathsKey][kOutputRootKey] = value;
          else if (key.rfind("paths.output-root.", 0) == 0)
            config[kPathsKey][kOutputRootKey] = YAML::Node(YAML::NodeType::Map);
        }
      }
    }
    return output_root(config);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Cannot resolve job output root: " + std::string(error.what()));
  }
}

} // namespace hm::user_config
