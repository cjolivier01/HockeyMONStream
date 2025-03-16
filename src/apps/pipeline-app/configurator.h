#pragma once

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

#include "hstream/src/apps/apps-common/deepstream_sources.h"

struct NvDsConfig;
struct NvDsPipeline;

namespace hm {
class Configurator {
 public:
  Configurator(const std::string& game_id, const std::string& config_root_dir);
  virtual ~Configurator();
  absl::StatusOr<YAML::Node> load_config();

  virtual absl::Status configure();

  bool underlay_config(const std::string& node_name, const std::string& filename);
  bool overlay_config(const std::string& node_name, const std::string& filename);

  std::optional<YAML::Node> load_private_config();
  absl::Status save_private_config(const YAML::Node& private_config);

  static std::filesystem::path get_game_dir(const std::string& game_id);
  static std::filesystem::path get_private_config_file_name(const std::string& game_id);

  // return vector of source id's
  std::vector<size_t> enable_source_types(const std::set<NvDsSourceType>& source_enums, bool disable_others);
  size_t disable_source_types(const std::set<NvDsSourceType>& source_enums);

  const YAML::Node& config() const {
    return config_;
  }

  absl::Status complete_configuration(bool force);

  absl::Status post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config);

  bool does_need_stitching(const std::string& game_dir) const;

  absl::Status load_sub_configs(
      const std::string& parent_node_name,
      const std::vector<std::string>& allowed_prefixes,
      const std::string& config_path);

 private:
  std::string file_maybe_in_game_dir(const std::string& basename);
  YAML::Node merge_nodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest);

  YAML::Node auto_config(YAML::Node&& config);

  const std::string game_id_;
  const std::string config_root_dir_;

  // The fully-realzied merged config
  YAML::Node config_;
  YAML::Node private_config_;

  bool set_stream_offsets_{false};
};

} // namespace hm
