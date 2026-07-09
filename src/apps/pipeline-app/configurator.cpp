#include "configurator.h"

#include <gstreamer-1.0/gst/gstelement.h>
#include <gstreamer-1.0/gst/gstpipeline.h>
#include <unistd.h>
#include <yaml-cpp/node/parse.h>

#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "StitcherOnePassConfig.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "cupano/pano/controlMasks.h"
#include "deepstream_app.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/filesystem.h"
#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/Orientation.h"

namespace fs = std::filesystem;

namespace hm {

namespace {

constexpr long kMaxPlayCropperOutputWidth = 7680;
constexpr long kMaxPlayCropperOutputHeight = 4320;

constexpr const char* kRinkMaskFilename = "rink_mask_0.png";

constexpr const char* kEnableFlagField = "enable";

constexpr const char* kDefaultOutputVideoName = "tracking_output.mkv";
constexpr const char* kLegacyDefaultOutputName = "out.mkv";

int as_int(const YAML::Node& node) {
  // be less asserty than YAML-CPP
  // std::cout << node << std::endl;
  if (!node.IsDefined()) {
    return 0;
  }
  std::string s = node.as<std::string>();
  return std::atoi(s.c_str());
}

bool is_enabled(const YAML::Node& config, const std::string& dot_string) {
  auto opt_node = get_node(config, dot_string);
  if (!opt_node.has_value()) {
    return false;
  }
  YAML::Node& node = *opt_node;
  if (!node.IsDefined()) {
    return false;
  }
  return get_node_value(node, kEnableFlagField, static_cast<int>(false));
}

void remove_whitespace_in_place(std::string& input) {
  int index = 0; // This will keep track of the position in the original string
  for (char c : input) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      input[index++] = c;
    }
  }
  input.resize(index); // Resize the string to the new length, removing the
                       // trailing whitespace
}

bool is_valid_yaml_value_string(std::string s) {
  remove_whitespace_in_place(s);
  if (s.empty()) {
    return false;
  }
  if (s[0] == '#') {
    // Comment
    return false;
  }
  return true;
}

bool is_empty_yaml_document(const YAML::Node& node) {
  return !node.IsDefined() || node.IsNull() || ((node.IsMap() || node.IsSequence()) && node.size() == 0);
}

bool is_enabled(YAML::Node n) {
  if (!n.IsDefined()) {
    return false;
  }
  if (!n.IsMap()) {
    return false;
  }
  YAML::Node enabled = n[kEnableFlagField];
  if (enabled.IsDefined()) {
    std::string as_str = enabled.as<std::string>();
    if (as_str == "true") {
      return true;
    } else if (as_str == "false") {
      return false;
    }
    if (enabled.as<int>()) {
      return true;
    }
  }
  return false;
}

std::optional<YAML::Node> map_child(const YAML::Node& node, const std::string& key) {
  if (!node.IsMap()) {
    return std::nullopt;
  }
  for (const auto& item : node) {
    if (item.first.as<std::string>() == key) {
      return item.second;
    }
  }
  return std::nullopt;
}

bool remove_yaml_key_path(YAML::Node& root, const std::initializer_list<std::string>& path) {
  if (!root.IsMap() || path.size() == 0) {
    return false;
  }
  std::vector<std::string> keys(path.begin(), path.end());
  YAML::Node current = root;
  std::vector<std::pair<YAML::Node, std::string>> path_nodes;
  for (size_t i = 0, n = keys.size(); i + 1 < n; ++i) {
    if (!current.IsMap()) {
      return false;
    }
    std::optional<YAML::Node> next = map_child(current, keys.at(i));
    if (!next.has_value() || !next->IsDefined()) {
      return false;
    }
    path_nodes.emplace_back(current, keys.at(i));
    current = *next;
  }
  if (!current.IsMap()) {
    return false;
  }
  const std::string& leaf_key = keys.back();
  std::optional<YAML::Node> leaf = map_child(current, leaf_key);
  if (!leaf.has_value() || !leaf->IsDefined()) {
    return false;
  }
  current.remove(leaf_key);
  for (auto it = path_nodes.rbegin(); it != path_nodes.rend(); ++it) {
    YAML::Node parent = it->first;
    const std::string& key = it->second;
    YAML::Node child = parent[key];
    if (!child.IsMap() || child.size() != 0) {
      break;
    }
    parent.remove(key);
  }
  return true;
}

bool is_cam_video_key(const std::string& key) {
  static const std::regex cam_pattern(R"(cam[0-9]+)", std::regex::icase);
  return std::regex_match(key, cam_pattern);
}

std::vector<std::string> sequence_path_values(const YAML::Node& root, const std::initializer_list<std::string>& path) {
  YAML::Node current = root;
  for (const std::string& key : path) {
    if (!current.IsMap()) {
      return {};
    }
    std::optional<YAML::Node> next = map_child(current, key);
    if (!next.has_value() || !next->IsDefined()) {
      return {};
    }
    current = *next;
  }
  if (!current.IsSequence()) {
    return {};
  }
  return current.as<std::vector<std::string>>();
}

bool same_sequence_values(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

int explicit_file_chapter(const std::string& file) {
  const std::string filename = fs::path(file).filename().string();
  std::smatch match;
  static const std::regex gopro_pattern(R"(^G[A-Z]([0-9]{2})([0-9]{4})\.(MP4|mp4)$)");
  static const std::regex insta360_pattern(R"(^VID_[0-9]{8}_[0-9]{6}_([0-9]{3})\.(MP4|mp4)$)");
  static const std::regex left_right_pattern(R"((left|right)(?:-([0-9]))?\.mp4$)");
  try {
    if (std::regex_search(filename, match, gopro_pattern)) {
      return std::stoi(match[1].str());
    }
    if (std::regex_search(filename, match, insta360_pattern)) {
      return std::stoi(match[1].str());
    }
    if (std::regex_search(filename, match, left_right_pattern)) {
      return match[2].matched ? std::stoi(match[2].str()) : 1;
    }
  } catch (const std::exception&) {
    return -1;
  }
  return -1;
}

std::string explicit_file_chapter_key(const std::string& file) {
  const std::string filename = fs::path(file).filename().string();
  std::smatch match;
  static const std::regex gopro_pattern(R"(^G[A-Z]([0-9]{2})([0-9]{4})\.(MP4|mp4)$)");
  static const std::regex insta360_chapter_pattern(R"(^VID_[0-9]{8}_[0-9]{6}_([0-9]{3})\.(MP4|mp4)$)");
  static const std::regex left_right_pattern(R"((left|right)(?:-([0-9]))?\.mp4$)");
  if (std::regex_search(filename, match, gopro_pattern)) {
    return "gopro:" + match[1].str();
  }
  if (std::regex_search(filename, match, insta360_chapter_pattern)) {
    return "insta360:" + match[1].str();
  }
  if (std::regex_search(filename, match, left_right_pattern)) {
    return "lr:" + std::string(match[2].matched ? match[2].str() : "1");
  }
  return {};
}

std::map<int, std::string> files_by_explicit_chapter(const std::vector<std::string>& files) {
  std::map<int, std::string> by_chapter;
  for (const std::string& file : files) {
    const int chapter = explicit_file_chapter(file);
    if (chapter <= 0 || by_chapter.count(chapter)) {
      by_chapter.clear();
      return by_chapter;
    }
    by_chapter[chapter] = file;
  }
  return by_chapter;
}

std::map<std::string, std::string> files_by_explicit_chapter_key(const std::vector<std::string>& files) {
  std::map<std::string, std::string> by_chapter;
  for (const std::string& file : files) {
    const std::string chapter = explicit_file_chapter_key(file);
    if (chapter.empty() || by_chapter.count(chapter)) {
      by_chapter.clear();
      return by_chapter;
    }
    by_chapter[chapter] = file;
  }
  return by_chapter;
}

bool explicit_runtime_videos_match_ui_roles(
    const std::vector<std::string>& runtime_left,
    const std::vector<std::string>& runtime_right,
    const std::vector<std::string>& ui_left,
    const std::vector<std::string>& ui_right) {
  if (runtime_left.empty() || runtime_right.empty() || ui_left.empty() || ui_right.empty()) {
    return false;
  }
  const std::map<std::string, std::string> left_by_chapter = files_by_explicit_chapter_key(ui_left);
  const std::map<std::string, std::string> right_by_chapter = files_by_explicit_chapter_key(ui_right);
  if (!left_by_chapter.empty() && !right_by_chapter.empty()) {
    if (left_by_chapter.size() != right_by_chapter.size()) {
      return false;
    }
    std::vector<std::string> sorted_left;
    std::vector<std::string> sorted_right;
    for (const auto& [chapter, left_file] : left_by_chapter) {
      auto right_found = right_by_chapter.find(chapter);
      if (right_found == right_by_chapter.end()) {
        return false;
      }
      sorted_left.emplace_back(left_file);
      sorted_right.emplace_back(right_found->second);
    }
    return same_sequence_values(runtime_left, sorted_left) && same_sequence_values(runtime_right, sorted_right);
  }
  auto is_unparseable = [](const std::string& file) { return explicit_file_chapter_key(file).empty(); };
  if (ui_left.size() == ui_right.size() && std::all_of(ui_left.begin(), ui_left.end(), is_unparseable) &&
      std::all_of(ui_right.begin(), ui_right.end(), is_unparseable)) {
    return same_sequence_values(runtime_left, ui_left) && same_sequence_values(runtime_right, ui_right);
  }
  return false;
}

void remove_rotation_dependent_rink_cache_keys(YAML::Node& config);

void remove_cleanable_stitching_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"game", "stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"stitching", "control_points"});
  remove_yaml_key_path(config, {"game", "stitching", "control_points"});
  remove_rotation_dependent_rink_cache_keys(config);
}

