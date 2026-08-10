#include "hstream/src/apps/apps-common/deepstream_common.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/utils.h"

#include "deepstream_app.h"
// #include "gst-nvdscommonconfig.h"

#include "absl/status/statusor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

#include <stdlib.h>
#include <yaml-cpp/node/parse.h>
#include <filesystem>
#include <fstream>

using std::cout;
using std::endl;

namespace fs = std::filesystem;

namespace {

bool find_yaml_alias(
    const YAML::Node& yaml_node,
    const char* dashed_key,
    const char* underscored_key,
    YAML::Node* out) {
  if (!out) {
    return false;
  }
  if (yaml_node[dashed_key].IsDefined()) {
    *out = yaml_node[dashed_key];
    return true;
  }
  if (underscored_key) {
    if (yaml_node[underscored_key].IsDefined()) {
      *out = yaml_node[underscored_key];
      return true;
    }
  }
  return false;
}

bool parse_finite_yaml_double(
    const YAML::Node& yaml_node,
    const char* dashed_key,
    const char* underscored_key,
    double* out,
    bool* present = nullptr) {
  YAML::Node node;
  const bool found = find_yaml_alias(yaml_node, dashed_key, underscored_key, &node);
  if (present) {
    *present = found;
  }
  if (!found) {
    return true;
  }
  try {
    const double value = node.as<double>();
    if (!std::isfinite(value)) {
      cout << "Invalid non-finite value for " << dashed_key << endl;
      return false;
    }
    if (out) {
      *out = value;
    }
  } catch (const std::exception& exc) {
    cout << "Invalid numeric value for " << dashed_key << ": " << exc.what() << endl;
    return false;
  }
  return true;
}

} // namespace

static int get_trailing_integer(const std::string& input) {
  int len = input.length();
  int end = len - 1;

  // Find the last character that is a digit
  while (end >= 0 && std::isdigit(input[end])) {
    --end;
  }

  // If no digits were found at the end of the string
  if (end == len - 1) {
    return 0;
  }

  // Extract the substring from the first digit of the continuous number segment to the end
  std::string numberStr = input.substr(end + 1, len - end - 1);
  return std::stoi(numberStr); // Convert to integer
}

static gboolean parse_tests_yaml(NvDsConfig* config, const YAML::Node& yaml_ndoe) {
  gboolean ret = FALSE;
  for (YAML::const_iterator itr = yaml_ndoe.begin(); itr != yaml_ndoe.end(); ++itr) {
    std::string paramKey = itr->first.as<std::string>();
    if (paramKey == "file-loop") {
      config->file_loop = itr->second.as<gint>();
    } else {
      cout << "Unknown key " << paramKey << " for group tests" << endl;
    }
  }

  ret = TRUE;

  if (!ret) {
    cout << __func__ << " failed" << endl;
  }
  return ret;
}

gboolean parse_dsplaytracker_yaml(
    NvDsDsPlayTrackerConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir) {
  hm::utils::ConfigLocator locator;
  locator.ignored.emplace("config-file");
  locator.ignored.emplace("properties");
  locator.ignored.emplace("private-properties");
  // Nested config consumed by the vpplaytracker plugin.
  locator.ignored.emplace("play-tracker");
  SET_LOCATOR(locator, *config, enable);
  SET_LOCATOR(locator, *config, unique_id);
  SET_LOCATOR(locator, *config, gpu_id);
  SET_LOCATOR(locator, *config, draw);
  SET_LOCATOR(locator, *config, show);
  bool fixed_edge_rotation_angle_present = false;
  double fixed_edge_rotation_angle = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle",
          "fixed_edge_rotation_angle",
          &fixed_edge_rotation_angle,
          &fixed_edge_rotation_angle_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_present) {
    config->fixed_edge_rotation_angle = static_cast<gfloat>(fixed_edge_rotation_angle);
  }
  bool fixed_edge_rotation_angle_left_present = false;
  double fixed_edge_rotation_angle_left = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle-left",
          "fixed_edge_rotation_angle_left",
          &fixed_edge_rotation_angle_left,
          &fixed_edge_rotation_angle_left_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_left_present) {
    config->fixed_edge_rotation_angle_left = static_cast<gfloat>(fixed_edge_rotation_angle_left);
  }
  bool fixed_edge_rotation_angle_right_present = false;
  double fixed_edge_rotation_angle_right = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle-right",
          "fixed_edge_rotation_angle_right",
          &fixed_edge_rotation_angle_right,
          &fixed_edge_rotation_angle_right_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_right_present) {
    config->fixed_edge_rotation_angle_right = static_cast<gfloat>(fixed_edge_rotation_angle_right);
  }
  double dynamic_acceleration_scaling = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node, "dynamic-acceleration-scaling", "dynamic_acceleration_scaling", &dynamic_acceleration_scaling)) {
    return false;
  }
  YAML::Node dynamic_acceleration_scaling_node;
  if (find_yaml_alias(
          yaml_node,
          "dynamic-acceleration-scaling",
          "dynamic_acceleration_scaling",
          &dynamic_acceleration_scaling_node)) {
    config->dynamic_acceleration_scaling = static_cast<gfloat>(dynamic_acceleration_scaling);
  }
  config->fixed_edge_rotation_angle_set = fixed_edge_rotation_angle_present;
  config->fixed_edge_rotation_angle_left_set = fixed_edge_rotation_angle_left_present;
  config->fixed_edge_rotation_angle_right_set = fixed_edge_rotation_angle_right_present;
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle);
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle_left);
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle_right);
  SET_LOCATOR(locator, *config, dynamic_acceleration_scaling);
  hm::utils::parse_chracter_buffer(config->config_file, yaml_node, "config-file", config_dir);
  if (!hm::gst::append_plugin_properties_from_yaml(yaml_node, "properties", &config->plugin_properties) ||
      !hm::gst::append_plugin_private_properties_from_yaml(
          yaml_node, "private-properties", &config->private_properties)) {
    return false;
  }
  set_config_from_yaml(yaml_node, locator);
  return true;
}

