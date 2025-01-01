#pragma once

#include "ConfigYaml.h"

#include <string>

namespace hm {
class Configurator {
 public:
  Configurator(const std::string& game_id, const std::string& game_root_dir, const std::string& config_root_dir);
  virtual ~Configurator();
  void configure();

 private:
  YAML::Node& amend_config(const YAML::Node& new_config, YAML::Node& base_config);

  const std::string game_id_;
  const std::string config_root_dir_;
  const std::string game_root_dir_;

  // The fully-realzied merged config
  const YAML::Node config_;
};
} // namespace hm
