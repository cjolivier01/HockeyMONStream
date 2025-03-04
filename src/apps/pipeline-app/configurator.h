#pragma once

#include "yaml-cpp/yaml.h"
#include "absl/status/status.h"

#include <filesystem>
#include <optional>
#include <string>

struct NvDsConfig;
struct NvDsPipeline;

namespace hm {
class Configurator {
 public:
  Configurator(const std::string& game_id, const std::string& config_root_dir);
  virtual ~Configurator();
  YAML::Node load_config();

  virtual void configure();

  bool underlay_config(const std::string& node_name, const std::string& filename);
  bool overlay_config(const std::string& node_name, const std::string& filename);

  std::optional<YAML::Node> load_private_config();
  void save_private_config(const YAML::Node& private_config);

  static std::filesystem::path get_game_dir(const std::string& game_id);
  static std::filesystem::path get_private_config_file_name(const std::string& game_id);

  const YAML::Node& config() const {
    return config_;
  }

  absl::Status complete_configuration();

  bool post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config);

 private:
  std::string file_maybe_in_game_dir(const std::string& basename);
  YAML::Node merge_nodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest);

  YAML::Node auto_config(YAML::Node&& config);

  const std::string game_id_;
  const std::string config_root_dir_;

  // The fully-realzied merged config
  YAML::Node config_;

  bool set_stream_offsets_{false};
};

} // namespace hm
