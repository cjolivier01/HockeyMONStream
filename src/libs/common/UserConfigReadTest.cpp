#include "hstream/src/libs/common/UserConfig.h"

#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hstream-user-read-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  ::setenv("HOME", root.c_str(), 1);
  ::unsetenv("XDG_CONFIG_HOME");
  const fs::path legacy = root / ".hstream/hstream.yaml";
  const fs::path defaults = root / ".config/hstream-ui/defaults.yaml";
  const auto empty = hm::user_config::load_snapshot();
  bool ok = empty.ok() && empty->values.IsMap() && !fs::exists(legacy) && !fs::exists(defaults);
  fs::create_directories(legacy.parent_path());
  std::ofstream(legacy) << "runtime: {gpu_memory_profile: low}\npaths: {game_root: /legacy-games}\n";
  const auto first = hm::user_config::load_snapshot();
  ok &= first.ok() && first->values["runtime"]["gpu_memory_profile"].as<std::string>() == "low";
  fs::create_directories(defaults.parent_path());
  std::ofstream(defaults) << "runtime: {gpu_memory_profile: standard}\n";
  const auto active = hm::user_config::load_snapshot();
  ok &= active.ok() && active->values["runtime"]["gpu_memory_profile"].as<std::string>() == "standard" &&
      active->values["paths"]["game_root"].as<std::string>() == "/legacy-games";
  std::ofstream(defaults) << "{}\n";
  const auto cleared = hm::user_config::load_snapshot();
  ok &= cleared.ok() && !cleared->values["runtime"];
  std::ofstream(defaults) << "runtime: [\n";
  ok &= !hm::user_config::load_snapshot().ok();
  const fs::path frozen = root / "frozen.yaml";
  YAML::Node envelope;
  envelope["schema"] = 1;
  envelope["source_path"] = active->path.string();
  envelope["values"] = active->values;
  std::ofstream(frozen) << envelope;
  ::setenv("HSTREAM_USER_CONFIG_SNAPSHOT", frozen.c_str(), 1);
  const auto frozen_read = hm::user_config::load_snapshot();
  const auto frozen_create = hm::user_config::load_snapshot_or_create();
  const auto frozen_defaults = hm::user_config::load_defaults();
  const auto frozen_preferences = hm::user_config::load_preferences_or_create();
  const auto frozen_path = hm::user_config::active_file_path();
  ok &= frozen_read.ok() && frozen_create.ok() && frozen_defaults.ok() && frozen_preferences.ok() && frozen_path.ok() &&
      *frozen_path == defaults && frozen_read->path == defaults &&
      YAML::Dump(frozen_read->values) == YAML::Dump(active->values) &&
      YAML::Dump(frozen_create->values) == YAML::Dump(active->values) &&
      YAML::Dump(*frozen_defaults) == YAML::Dump(active->values) &&
      YAML::Dump(*frozen_preferences) == YAML::Dump(active->values);
  fs::remove(legacy);
  ok &= hm::user_config::load_or_create().ok() && !fs::exists(legacy);
  envelope["source_path"] = "relative.yaml";
  std::ofstream(frozen) << envelope;
  ok &= !hm::user_config::load_snapshot().ok();
  std::ofstream(frozen) << "schema: 1\nsource_path: ''\nvalues: &cycle {loop: *cycle}\n";
  ok &= !hm::user_config::load_snapshot().ok();
  fs::remove(frozen);
  fs::create_symlink(defaults, frozen);
  ok &= !hm::user_config::load_snapshot().ok();
  fs::remove(frozen);
  ok &= !hm::user_config::load_snapshot_or_create().ok() && !fs::exists(legacy);
  ::unsetenv("HSTREAM_USER_CONFIG_SNAPSHOT");
  fs::remove_all(root);
  std::cout << (ok ? "PASS" : "FAIL")
            << ": read-only user config preserves overlay precedence without first-run writes\n";
  return ok ? 0 : 1;
}