gboolean parse_dsfieldmask_yaml(
    NvDsDsFieldMaskConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir) {
  hm::utils::ConfigLocator locator{.ignored = {"detection-mask", "properties"}};
  SET_LOCATOR(locator, *config, enable);
  SET_LOCATOR(locator, *config, unique_id);
  SET_LOCATOR(locator, *config, gpu_id);
  SET_LOCATOR(locator, *config, nvbuf_memory_type);
  SET_LOCATOR_CHARS(locator, *config, detection_mask_file);
  hm::utils::parse_chracter_buffer(config->detection_mask_file, yaml_node, "detection-mask", config_dir);
  if (!hm::gst::append_plugin_properties_from_yaml(yaml_node, "properties", &config->plugin_properties)) {
    return false;
  }
  set_config_from_yaml(yaml_node, locator);
  return true;
}

gboolean parse_videoprep_yaml(
    NvDsHmVideoPrepConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir,
    bool quiet = false) {
  hm::utils::ConfigLocator locator{
      .ignored = {"config-file", "configure-only", "one-pass-mode", "properties", "private-properties"}};
  SET_LOCATOR(locator, *config, enable);
  SET_LOCATOR(locator, *config, unique_id);
  SET_LOCATOR(locator, *config, gpu_id);
  SET_LOCATOR(locator, *config, show);
  SET_LOCATOR(locator, *config, has_queue);
  SET_LOCATOR(locator, *config, has_videoconvert);
  SET_LOCATOR(locator, *config, nvbuf_memory_type);
  SET_LOCATOR(locator, *config, num_output_buffers);
  SET_LOCATOR(locator, *config, output_width);
  SET_LOCATOR(locator, *config, output_height);
  SET_LOCATOR(locator, *config, dewarper_dump_frames);
  SET_LOCATOR(locator, *config, source_id);
  SET_LOCATOR(locator, *config, num_surfaces_per_frame);
  SET_LOCATOR(locator, *config, num_batch_buffers);
  SET_LOCATOR_CHARS(locator, *config, plugin_type);
  SET_LOCATOR_CHARS(locator, *config, plugin_private_config);

  hm::utils::parse_chracter_buffer(config->config_file, yaml_node, "config-file", config_dir);
  if (!hm::gst::append_plugin_properties_from_yaml(yaml_node, "properties", &config->plugin_properties) ||
      !hm::gst::append_plugin_private_properties_from_yaml(
          yaml_node, "private-properties", &config->private_properties)) {
    return false;
  }

  set_config_from_yaml(yaml_node, locator, quiet);
  return true;
}

