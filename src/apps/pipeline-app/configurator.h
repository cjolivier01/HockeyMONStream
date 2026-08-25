#pragma once

#include "absl/status/statusor.h"
#include "absl/strings/match.h"

#include "yaml-cpp/yaml.h"

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/filesystem.h"
#include "hstream/src/libs/common/pipeline_utils.h"

struct NvDsConfig;
struct NvDsPipeline;

namespace hm {
namespace configurator_internal {

struct ExplicitStitchingVideoSelection {
  std::vector<std::string> left;
  std::vector<std::string> right;
  bool left_is_explicit{false};
  bool right_is_explicit{false};
  bool ui_roles_are_authoritative{false};
  std::string error;
};

ExplicitStitchingVideoSelection select_explicit_stitching_videos(const YAML::Node& config, bool force);
absl::Status validate_mixed_explicit_auto_playlists(
    bool left_is_explicit,
    bool right_is_explicit,
    size_t auto_left_chapters,
    size_t auto_right_chapters);
absl::StatusOr<std::optional<std::filesystem::path>> preserve_existing_archive_work_file(
    const std::filesystem::path& output_path);
absl::StatusOr<std::filesystem::path> reserve_unique_archive_work_file(
    const std::filesystem::path& configured_path,
    const std::string& run_id);
std::filesystem::path archive_work_owner_lock_path(const std::filesystem::path& work_path);
absl::StatusOr<int> acquire_archive_work_owner_lock(const std::filesystem::path& work_path);
absl::StatusOr<int> acquire_archive_output_lock(const std::filesystem::path& configured_path);
absl::Status claim_unique_archive_output_path(
    std::map<std::string, std::string>& claimed_paths,
    const std::filesystem::path& configured_path,
    const std::string& sink_name);
absl::StatusOr<std::vector<std::filesystem::path>> recover_stale_archive_work_files(
    const std::filesystem::path& configured_path);
absl::StatusOr<double> effective_stitch_output_rotation(const YAML::Node& config);
bool hmstitcher_owns_stitching_cleanup(const YAML::Node& config);
std::vector<std::string> enabled_source_video_uris(const YAML::Node& pipeline);
using ConfigLeafRanks = std::map<std::string, int>;
absl::StatusOr<YAML::Node> build_effective_playtracker_config(
    const YAML::Node& effective_config,
    const ConfigLeafRanks& canonical_value_ranks,
    int native_base_rank,
    const YAML::Node& base_playtracker_config);

} // namespace configurator_internal

class Configurator {
 public:
  static constexpr int kUseConfigFileGpu = -1;

  Configurator(const std::string& game_id, const std::string& config_root_dir, int override_gpu_id);
  virtual ~Configurator();
  absl::StatusOr<YAML::Node> load_config();

  virtual absl::Status configure();

  bool underlay_config(const std::string& node_name, const std::string& filename);
  bool overlay_config(const std::string& node_name, const std::string& filename);

  absl::Status apply_config_item(const std::string& key, const std::string& value);

  // Translate canonical baseline/user/game/CLI settings into the native
  // pipeline properties that currently implement those features.
  absl::Status apply_supported_baseline_mappings();

  absl::StatusOr<std::optional<YAML::Node>> load_private_config();
  absl::Status save_private_config(
      const YAML::Node& private_config,
      const std::string& expected_invalidation_id = {},
      bool remove_rink_masks = false);
  absl::Status persist_stitch_frame_time_override(const std::string& normalized_stitch_frame_time);
  absl::Status persist_effective_stitching_backend_choices(const std::string& expected_invalidation_id = {});
  absl::StatusOr<bool> reconcile_stitch_frame_time_override(
      const std::string& normalized_stitch_frame_time,
      const std::string& expected_invalidation_id = {});

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
  const YAML::Node& game_private_config() const {
    return private_config_;
  }

  bool stitching_calibration_required() const {
    return stitching_calibration_required_;
  }
  const std::string& active_stitching_invalidation_id() const {
    return active_stitching_invalidation_id_;
  }

  absl::Status complete_configuration(
      bool force,
      bool clean_stitching_artifacts = false,
      bool clean_stitching_from_control_points = false,
      const std::string& clean_expected_invalidation_id = {},
      bool show_render_sink = false,
      double show_render_scale = -1.0,
      const std::filesystem::path& pipeline_config_dir = {});

