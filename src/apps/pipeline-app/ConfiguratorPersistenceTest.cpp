#include "src/apps/pipeline-app/configurator.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <gst/gst.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("configurator-persistence-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const fs::path games = root / "games";
  const fs::path game_dir = games / "first-save";
  fs::create_directories(game_dir);
  ::setenv("HM_GAME_DIR", games.c_str(), 1);

  hm::Configurator configurator("first-save", "", hm::Configurator::kUseConfigFileGpu);
  const auto loaded = configurator.load_config();
  ok &= expect(loaded.ok(), "Configurator must load when the private config is initially absent");

  {
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game_dir);
    ok &= expect(lock.ok(), "concurrent config creator must acquire the transaction lock");
    if (lock.ok()) {
      YAML::Node concurrent(YAML::NodeType::Map);
      concurrent["hstream_ui"]["keep"] = true;
      ok &= expect(
          hm::stitching::publish_game_config(game_dir, YAML::Dump(concurrent) + "\n").ok(),
          "concurrent config creation must publish");
    }
  }

  YAML::Node desired(YAML::NodeType::Map);
  desired["pipeline"]["generated"] = true;
  ok &= expect(
      configurator.save_private_config(desired).ok(), "Configurator first save after concurrent creation must publish");

  auto final_config = hm::stitching::load_game_config_file(game_dir / "config.yaml");
  ok &= expect(
      final_config.ok() && final_config->has_value() && (**final_config)["pipeline"]["generated"].as<bool>() &&
          (**final_config)["hstream_ui"]["keep"].as<bool>(),
      "Configurator first save must retain keys created after its absent baseline");

  ::unsetenv("HM_GAME_DIR");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