gboolean parse_hmplaycropper_yaml(
    HmPlayCropperConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir,
    bool quiet = false) {
  if (!parse_videoprep_yaml(config, yaml_node, config_dir, /*quiet=*/true)) {
    return false;
  }
  hm::utils::ConfigLocator locator;
  // Base videoprep fields were parsed above; ignore them here so we only warn on truly unknown keys.
  locator.ignored = {
      "enable",
      "unique_id",
      "gpu_id",
      "show",
      "has_queue",
      "has_videoconvert",
      "nvbuf_memory_type",
      "num_output_buffers",
      "output_width",
      "output_height",
      "dewarper_dump_frames",
      "source_id",
      "num_surfaces_per_frame",
      "num_batch_buffers",
      "plugin_type",
      "plugin_private_config",
      "config_file",
      "configure_only",
      "one_pass_mode",
      "properties",
      "private_properties",
      "private-properties",
  };
  SET_LOCATOR(locator, *config, plot_play_tracking);
  SET_LOCATOR(locator, *config, plot_player_tracking);
  SET_LOCATOR(locator, *config, transform_object_meta);
  bool fixed_edge_rotation_angle_present = false;
  double fixed_edge_rotation_angle = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle",
          "fixed_edge_rotation_angle",
          &fixed_edge_rotation_angle,
          &fixed_edge_rotation_angle_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_present) {
    config->fixed_edge_rotation_angle = static_cast<gfloat>(fixed_edge_rotation_angle);
  }
  config->fixed_edge_rotation_angle_set = fixed_edge_rotation_angle_present;
  bool fixed_edge_rotation_angle_left_present = false;
  double fixed_edge_rotation_angle_left = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle-left",
          "fixed_edge_rotation_angle_left",
          &fixed_edge_rotation_angle_left,
          &fixed_edge_rotation_angle_left_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_left_present) {
    config->fixed_edge_rotation_angle_left = static_cast<gfloat>(fixed_edge_rotation_angle_left);
  }
  bool fixed_edge_rotation_angle_right_present = false;
  double fixed_edge_rotation_angle_right = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node,
          "fixed-edge-rotation-angle-right",
          "fixed_edge_rotation_angle_right",
          &fixed_edge_rotation_angle_right,
          &fixed_edge_rotation_angle_right_present)) {
    return false;
  }
  if (fixed_edge_rotation_angle_right_present) {
    config->fixed_edge_rotation_angle_right = static_cast<gfloat>(fixed_edge_rotation_angle_right);
  }
  config->fixed_edge_rotation_angle_left_set = fixed_edge_rotation_angle_left_present;
  config->fixed_edge_rotation_angle_right_set = fixed_edge_rotation_angle_right_present;
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle);
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle_left);
  SET_LOCATOR(locator, *config, fixed_edge_rotation_angle_right);
  SET_LOCATOR(locator, *config, no_crop);
  SET_LOCATOR(locator, *config, show_scoreboard);
  SET_LOCATOR_CHARS(locator, *config, scoreboard_projected_width);
  SET_LOCATOR_CHARS(locator, *config, scoreboard_projected_height);
  SET_LOCATOR(locator, *config, scoreboard_scale);
  SET_LOCATOR(locator, *config, runtime_output_max_width);
  SET_LOCATOR(locator, *config, runtime_output_max_height);
  SET_LOCATOR_INTS(locator, *config, scoreboard_perspective_polygon);

  set_config_from_yaml(yaml_node, locator, quiet);
  return true;
}

gboolean parse_hmstitcher_yaml(HmStitcherConfig* config, const YAML::Node& yaml_node, const std::string& config_path) {
  if (!parse_videoprep_yaml(config, yaml_node, config_path, /*quiet=*/true)) {
    return false;
  }
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, *config, configure_only);
  SET_LOCATOR(locator, *config, one_pass_mode);
  SET_LOCATOR(locator, *config, left_frame_offset_ns);
  SET_LOCATOR(locator, *config, right_frame_offset_ns);
  SET_LOCATOR(locator, *config, show);
  SET_LOCATOR(locator, *config, force_scoreboard_config);
  double post_stitch_rotate_degrees = 0.0;
  if (!parse_finite_yaml_double(
          yaml_node, "post-stitch-rotate-degrees", "post_stitch_rotate_degrees", &post_stitch_rotate_degrees)) {
    return false;
  }
  YAML::Node post_stitch_rotate_degrees_node;
  if (find_yaml_alias(
          yaml_node, "post-stitch-rotate-degrees", "post_stitch_rotate_degrees", &post_stitch_rotate_degrees_node)) {
    config->post_stitch_rotate_degrees = static_cast<gfloat>(post_stitch_rotate_degrees);
  }
  SET_LOCATOR(locator, *config, post_stitch_rotate_degrees);
  SET_LOCATOR(locator, *config, minimize_blend);
  SET_LOCATOR(locator, *config, ui_preview);
  SET_LOCATOR_CHARS(locator, *config, stitch_compute_precision);
  SET_LOCATOR_CHARS(locator, *config, config_file);
  locator.ignored.emplace("properties");
  locator.ignored.emplace("private-properties");
  set_config_from_yaml(yaml_node, locator, /*quiet=*/true);
  return true;
}