  absl::Status prepare_initial_pipeline_position(
      NvDsPipeline& pipeline,
      const NvDsConfig& config,
      uint64_t start_time_ns);
  absl::Status post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config, uint64_t start_time_ns);

  absl::StatusOr<bool> does_need_stitching(const std::string& game_dir) const;

  absl::Status load_sub_configs(
      const std::string& parent_node_name,
      const std::vector<std::string>& allowed_prefixes,
      const std::string& config_path);

 private:
  absl::Status ensure_user_config_snapshot();
  std::filesystem::path resolved_game_dir();
  void record_explicit_overlay(const YAML::Node& overlay, const std::string& prefix, int rank);
  int explicit_value_rank(const std::string& path) const;

  // Refactoring helpers to keep complete_configuration() readable
  void apply_gpu_override(YAML::Node& pipeline);
  absl::Status setup_stitcher_and_masks(
      YAML::Node& pipeline,
      const std::filesystem::path& game_dir,
      bool force,
      bool& has_hmstitcher);
  absl::Status map_common_config_keys();
  absl::Status materialize_playtracker_config(
      YAML::Node& pipeline,
      const std::filesystem::path& game_dir,
      const std::filesystem::path& pipeline_config_dir);
  absl::Status invalidate_rotation_dependent_cache_if_needed(const std::filesystem::path& game_dir);
  absl::Status invalidate_canvas_dependent_cache_if_needed(const std::filesystem::path& game_dir);
  absl::Status apply_scoreboard_perspective(YAML::Node& pipeline);
  absl::Status gather_stitching_videos(
      const std::filesystem::path& game_dir,
      bool force,
      std::vector<std::string>& left_files,
      std::vector<std::string>& right_files,
      YAML::Node& offsets);
  void apply_frame_offsets_and_sizes(
      const std::vector<std::string>& left_files,
      const std::vector<std::string>& right_files,
      const YAML::Node& offsets,
      size_t& ww,
      size_t& hh,
      size_t& area,
      YAML::Node& pipeline);
  std::tuple<long, long> cap_playcropper_output(long width, long height) const;
  absl::Status set_output_dimensions(
      YAML::Node& pipeline,
      bool is_camera_source,
      const std::map<int, YAML::Node>& camera_sources,
      const std::vector<std::string>& left_files,
      const std::vector<std::string>& right_files,
      bool has_hmstitcher,
      const std::filesystem::path& game_dir,
      size_t& ww,
      size_t& hh,
      size_t& area,
      size_t& num_video_sources);
  void configure_audio(
      YAML::Node& pipeline,
      const std::vector<std::string>& left_files,
      const std::vector<std::string>& right_files,
      const YAML::Node& offsets,
      size_t& num_video_sources);
  absl::Status configure_encode_file_outputs(YAML::Node& pipeline, const std::vector<std::string>& source_video_paths)
      const;
  void log_enabled_bins(const YAML::Node& pipeline) const;

  std::string file_maybe_in_game_dir(const std::string& basename);
  YAML::Node merge_nodes(const YAML::Node& base, const YAML::Node& overlay, bool warn_if_key_not_in_dest);

  YAML::Node auto_config(YAML::Node&& config);

  const std::string game_id_;
  const std::string config_root_dir_;
  const int override_gpu_id_;

  // The fully-realzied merged config
  YAML::Node config_;
  // Bundled baseline plus the user overlay, before per-game values. This is
  // the lower layer used to decide whether a game must persist an explicit
  // zero/nonzero override.
  YAML::Node lower_layer_config_;
  // Leaf-path provenance: user=1, game=2, CLI=3. Structural app YAML is not
  // recorded, and bundled baseline is implicit rank 0.
  std::map<std::string, int> explicit_value_ranks_;
  std::optional<YAML::Node> user_config_snapshot_;
  std::optional<std::filesystem::path> resolved_game_dir_;
  YAML::Node private_config_;
  YAML::Node persisted_private_config_;
  std::string active_stitching_invalidation_id_;
  bool stitching_calibration_required_{false};
  bool loaded_generated_stitching_backend_choices_{false};
  bool restored_generated_stitching_backend_choices_{false};
  mutable std::map<std::string, int> archive_lock_fds_;
  mutable std::map<std::string, int> archive_work_lock_fds_;
  mutable std::map<std::string, std::filesystem::path> archive_run_paths_;
  std::map<std::string, int> playtracker_runtime_lock_fds_;

  bool set_stream_offsets_{false};
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
