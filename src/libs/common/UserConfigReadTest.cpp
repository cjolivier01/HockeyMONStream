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
  fs::remove_all(root);
  std::cout << (ok ? "PASS" : "FAIL")
            << ": read-only user config preserves overlay precedence without first-run writes\n";
  return ok ? 0 : 1;
}