gboolean parse_hmimagemetamerger_yaml(NvDsHmImageMetaMergerConfig* config, const YAML::Node& yaml_node) {
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, *config, enable);
  SET_LOCATOR(locator, *config, unique_id);
  SET_LOCATOR(locator, *config, gpu_id);
  SET_LOCATOR(locator, *config, nvbuf_memory_type);
  set_config_from_yaml(yaml_node, locator);
  return true;
}

gboolean parse_hmaudio_yaml(NvDsHmAudioConfig* config, const YAML::Node& yaml_node) {
  std::fill(std::begin(config->multi_sink_ids), std::end(config->multi_sink_ids), -1);
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, *config, enable);
  SET_LOCATOR(locator, *config, src);
  SET_LOCATOR(locator, *config, source_id);
  SET_LOCATOR(locator, *config, dest);
  SET_LOCATOR(locator, *config, sink_id);
  SET_LOCATOR_INTS(locator, *config, multi_sink_ids);
  SET_LOCATOR_CHARS(locator, *config, alsa_src_device);
  SET_LOCATOR_CHARS(locator, *config, alsa_dest_device);
  SET_LOCATOR_CHARS(locator, *config, audio_location);
  set_config_from_yaml(yaml_node, locator);
  return true;
}

gboolean parse_app_yaml(NvDsConfig* config, const YAML::Node& yaml_node) {
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, *config, stage);
  SET_LOCATOR(locator, *config, global_gpu_id);
  SET_LOCATOR(locator, *config, enable_perf_measurement);
  SET_LOCATOR(locator, *config, source_list_enabled);
  SET_LOCATOR(locator, *config, perf_measurement_interval_sec);
  SET_LOCATOR(locator, *config, sgie_batch_size);
  SET_LOCATOR(locator, *config, extract_sei_type5_data);
  SET_LOCATOR_CHAR_PTR(locator, *config, sei_uuid);
  SET_LOCATOR(locator, *config, low_latency_mode);

  // nvmultiurisrcbin support (YAML: use-nvmultiurisrcbin, stream-name-display, max-batch-size, http-ip, http-port).
  SET_LOCATOR(locator, *config, use_nvmultiurisrcbin);
  SET_LOCATOR(locator, *config, stream_name_display);
  SET_LOCATOR(locator, *config, max_batch_size);
  SET_LOCATOR_CHAR_PTR(locator, *config, http_ip);
  SET_LOCATOR_CHAR_PTR(locator, *config, http_port);

  // Optional output paths.
  SET_LOCATOR_CHAR_PTR(locator, *config, bbox_dir_path);
  SET_LOCATOR_CHAR_PTR(locator, *config, kitti_track_dir_path);
  SET_LOCATOR_CHAR_PTR(locator, *config, reid_track_dir_path);
  SET_LOCATOR_CHAR_PTR(locator, *config, terminated_track_output_path);
  SET_LOCATOR_CHAR_PTR(locator, *config, shadow_track_output_path);
  set_config_from_yaml(yaml_node, locator);
  return true;
}

static std::vector<std::string> split_csv_entries(std::string input) {
  std::vector<int> positions;
  for (unsigned int i = 0; i < input.size(); i++) {
    if (input[i] == ',')
      positions.push_back(i);
  }
  std::vector<std::string> ret;
  int prev = 0;
  for (auto& j : positions) {
    std::string temp = input.substr(prev, j - prev);
    ret.push_back(temp);
    prev = j + 1;
  }
  ret.push_back(input.substr(prev, input.size() - prev));
  return ret;
}

absl::StatusOr<YAML::Node> get_app_config(const gchar* cfg_file_path) {
  if (!cfg_file_path || !*cfg_file_path) {
    return absl::InvalidArgumentError("No config file specified");
  }
  if (!fs::exists(cfg_file_path)) {
    return absl::NotFoundError(TO_STRING("Could not find file: " << cfg_file_path));
  }
  YAML::Node config = YAML::LoadFile(cfg_file_path);
  return config["application"];
}

