#include "apps/pipeline-app/configurator.h"
#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

namespace hm {
Configurator::Configurator(
    const std::string& game_id,
    const std::string& game_root_dir,
    const std::string& config_root_dir)
    : game_id_(game_id), config_root_dir_(config_root_dir), game_root_dir_(game_root_dir) {
  // Constructor
}
Configurator::~Configurator() {
  // Destructor
}

YAML::Node& Configurator::amend_config(const YAML::Node& new_config, YAML::Node& base_config) {
  return base_config;
}

void Configurator::configure() {
  // Configure
}
} // namespace hm
