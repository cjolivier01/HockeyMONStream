#include "configurator.h"
#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace hm {
Configurator::Configurator(const std::string& game_id, const std::string& config_root_dir)
    : game_id_(game_id), config_root_dir_(config_root_dir) {
  // Constructor
}
Configurator::~Configurator() {
  // Destructor
}

std::filesystem::path Configurator::get_game_dir(const std::string& game_id) {
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

std::filesystem::path Configurator::get_private_config_file_name(const std::string& game_id) {
  return get_game_dir(game_id) / "config.yaml";
}

std::optional<YAML::Node> Configurator::load_private_config() {
  std::string private_config_file = get_private_config_file_name(game_id_);
  if (!std::filesystem::exists(private_config_file)) {
    return std::nullopt;
  }
  return YAML::LoadFile(private_config_file);
}

void Configurator::save_private_config(const YAML::Node& private_config) {}

YAML::Node Configurator::load_config() {
  YAML::Node config;
  if (!config_root_dir_.empty()) {
    std::filesystem::path baseline_path = std::filesystem::path(config_root_dir_) / "baseline.yaml";
    if (std::filesystem::exists(baseline_path)) {
      config = YAML::LoadFile(baseline_path);
    }
  }
  std::optional<YAML::Node> private_config = load_private_config();
  if (private_config.has_value()) {
    config = merge_nodes(config, *private_config, /*warn_if_key_not_in_dest=*/!config);
  }
  return config;
}

bool Configurator::underlay_config(const std::string& node_name, const std::string& filename) {
  if (!std::filesystem::exists(filename)) {
    return false;
  }
  YAML::Node underlaid_config = YAML::LoadFile(filename);
  if (node_name.empty()) {
    config_ = merge_nodes(underlaid_config, config_, /*warn_if_key_not_in_dest=*/false);
  } else {
    config_[node_name] = merge_nodes(underlaid_config, config_[node_name], /*warn_if_key_not_in_dest=*/false);
  }
  return true;
}

bool Configurator::overlay_config(const std::string& node_name, const std::string& filename) {
  if (!std::filesystem::exists(filename)) {
    return false;
  }
  YAML::Node overlaid_config = YAML::LoadFile(filename);
  if (node_name.empty()) {
    config_ = merge_nodes(config_, overlaid_config, /*warn_if_key_not_in_dest=*/false);
  } else {
    config_[node_name] = merge_nodes(config_[node_name], overlaid_config, /*warn_if_key_not_in_dest=*/false);
  }
  return true;
}

YAML::Node Configurator::merge_nodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest) {
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
      result[key] = merge_nodes(base[key], pair.second, warn_if_key_not_in_dest);
    } else {
      result[key] = pair.second;
    }
  }

  return result;
}

void Configurator::configure() {
  YAML::Node config = Configurator::load_config();
  config_ = auto_config(std::move(config));
}

YAML::Node Configurator::auto_config(YAML::Node&& config) {
  std::cout << config << std::endl;
  return std::move(config);
}

void Configurator::complete_configuration() {
  YAML::Node pipeline = config_["pipeline"];
  assert(pipeline.IsDefined()); 
  // 
}

} // namespace hm