void remove_rotation_dependent_rink_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"rink", "scoreboard", "perspective_polygon"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_count"});
  remove_yaml_key_path(config, {"rink", "ice_contours_mask_centroid"});
  remove_yaml_key_path(config, {"rink", "ice_contours_combined_bbox"});
}

std::optional<double> get_optional_double(const YAML::Node& node, const std::string& key) {
  std::optional<YAML::Node> value = get_node(node, key);
  if (!value.has_value() || !value->IsDefined() || value->IsNull()) {
    return std::nullopt;
  }
  return value->as<double>();
}

double get_rotation_or_default(const YAML::Node& node, const std::string& key, double default_value) {
  std::optional<double> value = get_optional_double(node, key);
  return value.value_or(default_value);
}

absl::Status remove_rink_mask_files(const fs::path& game_dir) {
  if (!fs::exists(game_dir)) {
    return absl::OkStatus();
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(game_dir, ec)) {
    if (ec) {
      return absl::InternalError(TO_STRING("Failed to scan rink mask files in " << game_dir << ": " << ec.message()));
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (!absl::StartsWith(filename, "rink_mask_") || !absl::EndsWith(filename, ".png")) {
      continue;
    }
    fs::remove(entry.path(), ec);
    if (ec) {
      return absl::InternalError(
          TO_STRING("Failed to remove stale rink mask " << entry.path() << ": " << ec.message()));
    }
  }
  return absl::OkStatus();
}

bool is_render_sink_type(int sink_type) {
  const int kRenderSinkType =
#if defined(IS_TEGRA)
      static_cast<int>(NV_DS_SINK_RENDER_3D);
#else
      static_cast<int>(NV_DS_SINK_RENDER_EGL);
#endif
  return sink_type == kRenderSinkType || sink_type == static_cast<int>(NV_DS_SINK_RENDER_DRM);
}

int get_render_sink_type() {
#if defined(IS_TEGRA)
  return static_cast<int>(NV_DS_SINK_RENDER_3D);
#else
  return static_cast<int>(NV_DS_SINK_RENDER_EGL);
#endif
}

std::optional<size_t> get_sink_index(const std::string& key) {
  if (!absl::StartsWith(key, "sink")) {
    return std::nullopt;
  }
  if (key.size() == 4) {
    return std::nullopt;
  }

  std::string suffix = key.substr(4);
  if (suffix.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  try {
    return std::stoull(suffix);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

long round_down_even(long value) {
  if (value <= 2) {
    return 2;
  }
  return (value / 2) * 2;
}

long scaled_render_dimension(long value, double scale) {
  if (value <= 0 || scale <= 0.0) {
    return value;
  }
  long scaled = std::llround(static_cast<double>(value) * scale);
  return round_down_even(std::max<long>(scaled, 2));
}

std::optional<std::pair<long, long>> get_default_render_base_size(const YAML::Node& pipeline) {
  std::vector<std::string> width_height_sources = {
      "hmplaycropper.output-width",
      "hmplaycropper.output-height",
      "streammux.width",
      "streammux.height",
      "hmstitcher.output-width",
      "hmstitcher.output-height",
  };

  auto maybe_stream_width = get_node_value<long>(pipeline, width_height_sources.at(0), -1);
  auto maybe_stream_height = get_node_value<long>(pipeline, width_height_sources.at(1), -1);
  if (maybe_stream_width > 0 && maybe_stream_height > 0) {
    return std::make_pair(maybe_stream_width, maybe_stream_height);
  }

  maybe_stream_width = get_node_value<long>(pipeline, width_height_sources.at(2), -1);
  maybe_stream_height = get_node_value<long>(pipeline, width_height_sources.at(3), -1);
  if (maybe_stream_width > 0 && maybe_stream_height > 0) {
    return std::make_pair(maybe_stream_width, maybe_stream_height);
  }

  maybe_stream_width = get_node_value<long>(pipeline, width_height_sources.at(4), -1);
  maybe_stream_height = get_node_value<long>(pipeline, width_height_sources.at(5), -1);
  if (maybe_stream_width > 0 && maybe_stream_height > 0) {
    return std::make_pair(maybe_stream_width, maybe_stream_height);
  }

  return std::nullopt;
}

absl::Status ensure_render_sink_with_scale(YAML::Node& pipeline, double show_render_scale) {
  constexpr const char* kSinkKeyPrefix = "sink";

  bool has_render_sink = false;
  size_t max_sink_index = 0;
  int max_sink_id = -1;

  const bool show_render_sink = show_render_scale != 0.0;
  const bool has_render_scale = show_render_scale > 0.0;
  std::optional<std::pair<long, long>> base_size;
  if (has_render_scale) {
    base_size = get_default_render_base_size(pipeline);
    if (!base_size.has_value()) {
      std::cerr << "Warning: --show-scaled requested but no base render dimensions found; using defaults\n";
    }
  }

  for (auto kv : pipeline) {
    const std::string key = kv.first.as<std::string>();
    if (!absl::StartsWith(key, kSinkKeyPrefix)) {
      continue;
    }
    std::optional<size_t> index = get_sink_index(key);
    if (index.has_value()) {
      max_sink_index = std::max(max_sink_index, *index + 1);
    }

    YAML::Node sink_node = kv.second;
    int sink_id = get_node_value(sink_node, "sink-id", -1);
    if (sink_id >= 0) {
      max_sink_id = std::max<int>(max_sink_id, sink_id);
    }

    int sink_type = get_node_value(sink_node, "type", 0);
    if (!is_render_sink_type(sink_type)) {
      continue;
    }

    has_render_sink = true;

    if (!show_render_sink) {
      sink_node[kEnableFlagField] = "0";
      continue;
    }

    sink_node[kEnableFlagField] = "1";
    if (has_render_scale && base_size.has_value()) {
      sink_node["width"] = std::to_string(scaled_render_dimension(base_size.value().first, show_render_scale));
      sink_node["height"] = std::to_string(scaled_render_dimension(base_size.value().second, show_render_scale));
    }
  }

  if (!show_render_sink) {
    return absl::OkStatus();
  }

  if (has_render_sink) {
    return absl::OkStatus();
  }

  std::string new_key = kSinkKeyPrefix + std::to_string(max_sink_index);
  YAML::Node render_sink = pipeline[new_key];
  render_sink = YAML::Node(YAML::NodeType::Map);

  render_sink[kEnableFlagField] = "1";
  render_sink["sink-id"] = std::to_string(std::max<int>(max_sink_id, 0) + 1);
  render_sink["type"] = get_render_sink_type();
  render_sink["sync"] = "1";
  if (has_render_scale && base_size.has_value()) {
    render_sink["width"] = std::to_string(scaled_render_dimension(base_size.value().first, show_render_scale));
    render_sink["height"] = std::to_string(scaled_render_dimension(base_size.value().second, show_render_scale));
  }

  return absl::OkStatus();
}

std::string to_lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string add_audio_suffix_to_output_path(const std::string& output_path) {
  if (output_path.empty()) {
    return output_path;
  }
  fs::path p(output_path);
  std::string stem = p.stem().string();
  if (!absl::EndsWith(stem, "-with-audio")) {
    stem += "-with-audio";
    p = p.parent_path() / (stem + p.extension().string());
  }
  return p.string();
}

void set_container_from_output_extension(YAML::Node& sink_node, const fs::path& output_path) {
  const std::string ext = to_lower_ascii(output_path.extension().string());
  if (ext == ".mp4") {
    sink_node["container"] = static_cast<int>(NV_DS_CONTAINER_MP4);
  } else if (ext == ".mkv") {
    sink_node["container"] = static_cast<int>(NV_DS_CONTAINER_MKV);
  }
}

YAML::Node ensure_game_frame_offsets_node(YAML::Node& config) {
  std::optional<YAML::Node> game_offsets = get_node(config, "game.stitching.frame_offsets");
  if (!game_offsets.has_value() || !game_offsets->IsMap()) {
    std::optional<YAML::Node> legacy_offsets = get_node(config, "stitching.frame_offsets");
    config["game"]["stitching"]["frame_offsets"] =
        legacy_offsets.has_value() && legacy_offsets->IsMap() ? *legacy_offsets : YAML::Node(YAML::NodeType::Map);
  }
  return config["game"]["stitching"]["frame_offsets"];
}

void set_all_field_values(
    const YAML::Node& parent,
    const std::string& field_name,
    const std::string& field_value,
    bool only_if_exists) {
  for (auto& item : parent) {
    if (!item.second.IsMap()) {
      continue;
    }
    YAML::Node field_node = item.second[field_name];
    if (only_if_exists && !field_node.IsDefined()) {
      continue;
    }
    field_node = field_value;
  }
}

std::optional<std::tuple<int, int>> get_canvas_size(const std::string& game_dir) {
  // Control masks require `seam_file.png`. If it is missing, create a simple hard-seam fallback so we can still
  // determine canvas sizing and avoid the pipeline booting into a gray passthrough mode.
  (void)stitching::maybe_create_default_seam_file(game_dir);
  hm::pano::ControlMasks control_masks(game_dir);
  if (!control_masks.is_valid()) {
    return std::nullopt;
  }
  return std::make_tuple(control_masks.canvas_width(), control_masks.canvas_height());
}

std::optional<YAML::Node> get_node_if_enabled(const YAML::Node& pipeline, const std::string& name) {
  if (pipeline.IsDefined() && pipeline.IsMap()) {
    YAML::Node n = pipeline[name];
    if (is_enabled(n)) {
      return n;
    }
  }
  return std::nullopt;
}

absl::Status ready_camera(const std::string device_name) {
  auto v4l2_ctl = findExecutable("v4l2-ctl", {"PATH"});
  if (!v4l2_ctl) {
    v4l2_ctl = "/usr/bin/v4l2-ctl";
  }

  std::vector<std::string> cmd{*v4l2_ctl, "--device", device_name, "--all"};

  int exitcode = hm::run_command(cmd, ".", {}, [](const std::string& stderr, const std::string& stdout) -> void {
    if (!stderr.empty()) {
      std::cerr << "Failed to execute 'v4l2-ctl': " << stderr << std::endl;
    }
    if (!stdout.empty()) {
      std::cerr << stdout << std::endl;
    }
  });
  if (exitcode) {
    return absl::InternalError("Failed to contact camera.");
  }
  return absl::OkStatus();
}

[[maybe_unused]] std::map<size_t, YAML::Node> get_enabled_indexed_sections_with_prefix(
    const YAML::Node& parent_node,
    const std::string& section_prefix,
    size_t max_indexes = 10) {
  std::map<size_t, YAML::Node> results;
  for (size_t index = 0; index < max_indexes; ++index) {
    std::string source_key = section_prefix + std::to_string(index);
    YAML::Node section_node = parent_node[source_key];
    if (!section_node.IsDefined()) {
      continue;
    }
    if (section_node[kEnableFlagField].IsDefined() && section_node[kEnableFlagField].as<int>()) {
      results.emplace(index, section_node);
    }
  };
  return results;
}

std::optional<YAML::Node> get_enabled_audio_uri(const YAML::Node& pipeline) {
  size_t index = 0;
  const std::string source_base = "source";
  do {
    std::string source_key = source_base + std::to_string(index);
    if (!pipeline[source_key].IsDefined()) {
      break;
    }
    YAML::Node audio = pipeline[source_key];
    if (audio[kEnableFlagField].IsDefined() && audio[kEnableFlagField].as<int>() &&
        audio["type"].as<int>() == NV_DS_SOURCE_AUDIO_URI) {
      return audio;
    }
    ++index;
  } while (true);
  return std::nullopt;
}

// src-id -> source node
std::map<int, YAML::Node> get_enabled_sources(const YAML::Node& pipeline) {
  std::map<int, YAML::Node> sources;
  for (auto kv : pipeline) {
    std::string key = kv.first.as<std::string>();
    if (absl::StartsWith(key, "source")) {
      YAML::Node src_node = kv.second;
      if (!get_node_value(src_node, kEnableFlagField, 0)) {
        continue;
      }
      if (!has_node(src_node, "source-id", /*non_null=*/true)) {
        std::cerr << "No source-id in enabled source section: " << key << std::endl;
        continue;
      }
      sources.emplace(src_node["source-id"].as<int>(), src_node);
    }
  }
  return sources;
}

absl::StatusOr<std::map<int, YAML::Node>> get_camera_sources(const YAML::Node& pipeline) {
  std::map<int, YAML::Node> cameras;
  std::map<int, YAML::Node> sources = get_enabled_sources(pipeline);
  for (const auto& src_iter_item : sources) {
    NvDsSourceType type = (NvDsSourceType)(src_iter_item.second["type"].as<int>());
    if (type == NvDsSourceType::NV_DS_SOURCE_CAMERA_CSI || type == NvDsSourceType::NV_DS_SOURCE_CAMERA_V4L2) {
      if (type == NvDsSourceType::NV_DS_SOURCE_CAMERA_V4L2) {
        HM_RETURN_IF_ERROR(
            ready_camera(TO_STRING("/dev/video" << src_iter_item.second["camera-v4l2-dev-node"].as<int>())));
      }
      cameras.emplace(src_iter_item.first, src_iter_item.second);
    }
  }
  return cameras;
}

struct SimpleConfig {
  gboolean enable{false};
  char* config_file{nullptr};
  ~SimpleConfig() {
    if (config_file) {
      g_free(config_file);
    }
  }
};

std::optional<YAML::Node> maybe_get_config_file(const YAML::Node& yaml_node, const std::string& config_dir) {
  hm::utils::ConfigLocator locator;
  SimpleConfig config;
  SET_LOCATOR(locator, config, enable); // kEnableFlagField
  SET_LOCATOR_CHAR_PTR(locator, config, config_file);
  hm::utils::set_config_from_yaml(yaml_node, locator, /*quiet=*/true);

  if (config.enable && config.config_file && *config.config_file) {
    fs::path subconfig_file = fs::path(config_dir) / config.config_file;
    YAML::Node subnode = YAML::LoadFile(subconfig_file.string());
    return subnode;
  }
  return std::nullopt;
}

} // namespace

// Forward declaration for helper defined later in this file
void map_key_configs(YAML::Node yaml, const std::vector<std::pair<std::string, std::string>>& map_dest_from_src);

void Configurator::apply_gpu_override(YAML::Node& pipeline) {
  if (override_gpu_id_ != kUseConfigFileGpu) {
    pipeline["application"]["global-gpu-id"] = override_gpu_id_;
    set_all_field_values(pipeline, "gpu-id", std::to_string(override_gpu_id_), /*only_if_exists=*/true);
  }
}

absl::Status Configurator::setup_stitcher_and_masks(
    YAML::Node& pipeline,
    const fs::path& game_dir,
    bool force,
    bool& has_hmstitcher) {
  has_hmstitcher = get_node(pipeline, "hmstitcher")->IsDefined();
  if (has_hmstitcher) {
    if (get_node_value(pipeline, "hmstitcher.enable", FALSE) &&
        get_node_value(pipeline, "hmstitcher.configure-only", FALSE)) {
      bool is_configured;
      HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(game_dir));
      if (is_configured && !force) {
        return absl::CancelledError("Stitching is already configured.");
      }
    }
    pipeline["hmstitcher"]["config-file"] = std::string(game_dir);
  }
  if (pipeline["ds-fieldmask"].IsDefined()) {
    pipeline["ds-fieldmask"]["detection-mask"] = std::string(game_dir / kRinkMaskFilename);
  }
  return absl::OkStatus();
}

void Configurator::map_common_config_keys() {
  map_key_configs(
      config_,
      {
          {"stitching.post_stitch_rotate_degrees", "stitching.stitch_rotate_degrees"},
          {"stitching.post_stitch_rotate_degrees", "stitching.stitch-rotate-degrees"},
          {"stitching.post_stitch_rotate_degrees", "game.stitching.post_stitch_rotate_degrees"},
          {"stitching.post_stitch_rotate_degrees", "game.stitching.stitch_rotate_degrees"},
          {"stitching.post_stitch_rotate_degrees", "game.stitching.stitch-rotate-degrees"},
      });
  const std::vector<std::pair<std::string, std::string>> map_dest_from_src{
      {"pipeline.hmstitcher.post-stitch-rotate-degrees", "stitching.post_stitch_rotate_degrees"},
      {"pipeline.hmplaycropper.fixed-edge-rotation-angle", "rink.camera.fixed_edge_rotation_angle"},
      {"pipeline.ds-playtracker.fixed-edge-rotation-angle", "rink.camera.fixed_edge_rotation_angle"},
  };
  map_key_configs(config_, map_dest_from_src);
}

absl::Status Configurator::invalidate_rotation_dependent_cache_if_needed(const fs::path& game_dir) {
  const double desired_rotation = get_rotation_or_default(
      config_,
      "pipeline.hmstitcher.post-stitch-rotate-degrees",
      get_rotation_or_default(config_, "stitching.post_stitch_rotate_degrees", 0.0));
  constexpr double kRotationEpsilon = 1e-6;
  const bool has_marker = has_node(private_config_, "stitching.generated_field_mask_post_stitch_rotate_degrees", true);
  bool should_invalidate = std::abs(desired_rotation) > kRotationEpsilon;
  if (has_marker) {
    const double generated_rotation =
        get_rotation_or_default(private_config_, "stitching.generated_field_mask_post_stitch_rotate_degrees", 0.0);
    should_invalidate = std::abs(desired_rotation - generated_rotation) > kRotationEpsilon;
  } else if (std::abs(desired_rotation) <= kRotationEpsilon) {
    const std::optional<double> previous_rotation =
        get_optional_double(private_config_, "stitching.post_stitch_rotate_degrees");
    should_invalidate = previous_rotation.has_value() && std::abs(*previous_rotation) > kRotationEpsilon;
  }
  if (!should_invalidate) {
    return absl::OkStatus();
  }

  HM_RETURN_IF_ERROR(remove_rink_mask_files(game_dir));
  remove_rotation_dependent_rink_cache_keys(config_);
  remove_rotation_dependent_rink_cache_keys(private_config_);
  private_config_["stitching"]["generated_field_mask_post_stitch_rotate_degrees"] = desired_rotation;
  auto save_status = save_private_config(private_config_);
  if (!save_status.ok()) {
    std::cerr << "Warning: failed to save post-stitch rotation cache marker: " << save_status << std::endl;
  }
  return absl::OkStatus();
}

absl::Status Configurator::invalidate_canvas_dependent_cache_if_needed(const fs::path& game_dir) {
  bool exceeds_limit = false;
  HM_ASSIGN_OR_RETURN(exceeds_limit, stitching::stitching_artifacts_exceed_live_canvas_limit(game_dir.string()));
  if (!exceeds_limit) {
    return absl::OkStatus();
  }

  std::cout << "Stitching canvas exceeds live-stitch max dimension; clearing canvas-dependent cached rink geometry"
            << std::endl;
  remove_rotation_dependent_rink_cache_keys(config_);
  remove_rotation_dependent_rink_cache_keys(private_config_);
  if (private_config_.IsDefined()) {
    HM_RETURN_IF_ERROR(save_private_config(private_config_));
  }
  return absl::OkStatus();
}

void Configurator::apply_scoreboard_perspective(YAML::Node& pipeline) {
  if (!pipeline["hmplaycropper"].IsDefined()) {
    return;
  }
  const auto set_playcropper_if_not_set = [&](const std::string& dest_key, const std::string& src_key) {
    YAML::Node dest_node = pipeline["hmplaycropper"][dest_key];
    if (dest_node.IsDefined() && !dest_node.IsNull()) {
      return;
    }
    std::optional<YAML::Node> src_node = get_node(config_, src_key);
    if (!src_node || !src_node->IsDefined() || src_node->IsNull()) {
      return;
    }
    pipeline["hmplaycropper"][dest_key] = src_node->as<std::string>();
  };
  set_playcropper_if_not_set("scoreboard-projected-width", "rink.scoreboard.projected_width");
  set_playcropper_if_not_set("scoreboard-projected-height", "rink.scoreboard.projected_height");
  set_playcropper_if_not_set("scoreboard-scale", "rink.scoreboard.scoreboard_scale");
  if (has_node(config_, "rink.scoreboard.perspective_polygon", /*non_null=*/true)) {
    auto points = config_["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>();
    if (!points.empty()) {
      assert(points.size() == 4);
      std::stringstream ss;
      for (size_t i = 0, n = points.size(); i < n; ++i) {
        if (i)
          ss << ',';
        assert(points[i].size() == 2);
        ss << std::to_string(points[i].at(0)) << ',' << points[i].at(1);
      }
      pipeline["hmplaycropper"]["scoreboard-perspective-polygon"] = ss.str();
    }
  }
}

absl::Status Configurator::gather_stitching_videos(
    const fs::path& game_dir,
    bool force,
    std::vector<std::string>& left_files,
    std::vector<std::string>& right_files,
    YAML::Node& offsets) {
  // Prefer explicit config unless forcing
  bool explicit_left = false;
  bool explicit_right = false;
  std::vector<std::string> explicit_left_files;
  std::vector<std::string> explicit_right_files;
  if (!force) {
    if (has_node(config_, "game.videos.left", /*non_null=*/true)) {
      left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
      explicit_left = !left_files.empty();
      explicit_left_files = left_files;
    }
    if (has_node(config_, "game.videos.right", /*non_null=*/true)) {
      right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
      explicit_right = !right_files.empty();
      explicit_right_files = right_files;
    }
  }

  stitching::VideosDict videos;
  HM_ASSIGN_OR_RETURN(videos, stitching::get_available_videos(game_dir));
  const bool has_cam_auto =
      std::any_of(videos.begin(), videos.end(), [](const auto& item) { return is_cam_video_key(item.first); });
  const bool has_left_right_auto = videos.count("left") || videos.count("right");
  const std::vector<std::string> ui_left_files =
      sequence_path_values(config_, {"hmstream_ui", "video_roles", "left"});
  const std::vector<std::string> ui_right_files =
      sequence_path_values(config_, {"hmstream_ui", "video_roles", "right"});
  const bool runtime_videos_owned_by_ui_roles =
      explicit_runtime_videos_match_ui_roles(left_files, right_files, ui_left_files, ui_right_files);
  if (!force && has_cam_auto && !has_left_right_auto && !runtime_videos_owned_by_ui_roles &&
      (explicit_left || explicit_right)) {
    std::cerr << "Ignoring stale generated game.videos left/right because camN Auto video sets are available"
              << std::endl;
    left_files.clear();
    right_files.clear();
    explicit_left_files.clear();
    explicit_right_files.clear();
    explicit_left = false;
    explicit_right = false;
    remove_yaml_key_path(config_, {"game", "videos", "left"});
    remove_yaml_key_path(config_, {"game", "videos", "right"});
    remove_yaml_key_path(config_, {"game", "stitching", "frame_offsets"});
    remove_yaml_key_path(config_, {"stitching", "frame_offsets"});
    bool private_changed = remove_yaml_key_path(private_config_, {"game", "videos", "left"});
    private_changed = remove_yaml_key_path(private_config_, {"game", "videos", "right"}) || private_changed;
    private_changed = remove_yaml_key_path(private_config_, {"game", "stitching", "frame_offsets"}) || private_changed;
    private_changed = remove_yaml_key_path(private_config_, {"stitching", "frame_offsets"}) || private_changed;
    if (private_changed) {
      auto spp_status = save_private_config(private_config_);
      if (!spp_status.ok()) {
        std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
      }
    }
  }

  auto append_chapters = [](const stitching::VideoChapter& chapters, std::vector<std::string>& files) {
    for (const auto& item : chapters) {
      files.emplace_back(item.second);
    }
  };

  auto matching_chapters = [](const std::vector<std::string>& explicit_files, const stitching::VideoChapter& chapters) {
    std::set<int> matches;
    for (const std::string& explicit_file : explicit_files) {
      const fs::path explicit_path(explicit_file);
      for (const auto& [chapter, discovered_file] : chapters) {
        const fs::path discovered_path(discovered_file);
        if (explicit_path == discovered_path || explicit_path.filename() == discovered_path.filename()) {
          matches.insert(chapter);
        }
      }
    }
    return matches;
  };

  auto explicit_files_by_chapter = [&](const std::vector<std::string>& files) {
    return files_by_explicit_chapter(files);
  };

  auto chapter_keys = [](const std::map<int, std::string>& files) {
    std::set<int> keys;
    for (const auto& [chapter, _] : files) {
      keys.insert(chapter);
    }
    return keys;
  };

  auto append_matching_chapters =
      [](const stitching::VideoChapter& chapters, const std::set<int>& wanted, std::vector<std::string>& files) {
        for (int chapter : wanted) {
          auto found = chapters.find(chapter);
          if (found != chapters.end()) {
            files.emplace_back(found->second);
          }
        }
      };

  auto available_chapters = [](const stitching::VideoChapter& chapters, const std::set<int>& wanted) {
    std::set<int> available;
    for (int chapter : wanted) {
      if (chapters.count(chapter)) {
        available.insert(chapter);
      }
    }
    return available;
  };

  auto format_chapters = [](const std::set<int>& chapters) {
    std::stringstream ss;
    bool first = true;
    for (int chapter : chapters) {
      if (!first) {
        ss << ",";
      }
      first = false;
      ss << chapter;
    }
    return ss.str();
  };

  auto explicit_files_for_chapters =
      [](const std::vector<std::string>& files, const stitching::VideoChapter& chapters, const std::set<int>& wanted) {
        std::vector<std::string> filtered;
        for (int chapter : wanted) {
          auto discovered = chapters.find(chapter);
          if (discovered == chapters.end()) {
            continue;
          }
          const fs::path discovered_path(discovered->second);
          for (const std::string& file : files) {
            const fs::path file_path(file);
            if (file_path == discovered_path || file_path.filename() == discovered_path.filename()) {
              filtered.emplace_back(file);
              break;
            }
          }
        }
        return filtered;
      };

  auto explicit_files_for_parsed_chapters = [](const std::map<int, std::string>& files_by_chapter,
                                               const std::set<int>& wanted) {
    std::vector<std::string> filtered;
    for (int chapter : wanted) {
      auto found = files_by_chapter.find(chapter);
      if (found != files_by_chapter.end()) {
        filtered.emplace_back(found->second);
      }
    }
    return filtered;
  };

  if ((explicit_left || explicit_right) && left_files.empty() && videos.count("left")) {
    const std::map<int, std::string> explicit_right_by_chapter =
        explicit_right ? explicit_files_by_chapter(explicit_right_files) : std::map<int, std::string>();
    std::set<int> wanted = explicit_right && videos.count("right")
        ? matching_chapters(explicit_right_files, videos.at("right"))
        : std::set<int>();
    if (wanted.empty() && !explicit_right_by_chapter.empty()) {
      wanted = chapter_keys(explicit_right_by_chapter);
    }
    if (!wanted.empty()) {
      const std::set<int> paired = available_chapters(videos.at("left"), wanted);
      if (paired != wanted) {
        return absl::InvalidArgumentError(TO_STRING(
            "Auto left videos are missing chapter(s) for explicit right selection: wanted "
            << format_chapters(wanted) << ", found " << format_chapters(paired)));
      }
      append_matching_chapters(videos.at("left"), paired, left_files);
      if (!explicit_right_by_chapter.empty()) {
        right_files = explicit_files_for_parsed_chapters(explicit_right_by_chapter, paired);
      } else if (videos.count("right")) {
        right_files = explicit_files_for_chapters(explicit_right_files, videos.at("right"), paired);
      }
    } else if (!explicit_right || videos.at("left").size() == 1) {
      append_chapters(videos.at("left"), left_files);
    }
  }
  if ((explicit_left || explicit_right) && right_files.empty() && videos.count("right")) {
    const std::map<int, std::string> explicit_left_by_chapter =
        explicit_left ? explicit_files_by_chapter(explicit_left_files) : std::map<int, std::string>();
    std::set<int> wanted = explicit_left && videos.count("left")
        ? matching_chapters(explicit_left_files, videos.at("left"))
        : std::set<int>();
    if (wanted.empty() && !explicit_left_by_chapter.empty()) {
      wanted = chapter_keys(explicit_left_by_chapter);
    }
    if (!wanted.empty()) {
      const std::set<int> paired = available_chapters(videos.at("right"), wanted);
      if (paired != wanted) {
        return absl::InvalidArgumentError(TO_STRING(
            "Auto right videos are missing chapter(s) for explicit left selection: wanted "
            << format_chapters(wanted) << ", found " << format_chapters(paired)));
      }
      append_matching_chapters(videos.at("right"), paired, right_files);
      if (!explicit_left_by_chapter.empty()) {
        left_files = explicit_files_for_parsed_chapters(explicit_left_by_chapter, paired);
      } else if (videos.count("left")) {
        left_files = explicit_files_for_chapters(explicit_left_files, videos.at("left"), paired);
      }
    } else if (!explicit_left || videos.at("right").size() == 1) {
      append_chapters(videos.at("right"), right_files);
    }
  }

  if ((explicit_left || explicit_right) && (left_files.empty() || right_files.empty())) {
    if (has_cam_auto && !videos.count("left") && !videos.count("right")) {
      return absl::InvalidArgumentError(
          "Mixed explicit/Auto video selection cannot infer Left/Right sides from camN Auto video sets. Select both Left and Right explicitly, or use Auto for all video sets.");
    }
    return absl::InvalidArgumentError(
        "Mixed explicit/Auto video selection could not resolve both sides. Select both Left and Right explicitly, or use Auto for all video sets.");
  }

  if ((explicit_left || explicit_right) && !left_files.empty() && !right_files.empty()) {
    if (left_files.size() != right_files.size()) {
      return absl::InvalidArgumentError(TO_STRING(
          "Mismatched explicit/auto video chapters: " << left_files.size() << " left file(s), " << right_files.size()
                                                      << " right file(s)"));
    }
    private_config_["game"]["videos"]["left"] = left_files;
    private_config_["game"]["videos"]["right"] = right_files;
    auto spp_status = save_private_config(private_config_);
    if (!spp_status.ok()) {
      std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
    }
  }

  if (left_files.empty() && right_files.empty() && !videos.count("left") && !videos.count("right")) {
    HM_RETURN_IF_ERROR(stitching::configure_orientation(game_dir));
    overlay_config("", get_private_config_file_name(game_id_));
    private_config_["game"]["videos"]["left"] = config_["game"]["videos"]["left"];
    private_config_["game"]["videos"]["right"] = config_["game"]["videos"]["right"];
    left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
    right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
  }

  if (left_files.empty() && right_files.empty() && videos.count("left") && videos.count("right")) {
    const stitching::VideoChapter& left_chapter = videos.at("left");
    const stitching::VideoChapter& right_chapter = videos.at("right");
    if (!left_chapter.empty()) {
      for (const auto& item : left_chapter) {
        if (!right_chapter.empty()) {
          const int chapter = item.first;
          if (!right_chapter.count(chapter)) {
            std::cerr << "Right vids are missing chapter " << chapter << ", skipping..." << std::endl;
            continue;
          }
        }
        left_files.emplace_back(item.second);
      }
    }
    if (!right_chapter.empty()) {
      for (const auto& item : right_chapter) {
        if (!left_chapter.empty()) {
          const int chapter = item.first;
          if (!left_chapter.count(chapter)) {
            std::cerr << "Left vids are missing chapter " << chapter << ", skipping..." << std::endl;
            continue;
          }
        }
        right_files.emplace_back(item.second);
      }
    }
    if (!left_files.empty() && !right_files.empty()) {
      private_config_["game"]["videos"]["left"] = left_files;
      private_config_["game"]["videos"]["right"] = right_files;
      auto spp_status = save_private_config(private_config_);
      if (!spp_status.ok()) {
        std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
      }
    }
  }

  if (!left_files.empty() && !right_files.empty()) {
    if (!has_node(config_, "game.stitching.frame_offsets.left", /*non_null=*/true) ||
        !has_node(config_, "game.stitching.frame_offsets.right", /*non_null=*/true) || force) {
      stitching::Synchronization sync;
      HM_ASSIGN_OR_RETURN(
          sync, stitching::calculate_stitching_synchronization(game_dir / left_files[0], game_dir / right_files[0]));
      offsets["left"] = std::to_string(sync.video1_frame_offset);
      offsets["right"] = std::to_string(sync.video2_frame_offset);
      private_config_["game"]["stitching"]["frame_offsets"]["left"] = std::to_string(sync.video1_frame_offset);
      private_config_["game"]["stitching"]["frame_offsets"]["right"] = std::to_string(sync.video2_frame_offset);
      auto spp_status = save_private_config(private_config_);
      if (!spp_status.ok()) {
        std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
      }
    }
  }
  return absl::OkStatus();
}

void Configurator::apply_frame_offsets_and_sizes(
    const std::vector<std::string>& left_files,
    const std::vector<std::string>& right_files,
    const YAML::Node& offsets,
    size_t& ww,
    size_t& hh,
    size_t& area,
    YAML::Node& pipeline) {
  if (!left_files.empty()) {
    Videoinfo left_info = getVideoInfo(file_maybe_in_game_dir(left_files[0]));
    double lfo = get_node_value<double>(offsets, "left", 0.0); // decimal frames
    set_stream_offsets_ |= lfo != 0.0;
    pipeline["hmstitcher"]["left-frame-offset-ns"] = std::to_string(size_t(lfo / left_info.fps * GST_SECOND));
    area = left_info.width * left_info.height;
    ww = left_info.width;
    hh = left_info.height;
  }
  if (!right_files.empty()) {
    Videoinfo right_info = getVideoInfo(file_maybe_in_game_dir(right_files[0]));
    double rfo = get_node_value<double>(offsets, "right", 0.0); // decimal frames
    set_stream_offsets_ |= rfo != 0.0;
    pipeline["hmstitcher"]["right-frame-offset-ns"] = std::to_string(size_t(rfo / right_info.fps * GST_SECOND));
    if (right_info.width * right_info.height > (long)area) {
      ww = right_info.width;
      hh = right_info.height;
    }
  }
}

std::tuple<long, long> Configurator::cap_playcropper_output(long width, long height) const {
  return resize_to_fit(width, height, kMaxPlayCropperOutputWidth, kMaxPlayCropperOutputHeight);
}

absl::Status Configurator::set_output_dimensions(
    YAML::Node& pipeline,
    bool is_camera_source,
    const std::map<int, YAML::Node>& camera_sources,
    const std::vector<std::string>& left_files,
    const std::vector<std::string>& right_files,
    bool has_hmstitcher,
    const fs::path& game_dir,
    size_t& ww,
    size_t& hh,
    size_t& area,
    size_t& num_video_sources) {
  auto cap_output = [this](long width, long height) { return this->cap_playcropper_output(width, height); };
  auto round_down_even = [](long value) -> long {
    if (value <= 2) {
      return 2;
    }
    return (value / 2) * 2;
  };

  if (is_camera_source) {
    size_t cam_count = 0;
    for (const auto& cam_src_item : camera_sources) {
      if (!cam_count) {
        ww = cam_src_item.second["camera-width"].as<int>();
        hh = cam_src_item.second["camera-height"].as<int>();
        area = ww * hh;
      } else {
        size_t w2 = cam_src_item.second["camera-width"].as<int>();
        size_t h2 = cam_src_item.second["camera-height"].as<int>();
        if (w2 != ww || h2 != hh) {
          return absl::InvalidArgumentError("Camera widths and heights differ");
        }
      }
      ++num_video_sources;
    }
    auto wh_tuple = cap_output(ww, hh);
    pipeline["hmplaycropper"]["output-width"] = std::to_string(round_down_even(std::get<0>(wh_tuple)));
    pipeline["hmplaycropper"]["output-height"] = std::to_string(round_down_even(std::get<1>(wh_tuple)));
  } else if (!left_files.empty() && !right_files.empty() && has_hmstitcher) {
    StitcherSizingConfig sizing_cfg = ParseStitcherSizingConfig(pipeline);
    std::optional<std::tuple<int, int>> canvas_size_result;
    auto stitching_configured = stitching::is_stitching_configured(game_dir.string());
    if (!stitching_configured.ok()) {
      return stitching_configured.status();
    }
    if (stitching_configured.value()) {
      canvas_size_result = get_canvas_size(game_dir);
    }
    if (canvas_size_result) {
      size_t canvas_width = std::get<0>(*canvas_size_result);
      size_t canvas_height = std::get<1>(*canvas_size_result);
      pipeline["hmstitcher"]["output-width"] = std::to_string(canvas_width);
      pipeline["hmstitcher"]["output-height"] = std::to_string(canvas_height);
      constexpr double ar = 16.0 / 9.0;
      const auto even_canvas_height = static_cast<long>(round_down_even(static_cast<long>(canvas_height)));
      auto wh_tuple = cap_output(static_cast<long>(ar * even_canvas_height), even_canvas_height);
      pipeline["hmplaycropper"]["output-width"] = std::to_string(round_down_even(std::get<0>(wh_tuple)));
      pipeline["hmplaycropper"]["output-height"] = std::to_string(round_down_even(std::get<1>(wh_tuple)));
    } else {
      if (!sizing_cfg.allow_runtime_canvas()) {
        return absl::FailedPreconditionError(
            "Unable to determine the canvas size and runtime stitching configuration is disabled");
      }
      if (sizing_cfg.one_pass_mode) {
        std::cout << "hmstitcher one-pass mode enabled: deferring stitched canvas sizing to runtime" << std::endl;
        pipeline["hmplaycropper"]["runtime-output-max-width"] = std::to_string(kMaxPlayCropperOutputWidth);
        pipeline["hmplaycropper"]["runtime-output-max-height"] = std::to_string(kMaxPlayCropperOutputHeight);
      } else {
        std::cout << "The stitched canvas size is not yet known, will determine in the ensuing pipeline run"
                  << std::endl;
      }
    }
  } else {
    // If we don't have left/right files, we may still know dimensions from a stitched output
    if (ww == 0 || hh == 0) {
      stitching::VideosDict videos;
      HM_ASSIGN_OR_RETURN(videos, stitching::get_available_videos(game_dir));
      if (videos.count("stitched")) {
        const auto& stitched_chapters = videos.at("stitched");
        bool all_exist = true;
        for (const auto& stitched_item : stitched_chapters) {
          if (!fs::exists(stitched_item.second)) {
            all_exist = false;
            break;
          }
        }
        if (all_exist && !stitched_chapters.empty()) {
          auto stitched_file = stitched_chapters.begin()->second;
          Videoinfo stitched_info = getVideoInfo(file_maybe_in_game_dir(stitched_file));
          ww = stitched_info.width;
          hh = stitched_info.height;
          area = ww * hh;
          num_video_sources = 1;
        }
      }
    }
    auto wh_tuple = cap_output(ww, hh);
    pipeline["hmplaycropper"]["output-width"] = std::to_string(round_down_even(std::get<0>(wh_tuple)));
    pipeline["hmplaycropper"]["output-height"] = std::to_string(round_down_even(std::get<1>(wh_tuple)));
  }

  if (area) {
    pipeline["streammux"]["width"] = std::to_string(ww);
    pipeline["streammux"]["height"] = std::to_string(hh);
  }
  return absl::OkStatus();
}

void Configurator::configure_audio(
    YAML::Node& pipeline,
    const std::vector<std::string>& left_files,
    const std::vector<std::string>& right_files,
    const YAML::Node& offsets,
    size_t& num_video_sources) {
  std::string possible_audio_uri;
  size_t audio_source_id = std::numeric_limits<size_t>::max();
  static const std::string ff = "file://";

  if (!left_files.empty() && !right_files.empty()) {
    auto src0 = pipeline["source0"];
    auto src1 = pipeline["source1"];
    if (src0.IsDefined() && as_int(src0[kEnableFlagField]) && as_int(src0["type"]) == NV_DS_SOURCE_URI_MULTIPLE &&
        src1.IsDefined() && as_int(src1[kEnableFlagField]) && as_int(src1["type"]) == NV_DS_SOURCE_URI_MULTIPLE) {
      src0["uri"] = ff + file_maybe_in_game_dir(left_files[0]);
      src1["uri"] = ff + file_maybe_in_game_dir(right_files[0]);
      if (left_files.size() > 1) {
        std::vector<std::string> uri_list;
        uri_list.reserve(left_files.size());
        for (const auto& f : left_files) {
          uri_list.emplace_back(ff + file_maybe_in_game_dir(f));
        }
        src0["uri-list"] = uri_list;
      }
      if (right_files.size() > 1) {
        std::vector<std::string> uri_list;
        uri_list.reserve(right_files.size());
        for (const auto& f : right_files) {
          uri_list.emplace_back(ff + file_maybe_in_game_dir(f));
        }
        src1["uri-list"] = uri_list;
      }
      if (get_node_value<double>(offsets, "left", 0.0) == 0) {
        possible_audio_uri = src0["uri"].as<std::string>();
        audio_source_id = src0["source-id"].as<int>();
      } else {
        assert(get_node_value<double>(offsets, "right", 0.0) == 0);
        possible_audio_uri = src1["uri"].as<std::string>();
        audio_source_id = src1["source-id"].as<int>();
      }
      num_video_sources += 2;
    } else if (src0.IsDefined() && get_node_value<int>(src0, kEnableFlagField, false)) {
      possible_audio_uri = get_node_value<std::string>(src0, "uri", "");
      audio_source_id = src0["source-id"].as<int>();
    } else {
      assert(false);
    }
  } else {
    auto src0 = pipeline["source0"];
    if (is_enabled(config_, "pipeline.source0") && as_int(src0["type"]) == NvDsSourceType::NV_DS_SOURCE_URI_MULTIPLE) {
      if (!src0["uri"].IsDefined() || src0["uri"].IsNull() || src0["uri"].as<std::string>().empty()) {
        std::string stiched_output = file_maybe_in_game_dir("stitched_output-with-audio.mp4");
        if (std::filesystem::exists(stiched_output)) {
          src0["uri"] = ff + stiched_output;
          disable_source_types({NvDsSourceType::NV_DS_SOURCE_URI, NvDsSourceType::NV_DS_SOURCE_URI_MULTIPLE});
          src0[kEnableFlagField] = "1";
        }
      }
      if (src0["uri"].IsDefined() && !src0["uri"].IsNull()) {
        possible_audio_uri = src0["uri"].as<std::string>();
      }
      if (src0["source-id"].IsDefined() && !src0["source-id"].IsNull()) {
        audio_source_id = src0["source-id"].as<int>();
      }
      ++num_video_sources;
    }
  }

  if (num_video_sources < 2 && get_node(pipeline, "hmstitcher")->IsDefined()) {
    pipeline["hmstitcher"][kEnableFlagField] = "0";
  }

  if (!possible_audio_uri.empty() || audio_source_id != std::numeric_limits<size_t>::max()) {
    std::optional<YAML::Node> audio_uri_opt = get_enabled_audio_uri(pipeline);
    if (audio_uri_opt.has_value()) {
      YAML::Node audio_uri = *audio_uri_opt;
      if (!audio_uri["uri"].IsDefined() || !is_valid_yaml_value_string(audio_uri["uri"].as<std::string>())) {
        audio_uri["uri"] = possible_audio_uri;
      }
    }
    for (size_t hmaudio_index = 0; hmaudio_index < INT_MAX; ++hmaudio_index) {
      std::string hmaudio_name = "hmaudio" + std::to_string(hmaudio_index);
      if (!pipeline[hmaudio_name].IsDefined())
        break;
      audio_uri_opt = get_node_if_enabled(pipeline, hmaudio_name);
      if (!audio_uri_opt)
        continue;
      if (audio_source_id != std::numeric_limits<size_t>::max() &&
          (*audio_uri_opt)["src"].as<int>() == SRC_SOURCE_BIN) {
        if (!(*audio_uri_opt)["source-id"].IsDefined() ||
            !is_valid_yaml_value_string((*audio_uri_opt)["source-id"].as<std::string>())) {
          (*audio_uri_opt)["source-id"] = audio_source_id;
        }
      } else {
        YAML::Node audio_uri = *audio_uri_opt;
        const std::string key = "audio-location";
        const bool is_defined = audio_uri[key].IsDefined();
        if (!is_defined || !is_valid_yaml_value_string(audio_uri[key].as<std::string>())) {
          audio_uri[key] = possible_audio_uri;
        }
      }
    }
  }
}

absl::Status Configurator::configure_encode_file_outputs(YAML::Node& pipeline) const {
  if (game_id_.empty()) {
    return absl::OkStatus();
  }

  const auto has_audio_for_sink = [&](int sink_id) {
    for (auto kv : pipeline) {
      const std::string key = kv.first.as<std::string>();
      if (!absl::StartsWith(key, "hmaudio")) {
        continue;
      }
      YAML::Node audio_node = kv.second;
      if (!is_enabled(audio_node)) {
        continue;
      }
      const int dest = get_node_value<int>(audio_node, "dest", static_cast<int>(DEST_INDEPENDENT));
      if (dest != static_cast<int>(DEST_SINK) && dest != static_cast<int>(DEST_MULTI_SINK)) {
        continue;
      }
      const int audio_sink_id = get_node_value<int>(audio_node, "sink-id", -1);
      if (audio_sink_id == sink_id) {
        return true;
      }
      if (dest == static_cast<int>(DEST_MULTI_SINK) && audio_node["multi-sink-ids"].IsDefined()) {
        const std::string multi_sink_ids = get_node_value<std::string>(audio_node, "multi-sink-ids", "");
        for (const absl::string_view token : absl::StrSplit(multi_sink_ids, ',', absl::SkipWhitespace())) {
          if (!token.empty() && std::stoi(std::string(token)) == sink_id) {
            return true;
          }
        }
        continue;
      }
      if (audio_sink_id == -1) {
        return true;
      }
    }
    return false;
  };

  const fs::path output_work_dir = fs::path(".") / "output_workdirs" / game_id_;

  for (auto kv : pipeline) {
    const std::string key = kv.first.as<std::string>();
    if (!absl::StartsWith(key, "sink")) {
      continue;
    }
    YAML::Node sink_node = kv.second;
    if (!is_enabled(sink_node)) {
      continue;
    }
    if (static_cast<NvDsSinkType>(get_node_value<int>(sink_node, "type", 0)) != NV_DS_SINK_ENCODE_FILE) {
      continue;
    }

    const int sink_id = get_node_value<int>(sink_node, "sink-id", -1);
    std::string output_file = get_node_value<std::string>(sink_node, "output-file", "");
    if (!is_valid_yaml_value_string(output_file) || output_file == kLegacyDefaultOutputName) {
      output_file = kDefaultOutputVideoName;
    }

    fs::path output_path(output_file);
    const bool output_was_rebased = !output_path.has_parent_path();
    if (output_was_rebased) {
      output_path = output_work_dir / output_path;
    }
    if (output_was_rebased && has_audio_for_sink(sink_id)) {
      output_path = add_audio_suffix_to_output_path(output_path.string());
    }

    set_container_from_output_extension(sink_node, output_path);
    sink_node["output-file"] = output_path.string();

    const fs::path parent = output_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec) {
        return absl::InternalError(
            TO_STRING("Failed to create output directory \"" << parent.string() << "\": " << ec.message()));
      }
    }
  }

  return absl::OkStatus();
}

void Configurator::log_enabled_bins(const YAML::Node& pipeline) const {
  std::set<std::string> all_enabled;
  for (const auto& item : pipeline) {
    std::string key = item.first.as<std::string>();
    if (is_enabled(pipeline[key])) {
      all_enabled.emplace(key);
    }
  }
  if (!all_enabled.empty()) {
    std::stringstream ss;
    ss << "Enabled bins: \n";
    for (const std::string& s : all_enabled) {
      ss << "  " << s << '\n';
    }
    std::cout << ss.str() << std::flush;
  }
}

Configurator::Configurator(const std::string& game_id, const std::string& config_root_dir, int override_gpu_id)
    : game_id_(game_id), config_root_dir_(config_root_dir), override_gpu_id_(override_gpu_id) {
  // Constructor
}
Configurator::~Configurator() {
  // Destructor
}

std::vector<size_t> Configurator::enable_source_types(
    const std::set<NvDsSourceType>& source_enums,
    bool disable_others) {
  std::vector<size_t> source_ids;
  YAML::Node pipeline = config_["pipeline"];
  if (!pipeline.IsDefined()) {
    return source_ids;
  }
  for (auto kv : pipeline) {
    std::string key = kv.first.as<std::string>();
    if (absl::StartsWith(key, "source")) {
      YAML::Node src_node = kv.second;
      const NvDsSourceType type = static_cast<NvDsSourceType>(get_node_value(src_node, "type", 0));
      if (!type) {
        std::cerr << "Source entry has no type" << std::endl;
        continue;
      }
      if (source_enums.count(type)) {
        src_node[kEnableFlagField] = "1";
        if (!has_node(src_node, "source-id", /*non_null=*/true)) {
          std::cerr << "No source-id in enabled source section: " << key << std::endl;
          source_ids.emplace_back(std::numeric_limits<size_t>::max());
        } else {
          source_ids.emplace_back(src_node["source-id"].as<int>());
        }
      } else if (disable_others) {
        src_node[kEnableFlagField] = "0";
      }
    }
  }
  return source_ids;
}

size_t Configurator::disable_source_types(const std::set<NvDsSourceType>& source_enums) {
  size_t count = 0;
  YAML::Node pipeline = config_["pipeline"];
  if (!pipeline.IsDefined()) {
    return count;
  }
  for (auto kv : pipeline) {
    std::string key = kv.first.as<std::string>();
    if (absl::StartsWith(key, "source")) {
      YAML::Node src_node = kv.second;
      const NvDsSourceType type = static_cast<NvDsSourceType>(get_node_value(src_node, "type", 0));
      if (!type) {
        std::cerr << "Source entry has no type" << std::endl;
        continue;
      }
      if (source_enums.count(type)) {
        src_node[kEnableFlagField] = "0";
      }
    }
  }
  return count;
}

std::string Configurator::file_maybe_in_game_dir(const std::string& basename) {
  if (basename.empty()) {
    return basename;
  }
  const fs::path p(basename);
  if (p.is_absolute()) {
    return p.string();
  }
  return (get_game_dir(game_id_) / p).string();
}

std::filesystem::path Configurator::get_game_dir(const std::string& game_id) {
  if (game_id.empty()) {
    return {};
  }
  const char* sprefix = ::getenv("HM_GAME_DIR");
  if (sprefix && *sprefix) {
    return fs::path(sprefix) / game_id;
  }
  const char* homedir = ::getenv("HOME");
  if (homedir && *homedir) {
    return fs::path(homedir) / "Videos" / game_id;
  }
  return fs::path("/games") / game_id;
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

absl::Status Configurator::save_private_config(const YAML::Node& private_config) {
  std::string private_config_file = get_private_config_file_name(game_id_);
  std::ofstream fout(private_config_file, std::ios::out | std::ios::trunc);
  if (!fout.is_open()) {
    return absl::InternalError(TO_STRING(
        "Failed to open private config file for writing: \"" << private_config_file
                                                             << "\", reason: " << strerror(errno)));
  }
  if (!is_empty_yaml_document(private_config)) {
    fout << private_config << "\n";
  }
  return absl::OkStatus();
}

absl::StatusOr<YAML::Node> Configurator::load_config() {
  YAML::Node config;
  if (!config_root_dir_.empty()) {
    std::filesystem::path baseline_path = std::filesystem::path(config_root_dir_) / "baseline.yaml";
    if (std::filesystem::exists(baseline_path)) {
      config = YAML::LoadFile(baseline_path);
    }
  }
  std::optional<YAML::Node> private_config = load_private_config();
  if (private_config.has_value()) {
    private_config_ = *private_config;
    config = merge_nodes(
        config,
        private_config_,
        /*warn_if_key_not_in_dest=*/!config);
  } else {
    private_config_ = YAML::Node();
  }
  return config;
}

bool set_if_not_set(YAML::Node node, const std::string& dest_key, const std::string& src_key) {
  std::optional<YAML::Node> dest_node = get_node(node, dest_key);
  if (dest_node && dest_node->IsDefined() && !dest_node->IsNull()) {
    return false;
  }
  std::optional<YAML::Node> src_node = get_node(node, src_key);
  if (!src_node || !src_node->IsDefined() || src_node->IsNull()) {
    return false;
  }
  if (dest_key.find(':') != std::string::npos) {
    usleep(0);
  } else {
    set_node_value(node, dest_key, src_node->as<std::string>());
  }
  return true;
}

bool Configurator::underlay_config(const std::string& node_name, const std::string& filename) {
  if (!std::filesystem::exists(filename)) {
    return false;
  }
  YAML::Node underlaid_config = YAML::LoadFile(filename);
  if (node_name.empty()) {
    config_ = merge_nodes(
        underlaid_config,
        config_,
        /*warn_if_key_not_in_dest=*/false);
  } else {
    // std::cout << config_ << std::endl;
    config_[node_name] = merge_nodes(
        underlaid_config,
        config_[node_name],
        /*warn_if_key_not_in_dest=*/false);
    // std::cout << config_ << std::endl;
  }
  return true;
}

bool Configurator::overlay_config(const std::string& node_name, const std::string& filename) {
  if (!std::filesystem::exists(filename)) {
    return false;
  }
  // std::cout << config_ << std::endl;
  YAML::Node overlaid_config = YAML::LoadFile(filename);
  if (node_name.empty()) {
    config_ = merge_nodes(
        config_,
        overlaid_config,
        /*warn_if_key_not_in_dest=*/false);
  } else {
    config_[node_name] = merge_nodes(
        config_[node_name],
        overlaid_config,
        /*warn_if_key_not_in_dest=*/false);
  }
  // std::cout << config_ << std::endl;
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

absl::Status Configurator::configure() {
  YAML::Node config;
  HM_ASSIGN_OR_RETURN(config, Configurator::load_config());
  config_ = auto_config(std::move(config));
  return absl::OkStatus();
}

YAML::Node Configurator::auto_config(YAML::Node&& config) {
  return std::move(config);
}

absl::StatusOr<bool> Configurator::does_need_stitching(const std::string& game_dir) const {
  stitching::VideosDict videos;
  HM_ASSIGN_OR_RETURN(videos, stitching::get_available_videos(game_dir));
  if (videos.empty()) {
    return false;
  }
  if (!videos.count("stitched")) {
    return true;
  }
  return false;
}

void map_key_configs(YAML::Node yaml, const std::vector<std::pair<std::string, std::string>>& map_dest_from_src) {
  for (const auto& dest_from_src : map_dest_from_src) {
    const std::string& dest_key = dest_from_src.first;
    const std::string& src_key = dest_from_src.second;
    set_if_not_set(yaml, dest_key, src_key);
  }
}

absl::Status Configurator::complete_configuration(
    bool force,
    bool clean_stitching_artifacts,
    bool show_render_sink,
    double show_render_scale) {
  if (!get_node_value(config_, "pipeline.application.complete-configuration", false)) {
    return absl::OkStatus();
  }

  YAML::Node pipeline = config_["pipeline"];
  assert(pipeline.IsDefined());

  apply_gpu_override(pipeline);

  if (game_id_.empty()) {
    // return absl::InvalidArgumentError("No game id specified");
    // Just go by what's in the config file(s)
    if (clean_stitching_artifacts) {
      return absl::InvalidArgumentError("No game id specified for cleaning");
    }
    return absl::OkStatus();
  }

  std::map<int, YAML::Node> camera_sources;
  HM_ASSIGN_OR_RETURN(camera_sources, get_camera_sources(pipeline));
  const bool is_camera_source = !camera_sources.empty();

  // Stitching config mask config dir
  fs::path game_dir = get_game_dir(game_id_);
  const bool has_hmstitcher = has_node(pipeline, "hmstitcher", false);
  if (clean_stitching_artifacts && !has_hmstitcher) {
    return absl::FailedPreconditionError("No hmstitcher section is configured; nothing to clean");
  }
  if (has_hmstitcher && (force || clean_stitching_artifacts)) {
    YAML::Node preserved_pipeline = config_["pipeline"];
    absl::Status clean_status = stitching::clean_stitching_artifacts(game_dir.string());
    if (!clean_status.ok()) {
      if (clean_stitching_artifacts) {
        return clean_status;
      }
      std::cerr << "Warning: failed to clean stitching artifacts: " << clean_status << std::endl;
    } else {
      remove_cleanable_stitching_cache_keys(config_);
      remove_cleanable_stitching_cache_keys(private_config_);
      if (preserved_pipeline.IsDefined()) {
        config_["pipeline"] = preserved_pipeline;
      }
      if (private_config_.IsDefined()) {
        auto save_status = save_private_config(private_config_);
        if (!save_status.ok()) {
          if (clean_stitching_artifacts) {
            return save_status;
          }
          std::cerr << "Warning: failed to save private config: " << save_status << std::endl;
        }
      }
    }
  }

  if (has_hmstitcher) {
    pipeline["hmstitcher"]["force-scoreboard-config"] = force ? "1" : "0";
  }
  if (pipeline["hmplaycropper"].IsDefined()) {
    pipeline["hmplaycropper"]["config-file"] = std::string(game_dir);
  }

  if (clean_stitching_artifacts) {
    return absl::CancelledError("Stitching artifacts cleaned");
  }

  bool pipeline_has_hmstitcher = false;
  HM_RETURN_IF_ERROR(setup_stitcher_and_masks(pipeline, game_dir, force, pipeline_has_hmstitcher));

  map_common_config_keys();
  if (pipeline_has_hmstitcher) {
    HM_RETURN_IF_ERROR(invalidate_rotation_dependent_cache_if_needed(game_dir));
    HM_RETURN_IF_ERROR(invalidate_canvas_dependent_cache_if_needed(game_dir));
  }

  // Live box mappings
  const std::map<std::string, std::string> live_box_map_dest_from_src{
      {"pipeline.ds-playtracker.play-tracker.live-boxes:1", "rink.camera.follower_box_scale_width"},
      {"pipeline.ds-playtracker.play-tracker.live-boxes:1", "rink.camera.follower_box_scale_height"},
  };
  // TODO: this needs to go into or replace the play tracker config, but
  // currently just coming from the file later in parsing (these value mapings
  // arent currently applied anywhere)
  // map_key_configs(config_, live_box_map_dest_from_src);

  // std::cout << config_["pipeline.ds-playtracker"] << std::endl;

  // set_if_not_set(config_,
  // "pipeline.ds-playtracker.dynamic-acceleration-scaling",
  // "rink.camera.dynamic_acceleration_scaling");

  apply_scoreboard_perspective(pipeline);

  YAML::Node offsets = ensure_game_frame_offsets_node(config_);

  size_t area = 0, ww = 0, hh = 0;

  size_t num_video_sources = 0;

  std::vector<std::string> left_files;
  std::vector<std::string> right_files;

  if (!is_camera_source) {
    HM_RETURN_IF_ERROR(gather_stitching_videos(game_dir, force, left_files, right_files, offsets));
    apply_frame_offsets_and_sizes(left_files, right_files, offsets, ww, hh, area, pipeline);
  }
  HM_RETURN_IF_ERROR(set_output_dimensions(
      pipeline,
      is_camera_source,
      camera_sources,
      left_files,
      right_files,
      pipeline_has_hmstitcher,
      game_dir,
      ww,
      hh,
      area,
      num_video_sources));

  configure_audio(pipeline, left_files, right_files, offsets, num_video_sources);
  HM_RETURN_IF_ERROR(configure_encode_file_outputs(pipeline));

  if (show_render_sink) {
    HM_RETURN_IF_ERROR(ensure_render_sink_with_scale(pipeline, show_render_scale));
  }

  //
  // If RTSP server is active, we may need to downscale
  //
  log_enabled_bins(pipeline);
  return absl::OkStatus();
}

absl::Status Configurator::apply_config_item(const std::string& key, const std::string& value) {
  YAML::Node overlaid_config = YAML::Node(YAML::NodeType::Map);
  overlaid_config = set_node_value(overlaid_config, key, value);
  // std::cout << overlaid_config << std::endl;
  //  std::cout << config_ << std::endl;
  config_ = merge_nodes(config_, overlaid_config, /*warn_if_key_not_in_dest=*/false);
  // std::cout << config_ << std::endl;
  return absl::OkStatus();
}

absl::Status Configurator::post_config_pipeline(
    NvDsPipeline& pipeline,
    const NvDsConfig& config,
    uint64_t start_time_ns) {
  // Only pause/wait/seek when needed. Importantly, avoid an unbounded wait here because the GLib main loop is not
  // running yet (so bus watches won't fire), which can look like a "hang" at startup.
  const bool needs_seek =
      start_time_ns || config.hmsticher_config.left_frame_offset_ns || config.hmsticher_config.right_frame_offset_ns;
  if (!needs_seek) {
    return absl::OkStatus();
  }

  GstState state, pending;
  // Quick, non-blocking query.
  (void)gst_element_get_state(pipeline.pipeline, &state, &pending, 0);

  if (state == GST_STATE_NULL || state == GST_STATE_READY) {
    if (gst_element_set_state(pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      return absl::InternalError("Failed to set pipeline to PAUSED");
    }
  }

  constexpr GstClockTime kPauseWaitTimeout = 5 * GST_SECOND;
  GstStateChangeReturn wait_ret = gst_element_get_state(pipeline.pipeline, &state, &pending, kPauseWaitTimeout);
  if (wait_ret == GST_STATE_CHANGE_FAILURE) {
    return absl::InternalError("Failed while waiting for pipeline to PAUSED");
  }
  if (state != GST_STATE_PAUSED) {
    std::cerr << "Warning: pipeline did not reach PAUSED within " << (kPauseWaitTimeout / GST_SECOND) << "s"
              << " (state=" << gstStateToString(state) << ", pending=" << gstStateToString(pending)
              << "). Skipping initial seeks." << std::endl;
    return absl::OkStatus();
  }
  save_dot_file(pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_paused");

  // Seek pre-stitching streams to the proper offsets so that they're in sync
  std::vector<GstElement*> src_bins;
  src_bins.reserve(MAX_SOURCE_BINS);
  for (size_t i = 0; i < pipeline.multi_src_bin.num_bins; ++i) {
    if (!pipeline.multi_src_bin.sub_bins[i].bin) {
      continue;
    }
    if (config.multi_source_config[i].type != NV_DS_SOURCE_URI &&
        config.multi_source_config[i].type != NV_DS_SOURCE_URI_MULTIPLE) {
      continue;
    }
    src_bins.emplace_back(pipeline.multi_src_bin.sub_bins[i].bin);
  }
  if (src_bins.size() == 2) {
    if (config.hmsticher_config.left_frame_offset_ns || start_time_ns) {
      bool result = seek_element(src_bins[0], config.hmsticher_config.left_frame_offset_ns + start_time_ns);
      if (!result) {
        g_printerr("Failed to seek source 0\n");
      }
    }
    if (config.hmsticher_config.right_frame_offset_ns || start_time_ns) {
      // size_t seekTarget = 0.95 * GST_SECOND;
      bool result = seek_element(src_bins[1], config.hmsticher_config.right_frame_offset_ns + start_time_ns);
      if (!result) {
        g_printerr("Failed to seek source 1\n");
      }
    }
  } else if (!src_bins.empty() && start_time_ns) {
    for (auto* bin : src_bins) {
      bool result = seek_element(bin, start_time_ns);
      if (!result) {
        g_printerr("Failed to seek source 0\n");
      }
    }
  }
  for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
    if (pipeline.instance_bins[i].hmaudio_bin.bin && start_time_ns) {
      bool result = seek_element(pipeline.instance_bins[i].hmaudio_bin.bin, start_time_ns);
      if (!result) {
        g_printerr("Failed to seek hmaudio\n");
      }
    }
  }
  return absl::OkStatus();
}

std::string get_section_prefix(const std::string& section_name) {
  auto found_digit = std::find_if(section_name.begin(), section_name.end(), ::isdigit);
  if (found_digit == section_name.end()) {
    return section_name;
  }
  return {section_name.begin(), found_digit};
}

absl::Status Configurator::load_sub_configs(
    const std::string& parent_node_name,
    const std::vector<std::string>& allowed_prefixes,
    const std::string& config_path) {
  if (allowed_prefixes.empty()) {
    return absl::OkStatus();
  }
  YAML::Node parent_node = config_[parent_node_name];
  if (!parent_node.IsDefined()) {
    return absl::OkStatus();
  }
  // prefix -> (actual section header, config yaml)
  std::map<std::string, std::vector<std::pair<std::string, YAML::Node>>> subconfigs;
  std::set<std::string> config_files;
  std::unordered_set<std::string> all_existing_fields;
  for (const auto& node : parent_node) {
    const std::string key = node.first.as<std::string>();
    if (!all_existing_fields.emplace(key).second) {
      return absl::InvalidArgumentError(TO_STRING("Found duplicate YAML section: " << key));
    }
    for (const std::string& prefix : allowed_prefixes) {
      if (!strncmp(key.c_str(), prefix.c_str(), prefix.size())) {
        // Have a prefix match
        std::optional<YAML::Node> subconfig_node = maybe_get_config_file(node.second, config_path);
        if (subconfig_node.has_value()) {
          subconfigs[prefix].emplace_back(key, *subconfig_node);
        }
      }
    }
  }

  // Subfunction to get next available section name
  auto get_next_available_name = [&all_existing_fields](const std::string& prefix) {
    size_t c = 0;
    std::string sn;
    do {
      sn = prefix + std::to_string(c++);
    } while (all_existing_fields.count(sn));
    return sn;
  };

  std::unordered_set<std::string> disable_nodes;
  // Now go through all the sub-configs
  for (const auto& sc_item : subconfigs) {
    for (const auto& subitem : sc_item.second) {
      const std::string& section_name = subitem.first;
      const YAML::Node& section_config = subitem.second;
      for (const auto& subsection_section : section_config) {
        std::string subsection_key = subsection_section.first.as<std::string>();
        std::string subsection_prefix = get_section_prefix(subsection_key);
        std::string new_subsection_name = get_next_available_name(subsection_prefix);
        all_existing_fields.emplace(new_subsection_name);
        YAML::Node subsection_config = subsection_section.second;
        merge_nodes(
            subsection_config,
            YAML::Clone(parent_node[section_name]),
            /*warn_if_key_not_in_dest=*/false);
        // Remove the config file entry
        subsection_config.remove("config-file");
        parent_node[new_subsection_name] = subsection_config;
        disable_nodes.emplace(section_name);
      }
      // Now disable the original section since it has been consumed/expanded
      parent_node[section_name][kEnableFlagField] = 0;
    }
  }
  // Disable the node that had the config filew since we exploded them already
  for (const std::string& s : disable_nodes) {
    parent_node[s][kEnableFlagField] = "0";
  }
  return absl::OkStatus();
}

} // namespace hm