gboolean parse_config_yaml(const YAML::Node& configyml, NvDsConfig* config, const std::string& config_dir) {
  gboolean parse_err = false;
  gboolean ret = FALSE;
  std::string source_str = "source";
  std::string sink_str = "sink";
  std::string sgie_str = "secondary-gie";
  std::string msgcons_str = "message-consumer";
  std::string dewarper_str = "dewarper";

  config->source_list_enabled = FALSE;

  /** Initialize global gpu id to -1 */
  config->global_gpu_id = -1;
  /** App group parsing at top level to set global_gpu_id (if available)
   * before any other group parsing */
  parse_err = !parse_app_yaml(config, configyml["application"]);

  for (YAML::const_iterator itr = configyml.begin(); itr != configyml.end(); ++itr) {
    std::string paramKey = itr->first.as<std::string>();
    if (paramKey == "source" || (paramKey.size() > 6 && paramKey.substr(0, 6) == "source")) {
      if (configyml[paramKey]["csv-file-path"].IsDefined()) {
        std::string csv_file_path = configyml["source"]["csv-file-path"].as<std::string>();
        char* str = (char*)malloc(sizeof(char) * 1024);
        std::strncpy(str, csv_file_path.c_str(), 1023);
        char* abs_csv_path = (char*)malloc(sizeof(char) * 1024);
        get_absolute_file_path_yaml(config_dir.c_str(), str, abs_csv_path);
        g_free(str);

        std::ifstream inputFile(abs_csv_path);
        if (!inputFile.is_open()) {
          cout << "Couldn't open CSV file " << abs_csv_path << endl;
        }
        std::string line, temp;
        /* Separating header field and inserting as strings into the vector.
         */
        while (getline(inputFile, line)) {
          gboolean is_comment = false;
          size_t space_count = 0;
          for (char c : line) {
            if (c != ' ' && c != '\t') {
              if (c != '#') {
                is_comment = false;
              } else {
                is_comment = true;
              }
              break;
            } else {
              space_count++;
            }
          }
          if (!is_comment && space_count < line.length())
            break;
        }
        std::vector<std::string> headers = split_csv_entries(line);
        /*Parsing each csv entry as an input source */
        while (getline(inputFile, line)) {
          if (line.empty() || line[0] == '#') {
            continue;
          }
          std::vector<std::string> source_values = split_csv_entries(line);
          if (config->num_source_sub_bins == MAX_SOURCE_BINS) {
            NVGSTDS_ERR_MSG_V("App supports max %d sources", MAX_SOURCE_BINS);
            ret = FALSE;
            goto done;
          }

          YAML::Node src_node;
          assert(headers.size() == source_values.size());

          for (size_t i = 0; i < headers.size(); ++i) {
            src_node[headers.at(i)] = source_values.at(i);
          }

          guint source_id = 0;
          source_id = config->num_source_sub_bins;
          /** set gpu_id for source component using global_gpu_id(if available) */
          if (config->global_gpu_id != -1) {
            config->multi_source_config[source_id].gpu_id = config->global_gpu_id;
          }
          /** if gpu_id for source component is present,
           * it will override the value set using global_gpu_id in parse_source_yaml function */
          parse_err = !parse_source_yaml(&config->multi_source_config[source_id], src_node, config_dir);
          if (config->multi_source_config[source_id].enable)
            config->num_source_sub_bins++;
        }
      } else {
        // YAML::Node source_node = configyml[paramKey];
        // std::vector<std::string> headers, source_values;
        // for (YAML::const_iterator itr = source_node.begin(); itr != source_node.end(); ++itr) {
        //   headers.emplace_back(itr->first.as<std::string>());
        //   source_values.emplace_back(itr->second.as<std::string>());
        // }
        if (config->num_source_sub_bins == MAX_SOURCE_BINS) {
          NVGSTDS_ERR_MSG_V("App supports max %d sources", MAX_SOURCE_BINS);
          ret = FALSE;
          goto done;
        }
        guint source_id = 0;
        source_id = config->num_source_sub_bins;
        /** set gpu_id for source component using global_gpu_id(if available) */
        if (config->global_gpu_id != -1) {
          config->multi_source_config[source_id].gpu_id = config->global_gpu_id;
        }

#if 0
        std::optional<YAML::Node> maybe_sub_config_file =
            maybe_get_config_file(configyml[paramKey], fs::path(cfg_file_path).parent_path());
        if (maybe_sub_config_file) {
          guint start_source_id = config->num_source_sub_bins;
          parse_err = !parse_config_yaml(*maybe_sub_config_file, config, cfg_file_path, /*parse_config_yaml=*/false);
          assert(!parse_err);
          if (start_source_id == config->num_source_sub_bins) {
            parse_err =
                !parse_source_yaml(&config->multi_source_config[source_id], configyml[paramKey], (char*)cfg_file_path);
            if (config->multi_source_config[source_id].enable)
              config->num_source_sub_bins++;
          } else {
            for (guint i = start_source_id; i < config->num_source_sub_bins; ++i) {
              // Local config overrides config file
              parse_err =
                  !parse_source_yaml(&config->multi_source_config[i], configyml[paramKey], (char*)cfg_file_path);
              assert(!parse_err);
            }
          }
        } else {
          parse_err =
              !parse_source_yaml(&config->multi_source_config[source_id], configyml[paramKey], (char*)cfg_file_path);
          if (config->multi_source_config[source_id].enable)
            config->num_source_sub_bins++;
        }
#else
        assert(source_id >= 0 && source_id < MAX_SOURCE_BINS);
        parse_err = !parse_source_yaml(&config->multi_source_config[source_id], configyml[paramKey], config_dir);
        if (config->multi_source_config[source_id].enable)
          config->num_source_sub_bins++;
#endif
        /** if gpu_id for source component is present,
         * it will override the value set using global_gpu_id in parse_source_yaml function */
        // parse_err = !parse_source_yaml(&config->multi_source_config[source_id], headers, source_values,
        // cfg_file_path);
        // parse_source_yaml(&config->multi_source_config[source_id], configyml[paramKey], (char*)cfg_file_path);
        // if (config->multi_source_config[source_id].enable)
        //   config->num_source_sub_bins++;
      }
    } else if (paramKey == "streammux") {
      /** set gpu_id for streammux component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->streammux_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for streammux component is present,
       * it will override the value set using global_gpu_id in parse_streammux_yaml function */
      parse_err = !parse_streammux_yaml(&config->streammux_config, configyml, config_dir.c_str());
    } else if (paramKey == "streammux2") {
      /** set gpu_id for streammux component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->streammux2_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for streammux component is present,
       * it will override the value set using global_gpu_id in parse_streammux_yaml function */
      parse_err = !parse_streammux_yaml(&config->streammux2_config, configyml, config_dir.c_str());
    } else if (paramKey == "osd") {
      /** set gpu_id for osd component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->osd_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for osd component is present,
       * it will override the value set using global_gpu_id in parse_osd_yaml function */
      parse_err = !parse_osd_yaml(&config->osd_config, itr->second);
    } else if (paramKey == "segvisual") {
      /**  set gpu_id for segvisual component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->segvisual_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for segvisual component is present,
       * it will override the value set using global_gpu_id in parse_segvisual_yaml function */
      parse_err = !parse_segvisual_yaml(&config->segvisual_config, config_dir.c_str());
    } else if (paramKey == "pre-process") {
      parse_err = !parse_preprocess_yaml(&config->preprocess_config, itr->second, config_dir);
    } else if (paramKey == "primary-gie") {
      /** set gpu_id for primary gie component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->primary_gie_config.gpu_id = config->global_gpu_id;
        config->primary_gie_config.is_gpu_id_set = TRUE;
      }
      /** if gpu_id for primary gie component is present,
       * it will override the value set using global_gpu_id in parse_gie_yaml function */
      parse_err = !parse_gie_yaml(&config->primary_gie_config, paramKey, itr->second, config_dir.c_str());
    } else if (paramKey == "tracker") {
      /** set gpu_id for tracker component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->tracker_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for tracker component is present,
       * it will override the value set using global_gpu_id in parse_tracker_yaml function */
      parse_err = !parse_tracker_yaml(&config->tracker_config, itr->second, config_dir);
    } else if (paramKey.compare(0, sgie_str.size(), sgie_str) == 0) {
      if (config->num_secondary_gie_sub_bins == MAX_SECONDARY_GIE_BINS) {
        NVGSTDS_ERR_MSG_V("App supports max %d secondary GIEs", MAX_SECONDARY_GIE_BINS);
        ret = FALSE;
        goto done;
      }
      /* set gpu_id for secondary gie component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->secondary_gie_sub_bin_config[config->num_secondary_gie_sub_bins].gpu_id = config->global_gpu_id;
        config->secondary_gie_sub_bin_config[config->num_secondary_gie_sub_bins].is_gpu_id_set = TRUE;
      }
      /** if gpu_id for secondary gie component is present,
       * it will override the value set using global_gpu_id in parse_gie_yaml function */
      parse_err = !parse_gie_yaml(
          &config->secondary_gie_sub_bin_config[config->num_secondary_gie_sub_bins], paramKey, itr->second, config_dir);
      if (config->secondary_gie_sub_bin_config[config->num_secondary_gie_sub_bins].enable) {
        config->num_secondary_gie_sub_bins++;
      }
    } else if (paramKey.compare(0, sink_str.size(), sink_str) == 0) {
      if (config->num_sink_sub_bins == MAX_SINK_BINS) {
        NVGSTDS_ERR_MSG_V("App supports max %d sinks", MAX_SINK_BINS);
        ret = FALSE;
        goto done;
      }

      /* set gpu_id for sink component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1 && configyml[paramKey]["enable"].as<gboolean>()) {
        config->sink_bin_sub_bin_config[config->num_sink_sub_bins].encoder_config.gpu_id =
            config->sink_bin_sub_bin_config[config->num_sink_sub_bins].render_config.gpu_id = config->global_gpu_id;
      }
      /**  if gpu_id for sink component is present,
       * it will override the value set using global_gpu_id in parse_sink_yaml function */

      parse_err = !parse_sink_yaml(
          &config->sink_bin_sub_bin_config[config->num_sink_sub_bins], paramKey, configyml, config_dir);
      if (config->sink_bin_sub_bin_config[config->num_sink_sub_bins].enable) {
        config->num_sink_sub_bins++;
      }
    } else if (paramKey.compare(0, msgcons_str.size(), msgcons_str) == 0) {
      if (config->num_message_consumers == MAX_MESSAGE_CONSUMERS) {
        NVGSTDS_ERR_MSG_V("App supports max %d consumers", MAX_MESSAGE_CONSUMERS);
        ret = FALSE;
        goto done;
      }
      parse_err = !parse_msgconsumer_yaml(
          &config->message_consumer_config[config->num_message_consumers], paramKey, config_dir.c_str());

      if (config->message_consumer_config[config->num_message_consumers].enable) {
        config->num_message_consumers++;
      }
    } else if (paramKey == "tiled-display") {
      /* set gpu_id for tiled display component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->tiled_display_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for tiled display component is present,
       * it will override the value set using global_gpu_id in parse_tiled_display_yaml function */
      parse_err = !parse_tiled_display_yaml(&config->tiled_display_config, itr->second);
    } else if (paramKey == "img-save") {
      /** set gpu_id for image save component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->image_save_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for image save component is present,
       * it will override the value set using global_gpu_id in parse_image_save_yaml function */
      parse_err = !parse_image_save_yaml(&config->image_save_config, config_dir.c_str());
    } else if (paramKey == "nvds-analytics") {
      parse_err = !parse_dsanalytics_yaml(&config->dsanalytics_config, config_dir.c_str());
    } else if (paramKey == "ds-example") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->dsexample_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_dsexample_yaml function */
      parse_err = !parse_dsexample_yaml(&config->dsexample_config, itr->second);
    } else if (paramKey == "ds-fieldmask") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->dsfieldmask_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_fieldmask_yaml function */
      parse_err = !parse_dsfieldmask_yaml(&config->dsfieldmask_config, itr->second, config_dir.c_str());
    } else if (paramKey == "hmstitcher") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->hmsticher_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_fieldmask_yaml function */
      // parse_err = !parse_hmstitcher_yaml(&config->hmsticher_config, itr->second);
      parse_err = !parse_hmstitcher_yaml(&config->hmsticher_config, itr->second, config_dir.c_str());
    } else if (paramKey == "ds-playtracker") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->dsplaytracker_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_playtracker_yaml function */
      parse_err = !parse_dsplaytracker_yaml(&config->dsplaytracker_config, itr->second, config_dir.c_str());
    } else if (paramKey == "hmplaycropper") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->hmplaycropper_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_playtracker_yaml function */
      parse_err = !parse_hmplaycropper_yaml(&config->hmplaycropper_config, itr->second, config_dir.c_str());
    } else if (paramKey == "hm-image-meta-merger") {
      /** set gpu_id for dsexample component using global_gpu_id(if available) */
      if (config->global_gpu_id != -1) {
        config->hmimagemetamerger_config.gpu_id = config->global_gpu_id;
      }
      /** if gpu_id for dsexample component is present,
       * it will override the value set using global_gpu_id in parse_playtracker_yaml function */
      parse_err = !parse_hmimagemetamerger_yaml(&config->hmimagemetamerger_config, itr->second);
    } else if (!strncmp(paramKey.c_str(), "hmaudio", 7)) {
      ++config->num_hmaudio_sub_bins;
      parse_err = !parse_hmaudio_yaml(&config->hmaudio_config[get_trailing_integer(paramKey)], itr->second);
    } else if (paramKey == "message-converter") {
      parse_err = !parse_msgconv_yaml(&config->msg_conv_config, paramKey, config_dir.c_str());
    } else if (paramKey == "tests") {
      parse_err = !parse_tests_yaml(config, itr->second);
    } else if (paramKey.compare(0, dewarper_str.size(), dewarper_str) == 0) {
      size_t start = paramKey.find(dewarper_str);
      int source_id = 0;
      if (start != std::string::npos) {
        std::string index_str =
            paramKey.substr(start + dewarper_str.length(), paramKey.length() - start - dewarper_str.length());
        source_id = std::stoi(index_str);
        parse_dewarper_yaml(&config->multi_source_config[source_id].dewarper_config, itr->second, config_dir);
      } else {
        NVGSTDS_ERR_MSG_V("Dewarper key is wrong ! ");
        parse_err = true;
      }
    }

    if (parse_err) {
      std::cerr << "Failed parsing YAML section: " << paramKey << std::endl;
      goto done;
    }
  }
  /* Updating batch size when source list is enabled */
  /* if (config->source_list_enabled == TRUE) {
      // For streammux and pgie, batch size is set to number of sources
      config->streammux_config.batch_size = config->num_source_sub_bins;
      config->primary_gie_config.batch_size = config->num_source_sub_bins;
      if (config->sgie_batch_size != 0) {
          for (i = 0; i < config->num_secondary_gie_sub_bins; i++) {
              config->secondary_gie_sub_bin_config[i].batch_size = config->sgie_batch_size;
          }
      }
  } */
  unsigned int i, j;
  for (i = 0; i < config->num_secondary_gie_sub_bins; i++) {
    if (config->secondary_gie_sub_bin_config[i].unique_id == config->primary_gie_config.unique_id) {
      NVGSTDS_ERR_MSG_V("Non unique gie ids found");
      std::cerr << "Non unique gie id shared by primary and secondary gie: " << config->primary_gie_config.unique_id
                << std::endl;
      ret = FALSE;
      goto done;
    }
  }

  for (i = 0; i < config->num_secondary_gie_sub_bins; i++) {
    for (j = i + 1; j < config->num_secondary_gie_sub_bins; j++) {
      if (config->secondary_gie_sub_bin_config[i].unique_id == config->secondary_gie_sub_bin_config[j].unique_id) {
        NVGSTDS_ERR_MSG_V("Non unique gie id %d found", config->secondary_gie_sub_bin_config[i].unique_id);
        std::cerr << "Non unique secondary gie id: " << config->secondary_gie_sub_bin_config[i].unique_id << std::endl;
        ret = FALSE;
        goto done;
      }
    }
  }

  for (i = 0; i < config->num_source_sub_bins; i++) {
    if (config->multi_source_config[i].type == NV_DS_SOURCE_URI_MULTIPLE) {
      if (config->multi_source_config[i].num_sources < 1) {
        config->multi_source_config[i].num_sources = 1;
      }
      // HStream uses URI_MULTIPLE as one logical camera playlist. Preserve that type whenever a playlist is explicit,
      // or when this entry represents only one camera. Rewriting it to URI makes production mux selection miss the
      // exact-pair/full-batch contract even though uri-list survives and the decode barrier remains active.
      const gboolean is_uri_playlist =
          (config->multi_source_config[i].uri_list && *config->multi_source_config[i].uri_list) ||
          config->multi_source_config[i].num_sources == 1;
      if (is_uri_playlist) {
        if (!config->multi_source_config[i].uri) {
          g_printerr("No URI configured for source id %d\n", config->multi_source_config[i].source_id);
          std::cerr << "No URI configured for source id " << config->multi_source_config[i].source_id << std::endl;
          goto done;
        }
        continue;
      }
      for (j = 1; j < config->multi_source_config[i].num_sources; j++) {
        if (config->num_source_sub_bins == MAX_SOURCE_BINS) {
          NVGSTDS_ERR_MSG_V("App supports max %d sources", MAX_SOURCE_BINS);
          ret = FALSE;
          goto done;
        }
        memcpy(
            &config->multi_source_config[config->num_source_sub_bins],
            &config->multi_source_config[i],
            sizeof(config->multi_source_config[i]));
        config->multi_source_config[config->num_source_sub_bins].type = NV_DS_SOURCE_URI;
        config->multi_source_config[config->num_source_sub_bins].uri =
            g_strdup_printf(config->multi_source_config[config->num_source_sub_bins].uri, j);
        config->num_source_sub_bins++;
      }
      config->multi_source_config[i].type = NV_DS_SOURCE_URI;
      if (!config->multi_source_config[i].uri) {
        g_printerr("No URI configured for source id %d\n", config->multi_source_config[i].source_id);
        std::cerr << "No URI configured for source id " << config->multi_source_config[i].source_id << std::endl;
        goto done;
      }
      config->multi_source_config[i].uri = g_strdup_printf(config->multi_source_config[i].uri, 0);
    }
  }

  ret = TRUE;
done:
  if (!ret) {
    cout << __func__ << " failed" << endl;
  }
  return ret;
}

// gboolean parse_config_file_yaml(NvDsConfig* config, const gchar* cfg_file_path) {
//   YAML::Node configyml = YAML::LoadFile(cfg_file_path);
//   return parse_config_yaml(configyml, config, cfg_file_path);
// }
