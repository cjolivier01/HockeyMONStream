#pragma once

#include "src/libs/common/ConfigYaml.h"

#include <filesystem>
#include <optional>
#include <string>

namespace hm {
class Configurator {
 public:
  Configurator(const std::string& game_id, const std::string& config_root_dir);
  virtual ~Configurator();
  YAML::Node load_config();
  void configure();

  std::optional<YAML::Node> load_private_config();
  void save_private_config(const YAML::Node& private_config);

  static std::filesystem::path get_game_dir(const std::string& game_id);
  static std::filesystem::path get_private_config_file_name(const std::string& game_id);

 private:
  YAML::Node merge_nodes(const YAML::Node& base, const YAML::Node& overlay);

  const std::string game_id_;
  const std::string config_root_dir_;

  // The fully-realzied merged config
  const YAML::Node config_;
};
} // namespace hm
