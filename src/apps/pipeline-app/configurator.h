#pragma once

#include "absl/status/statusor.h"
#include "absl/strings/match.h"

#include "yaml-cpp/yaml.h"

#include <limits>
#include <optional>
#include <string>

#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/filesystem.h"

struct NvDsConfig;
struct NvDsPipeline;

namespace hm {
class Configurator {
 public:
  static constexpr int kUseConfigFileGpu = -1;

  Configurator(const std::string& game_id, const std::string& config_root_dir, int override_gpu_id);
  virtual ~Configurator();
  absl::StatusOr<YAML::Node> load_config();
  void set_output_overrides(const std::optional<int>& output_width, const std::optional<int>& output_height);

  virtual absl::Status configure();

  bool underlay_config(const std::string& node_name, const std::string& filename);
  bool overlay_config(const std::string& node_name, const std::string& filename);
  
  absl::Status apply_config_item(const std::string& key, const std::string& value);

  std::optional<YAML::Node> load_private_config();
  absl::Status save_private_config(const YAML::Node& private_config);

  static std::filesystem::path get_game_dir(const std::string& game_id);
  static std::filesystem::path get_private_config_file_name(const std::string& game_id);

  template <typename T_ENUM>
  std::vector<size_t> enable_sections(
      const std::string& section_prefix,
      const std::string& enum_field_name,
      const std::set<T_ENUM>& enable_field_values,
      bool disable_others,
      const std::string& return_id_label);

  // return vector of source id's
  std::vector<size_t> enable_source_types(const std::set<NvDsSourceType>& source_enums, bool disable_others);
  size_t disable_source_types(const std::set<NvDsSourceType>& source_enums);

  const YAML::Node& config() const {
    return config_;
  }

  absl::Status complete_configuration(bool force);

  absl::Status post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config, uint64_t start_time_ns);

  absl::StatusOr<bool> does_need_stitching(const std::string& game_dir) const;

  absl::Status load_sub_configs(
      const std::string& parent_node_name,
      const std::vector<std::string>& allowed_prefixes,
      const std::string& config_path);

 private:
  // Refactoring helpers to keep complete_configuration() readable
  void apply_gpu_override(YAML::Node& pipeline);
  absl::Status setup_stitcher_and_masks(YAML::Node& pipeline, const std::filesystem::path& game_dir, bool force, bool& has_hmstitcher);
  void map_common_config_keys();
  void apply_scoreboard_perspective(YAML::Node& pipeline);
  absl::Status gather_stitching_videos(const std::filesystem::path& game_dir, bool force, std::vector<std::string>& left_files, std::vector<std::string>& right_files, YAML::Node& offsets);
  void apply_frame_offsets_and_sizes(const std::vector<std::string>& left_files, const std::vector<std::string>& right_files, const YAML::Node& offsets, size_t& ww, size_t& hh, size_t& area, YAML::Node& pipeline);
  std::tuple<long,long> scaled_for_udp(bool is_udp_output, long width, long height) const;
  absl::Status set_output_dimensions(YAML::Node& pipeline, bool is_camera_source, const std::map<int, YAML::Node>& camera_sources, const std::vector<std::string>& left_files, const std::vector<std::string>& right_files, bool has_hmstitcher, const std::filesystem::path& game_dir, size_t& ww, size_t& hh, size_t& area, size_t& num_video_sources);
  void configure_audio(YAML::Node& pipeline, const std::vector<std::string>& left_files, const std::vector<std::string>& right_files, const YAML::Node& offsets, size_t& num_video_sources);
  void log_enabled_bins(const YAML::Node& pipeline) const;

  std::string file_maybe_in_game_dir(const std::string& basename);
  YAML::Node merge_nodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest);

  YAML::Node auto_config(YAML::Node&& config);

  const std::string game_id_;
  const std::string config_root_dir_;
  const int override_gpu_id_;

  // The fully-realzied merged config
  YAML::Node config_;
  YAML::Node private_config_;

  bool set_stream_offsets_{false};
  std::optional<int> output_width_override_;
  std::optional<int> output_height_override_;
};

template <typename T_ENUM>
inline std::vector<size_t> Configurator::enable_sections(
    const std::string& section_prefix,
    const std::string& enum_field_name,
    const std::set<T_ENUM>& enable_field_values,
    bool disable_others,
    const std::string& return_id_label) {
  if (enable_field_values.empty()) {
    return {};
  }
  std::vector<size_t> enabled_ids;
  YAML::Node pipeline = config_["pipeline"];
  if (!pipeline.IsDefined()) {
    return enabled_ids;
  }
  for (auto kv : pipeline) {
    std::string key = kv.first.as<std::string>();
    if (absl::StartsWith(key, section_prefix)) {
      YAML::Node section_node = kv.second;
      constexpr int kInvalid = std::numeric_limits<int>::max();
      const T_ENUM type = static_cast<T_ENUM>(get_node_value(section_node, enum_field_name, kInvalid));
      if (static_cast<int>(type) == kInvalid) {
        std::cerr << "Entry has no type" << std::endl;
        continue;
      }
      if (enable_field_values.count(type)) {
        section_node["enable"] = "1";
        if (!has_node(section_node, return_id_label, /*non_null=*/true)) {
          std::cerr << "No " << return_id_label << " in enabled " << section_prefix << " section: " << key << std::endl;
          enabled_ids.emplace_back(std::numeric_limits<size_t>::max());
        } else {
          enabled_ids.emplace_back(section_node[return_id_label].as<int>());
        }
      } else if (disable_others) {
        section_node["enable"] = "0";
      }
      // std::cout << "\n" << key << ":\n" << section_node << "\n" << std::endl;
    }
  }
  return enabled_ids;
}

} // namespace hm
