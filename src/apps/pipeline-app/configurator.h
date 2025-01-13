#pragma once

#include "src/libs/common/ConfigYaml.h"

#include <string>

namespace hm {
class Configurator {
 public:
  Configurator(const std::string& game_id, const std::string& config_root_dir);
  virtual ~Configurator();
  YAML::Node load_config();
  void configure();

  static std::string get_game_dir(const std::string& game_id);

 private:
  YAML::Node& amend_config(const YAML::Node& new_config, YAML::Node& base_config);

  const std::string game_id_;
  const std::string config_root_dir_;

  // The fully-realzied merged config
  const YAML::Node config_;
};
} // namespace hm
