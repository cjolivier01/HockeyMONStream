#include "configurator.h"
#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

#include <sstream>
#include <string>

#include <unistd.h>

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

std::string Configurator::get_game_dir(const std::string& game_id) {
  std::stringstream ss;
  const char* sprefix = ::getenv("HM_GAME_DIR");
  if (!sprefix) {
    const char* homedir = ::getenv("HOME");
    if (homedir) {
      ss << homedir << '/' << "Videos/";
    } else {
      ss << "/games/";
    }
  }
  ss << game_id << '/';
  return ss.str();
}

YAML::Node& Configurator::amend_config(const YAML::Node& new_config, YAML::Node& base_config) {
  return base_config;
}

void Configurator::configure() {
  // Configure
}
} // namespace hm
