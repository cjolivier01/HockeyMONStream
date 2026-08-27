#include "configurator.h"

#include <fcntl.h>
#include <gstreamer-1.0/gst/gstelement.h>
#include <gstreamer-1.0/gst/gstpipeline.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <yaml-cpp/node/parse.h>

#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "absl/cleanup/cleanup.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "deepstream_app.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/apps/apps-common/deepstream_sinks.h"
#include "hstream/src/apps/apps-common/deepstream_sources.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/PlayTrackerConfigRoles.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/common/VideoBitrate.h"
#include "hstream/src/libs/common/filesystem.h"
#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
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
constexpr size_t kDefaultStitchingControlPoints = 1500;
constexpr size_t kDefaultStitchingCalibrationFrameCount = 4;

bool is_enabled(YAML::Node n);
std::optional<std::string> local_video_path_from_uri(const std::string& uri);

absl::StatusOr<uint64_t> private_stitch_frame_time(const YAML::Node& config) {
  const auto value = get_node(config, "stitching.stitch_frame_time");
  if (!value.has_value())
    return 0;
  if (!value->IsScalar()) {
    return absl::InvalidArgumentError("stitching.stitch_frame_time must be a scalar HH:MM:SS[.mmm] value");
  }
  try {
    return stitch_frame_time_to_nanoseconds(value->as<std::string>());
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid stitching.stitch_frame_time: " + std::string(error.what()));
  }
}

absl::Status apply_hmstitcher_calibration_sample_span(YAML::Node& pipeline, const YAML::Node& config) {
  if (!get_node(pipeline, "hmstitcher")->IsDefined()) {
    return absl::OkStatus();
  }
  uint64_t stitch_frame_time_ns = 0;
  HM_ASSIGN_OR_RETURN(stitch_frame_time_ns, private_stitch_frame_time(config));
  // hstream's one-pass path reaches calibration frames by normal forward decode,
  // not by video-stitcher's source-side keyframe seek/extract pass. A zero stitch
  // time must therefore calibrate from the first synchronized pairs so playback
  // can begin promptly and no pre-calibration content is consumed.
  if (stitch_frame_time_ns != 0)
    return absl::OkStatus();
  pipeline["hmstitcher"].remove("calibration-sample-span-ns");
  pipeline["hmstitcher"].remove("calibration_sample_span_ns");
  YAML::Node private_properties = pipeline["hmstitcher"]["private-properties"];
  if (private_properties && private_properties.IsMap()) {
    private_properties.remove("calibration-sample-span-ns");
    private_properties.remove("calibration_sample_span_ns");
  }
  return absl::OkStatus();
}

absl::StatusOr<size_t> persisted_stitching_control_points(const YAML::Node& config) {
  const auto value = get_node(config, "hstream_ui.stitching_calibration.control_points");
  if (!value.has_value() || !value->IsScalar()) {
    return absl::InvalidArgumentError("Stitching calibration must persist a positive control_points value");
  }
  try {
    const uint64_t control_points = value->as<uint64_t>();
    if (control_points == 0 || control_points > std::numeric_limits<size_t>::max()) {
      return absl::InvalidArgumentError(
          "Stitching calibration control_points must be a positive platform-sized integer");
    }
    return static_cast<size_t>(control_points);
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid stitching calibration control_points: " + std::string(error.what()));
  }
}

absl::StatusOr<size_t> persisted_stitching_calibration_frame_count(const YAML::Node& config) {
  auto parse_frame_count = [](const YAML::Node& value, const char* path) -> absl::StatusOr<size_t> {
    if (!value.IsScalar()) {
      return absl::InvalidArgumentError(std::string(path) + " must be a positive scalar value");
    }
    try {
      const uint64_t frame_count = value.as<uint64_t>();
      if (frame_count == 0 || frame_count > 64) {
        return absl::InvalidArgumentError(std::string(path) + " must be in the range 1..64");
      }
      return static_cast<size_t>(frame_count);
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + std::string(path) + ": " + std::string(error.what()));
    }
  };
  const auto ui_value = get_node(config, "hstream_ui.stitching_calibration.frame_count");
  const auto canonical_value = get_node(config, "stitching.calibration_frame_count");
  if (!ui_value.has_value() && !canonical_value.has_value()) {
    return kDefaultStitchingCalibrationFrameCount;
  }
  std::optional<size_t> ui_frame_count;
  if (ui_value.has_value()) {
    HM_ASSIGN_OR_RETURN(ui_frame_count, parse_frame_count(*ui_value, "hstream_ui.stitching_calibration.frame_count"));
  }
  std::optional<size_t> canonical_frame_count;
  if (canonical_value.has_value()) {
    HM_ASSIGN_OR_RETURN(
        canonical_frame_count, parse_frame_count(*canonical_value, "stitching.calibration_frame_count"));
  }
  if (ui_frame_count.has_value() && canonical_frame_count.has_value() && *ui_frame_count != *canonical_frame_count) {
    return absl::InvalidArgumentError(
        "hstream_ui.stitching_calibration.frame_count conflicts with stitching.calibration_frame_count");
  }
  return ui_frame_count.has_value() ? *ui_frame_count : *canonical_frame_count;
}

absl::StatusOr<size_t> configured_stitching_calibration_frame_count_from_environment() {
  const char* configured = g_getenv("HM_STITCH_CALIBRATION_FRAME_COUNT");
  if (!configured || !*configured) {
    return kDefaultStitchingCalibrationFrameCount;
  }
  if (!std::all_of(configured, configured + std::strlen(configured), [](unsigned char character) {
        return std::isdigit(character);
      })) {
    return absl::InvalidArgumentError("HM_STITCH_CALIBRATION_FRAME_COUNT must be a positive integer");
  }
  size_t consumed = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(configured, &consumed);
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("HM_STITCH_CALIBRATION_FRAME_COUNT must be a positive integer");
  }
  if (consumed != std::strlen(configured) || parsed == 0 || parsed > 64) {
    return absl::InvalidArgumentError("HM_STITCH_CALIBRATION_FRAME_COUNT must be in the range 1..64");
  }
  return static_cast<size_t>(parsed);
}

absl::StatusOr<size_t> saved_or_environment_stitching_calibration_frame_count(const YAML::Node& config) {
  if (get_node(config, "hstream_ui.stitching_calibration.frame_count").has_value() ||
      get_node(config, "stitching.calibration_frame_count").has_value()) {
    return persisted_stitching_calibration_frame_count(config);
  }
  return configured_stitching_calibration_frame_count_from_environment();
}

absl::Status enforce_stitching_control_points_environment(size_t control_points) {
  const char* configured = g_getenv("HM_MAX_CONTROL_POINTS");
  if (configured && *configured) {
    if (!std::all_of(configured, configured + std::strlen(configured), [](unsigned char character) {
          return std::isdigit(character);
        })) {
      return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive integer");
    }
    size_t consumed = 0;
    unsigned long long runtime_control_points = 0;
    try {
      runtime_control_points = std::stoull(configured, &consumed);
    } catch (const std::exception&) {
      return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive integer");
    }
    if (consumed != std::strlen(configured) || runtime_control_points == 0 ||
        runtime_control_points > std::numeric_limits<size_t>::max()) {
      return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive platform-sized integer");
    }
    if (runtime_control_points != control_points) {
      return absl::InvalidArgumentError(TO_STRING(
          "HM_MAX_CONTROL_POINTS=" << runtime_control_points
                                   << " conflicts with pending stitching calibration control_points="
                                   << control_points));
    }
  } else if (!g_setenv("HM_MAX_CONTROL_POINTS", std::to_string(control_points).c_str(), /*overwrite=*/TRUE)) {
    return absl::InternalError("Unable to publish saved stitching control points to calibration workers");
  }
  std::cout << "Using persisted stitching calibration control-point limit " << control_points << std::endl;
  return absl::OkStatus();
}

absl::Status enforce_stitching_calibration_frame_count_environment(size_t frame_count) {
  const char* configured = g_getenv("HM_STITCH_CALIBRATION_FRAME_COUNT");
  if (configured && *configured) {
    auto runtime_frame_count_or = configured_stitching_calibration_frame_count_from_environment();
    if (!runtime_frame_count_or.ok())
      return runtime_frame_count_or.status();
    const size_t runtime_frame_count = *runtime_frame_count_or;
    if (runtime_frame_count != frame_count) {
      return absl::InvalidArgumentError(TO_STRING(
          "HM_STITCH_CALIBRATION_FRAME_COUNT="
          << runtime_frame_count << " conflicts with pending stitching calibration frame_count=" << frame_count));
    }
  } else if (!g_setenv(
                 "HM_STITCH_CALIBRATION_FRAME_COUNT",
                 std::to_string(frame_count).c_str(),
                 /*overwrite=*/TRUE)) {
    return absl::InternalError("Unable to publish saved stitching calibration frame count to calibration workers");
  }
  std::cout << "Using persisted stitching calibration frame count " << frame_count << std::endl;
  return absl::OkStatus();
}

struct SourceBitrateReference {
  std::string path;
  uint64_t bitrate{0};
  uint64_t width{0};
  uint64_t height{0};
  BitratePerPixel bitrate_per_pixel;
};

std::optional<SourceBitrateReference> select_source_bitrate_reference(
    const std::vector<std::string>& source_video_paths) {
  std::optional<SourceBitrateReference> selected;
  std::unordered_set<std::string> visited;
  for (const std::string& path : source_video_paths) {
    const std::string normalized_path = fs::path(path).lexically_normal().string();
    if (normalized_path.empty() || !visited.insert(normalized_path).second) {
      continue;
    }

    const Videoinfo info = getVideoInfo(normalized_path);
    if (info.video_bit_rate == 0 || info.width <= 0 || info.height <= 0) {
      continue;
    }
    const uint64_t width = static_cast<uint64_t>(info.width);
    const uint64_t height = static_cast<uint64_t>(info.height);
    const uint64_t bitrate = static_cast<uint64_t>(info.video_bit_rate);
    const std::optional<BitratePerPixel> bitrate_per_pixel = make_bitrate_per_pixel(bitrate, width, height);
    if (!bitrate_per_pixel.has_value()) {
      continue;
    }

    const uint64_t pixels = width * height;
    const uint64_t selected_pixels = selected.has_value() ? selected->width * selected->height : 0;
    if (!selected.has_value() || bitrate > selected->bitrate ||
        (bitrate == selected->bitrate && pixels < selected_pixels)) {
      selected = SourceBitrateReference{normalized_path, bitrate, width, height, *bitrate_per_pixel};
    }
  }
  return selected;
}

std::optional<std::string> local_video_path_from_uri(const std::string& uri) {
  if (uri.empty()) {
    return std::nullopt;
  }
  if (!absl::StartsWith(uri, "file://")) {
    return uri.find("://") == std::string::npos ? std::optional<std::string>(uri) : std::nullopt;
  }

  GError* error = nullptr;
  gchar* filename = g_filename_from_uri(uri.c_str(), nullptr, &error);
  if (error) {
    g_error_free(error);
  }
  if (!filename) {
    return uri.substr(std::strlen("file://"));
  }
  std::string path(filename);
  g_free(filename);
  return path;
}

absl::Status sync_parent_directory(const fs::path& path) {
  const int directory_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) {
    return absl::InternalError(TO_STRING(
        "Failed to open archive directory \"" << path.parent_path().string()
                                              << "\" for durability sync: " << std::strerror(errno)));
  }
  if (::fsync(directory_fd) != 0) {
    const int saved_errno = errno;
    ::close(directory_fd);
    return absl::InternalError(TO_STRING(
        "Failed to sync archive directory \"" << path.parent_path().string() << "\": " << std::strerror(saved_errno)));
  }
  if (::close(directory_fd) != 0) {
    return absl::InternalError(TO_STRING(
        "Failed to close archive directory \"" << path.parent_path().string()
                                               << "\" after sync: " << std::strerror(errno)));
  }
  return absl::OkStatus();
}

absl::Status sync_archive_and_parent(
    const fs::path& path,
    const struct stat* expected_stat = nullptr,
    const char* description = "retained archive") {
  const int archive_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (archive_fd < 0) {
    return absl::InternalError(TO_STRING(
        "Failed to open " << description << " \"" << path.string()
                          << "\" for durability sync: " << std::strerror(errno)));
  }
  struct stat opened_stat{};
  if (::fstat(archive_fd, &opened_stat) != 0 ||
      (expected_stat && (opened_stat.st_dev != expected_stat->st_dev || opened_stat.st_ino != expected_stat->st_ino))) {
    const int saved_errno = errno;
    ::close(archive_fd);
    return absl::FailedPreconditionError(
        expected_stat ? TO_STRING("Refusing to sync replaced " << description << " \"" << path.string() << "\"")
                      : TO_STRING(
                            "Failed to inspect " << description << " \"" << path.string()
                                                 << "\": " << std::strerror(saved_errno)));
  }
  if (::fsync(archive_fd) != 0) {
    const int saved_errno = errno;
    ::close(archive_fd);
    return absl::InternalError(
        TO_STRING("Failed to sync " << description << " \"" << path.string() << "\": " << std::strerror(saved_errno)));
  }
  if (::close(archive_fd) != 0) {
    return absl::InternalError(TO_STRING(
        "Failed to close " << description << " \"" << path.string() << "\" after sync: " << std::strerror(errno)));
  }
  return sync_parent_directory(path);
}

fs::path archive_recovery_candidate(const fs::path& output_path, int suffix) {
  const std::string suffix_text = suffix == 0 ? "" : "-" + std::to_string(suffix);
  return output_path.parent_path() /
      (output_path.stem().string() + "-finalization-failed" + suffix_text + output_path.extension().string());
}

fs::path archive_log_sidecar(const fs::path& output_path) {
  fs::path log_path = output_path;
  log_path += ".log";
  return log_path;
}

bool is_archive_recovery_path(const fs::path& candidate, const fs::path& configured_path) {
  if (candidate.parent_path() != configured_path.parent_path() || candidate.extension() != configured_path.extension())
    return false;
  const std::string candidate_stem = candidate.stem().string();
  const std::string prefix = configured_path.stem().string() + "-finalization-failed";
  if (!absl::StartsWith(candidate_stem, prefix))
    return false;
  const std::string suffix = candidate_stem.substr(prefix.size());
  return suffix.empty() ||
      (suffix.front() == '-' && suffix.size() > 1 &&
       std::all_of(suffix.begin() + 1, suffix.end(), [](unsigned char value) { return std::isdigit(value) != 0; }));
}

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
    // YAML::Node assignment writes through the current handle and can replace
    // the node it aliases (including the config root). reset() only rebinds
    // this traversal handle to the selected child.
    current.reset(*next);
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

bool normalize_generated_stitching_backend_choices(YAML::Node& config) {
  const auto generated_matcher =
      get_node(config, "hstream_ui.generated_stitching_backend_choices.control_point_matcher");
  const auto generated_backend = get_node(config, "hstream_ui.generated_stitching_backend_choices.mapping_backend");
  const auto private_matcher = get_node(config, "stitching.control_point_matcher");
  const auto private_backend = get_node(config, "stitching.mapping_backend");
  const bool generated_matches_private = generated_matcher.has_value() && generated_matcher->IsScalar() &&
      generated_backend.has_value() && generated_backend->IsScalar() && private_matcher.has_value() &&
      private_matcher->IsScalar() && private_backend.has_value() && private_backend->IsScalar() &&
      private_matcher->as<std::string>() == generated_matcher->as<std::string>() &&
      private_backend->as<std::string>() == generated_backend->as<std::string>();
  if (!generated_matches_private) {
    return false;
  }

  const auto previous_matcher =
      get_node(config, "hstream_ui.generated_stitching_backend_choices.previous_control_point_matcher");
  const auto previous_backend =
      get_node(config, "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend");
  if (previous_matcher.has_value() && previous_matcher->IsScalar()) {
    config["stitching"]["control_point_matcher"] = previous_matcher->as<std::string>();
  } else {
    remove_yaml_key_path(config, {"stitching", "control_point_matcher"});
  }
  if (previous_backend.has_value() && previous_backend->IsScalar()) {
    config["stitching"]["mapping_backend"] = previous_backend->as<std::string>();
  } else {
    remove_yaml_key_path(config, {"stitching", "mapping_backend"});
  }
  remove_yaml_key_path(config, {"hstream_ui", "generated_stitching_backend_choices"});
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
    current.reset(*next);
  }
  if (!current.IsSequence()) {
    return {};
  }
  return current.as<std::vector<std::string>>();
}

std::string explicit_file_chapter_key(const std::string& file) {
  const std::string filename = fs::path(file).filename().string();
  std::smatch match;
  static const std::regex gopro_pattern(R"(^G[A-Z]([0-9]{2})([0-9]{4})\.(MP4|mp4)$)");
  static const std::regex insta360_pattern(R"(^VID_([0-9]{8})_([0-9]{6})_([0-9]{3})\.(MP4|mp4)$)");
  static const std::regex left_right_pattern(R"((left|right)(?:-([0-9]+))?\.(mp4|mkv|m4v)$)", std::regex::icase);
  if (std::regex_search(filename, match, gopro_pattern)) {
    return "gopro:" + match[2].str() + ":" + match[1].str();
  }
  if (std::regex_search(filename, match, insta360_pattern)) {
    return "insta360:" + match[1].str() + ":" + match[2].str() + ":" + match[3].str();
  }
  if (std::regex_search(filename, match, left_right_pattern)) {
    std::string part = match[2].matched ? match[2].str() : "1";
    const size_t first_nonzero = part.find_first_not_of('0');
    part = first_nonzero == std::string::npos ? "0" : part.substr(first_nonzero);
    const std::string digits = std::to_string(part.size());
    return "lr:" + std::string(10 - std::min<size_t>(digits.size(), 10), '0') + digits + ":" + part;
  }
  return {};
}

void remove_rotation_dependent_rink_cache_keys(YAML::Node& config);
void remove_control_point_dependent_stitching_cache_keys(YAML::Node& config);

void remove_cleanable_stitching_cache_keys(YAML::Node& config) {
  remove_yaml_key_path(config, {"stitching", "frame_offsets"});
  remove_yaml_key_path(config, {"game", "stitching", "frame_offsets"});
  remove_control_point_dependent_stitching_cache_keys(config);
}

void remove_control_point_dependent_stitching_cache_keys(YAML::Node& config) {
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

absl::StatusOr<bool> get_yaml_bool_value(const YAML::Node& node, const std::string& key, bool default_value) {
  const auto value = get_node(node, key);
  if (!value.has_value())
    return default_value;
  try {
    return value->as<bool>();
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Invalid boolean config value for " + key + ": " + exception.what());
  }
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

absl::StatusOr<std::string> validated_video_converter_element(const YAML::Node& value, const std::string& path) {
  if (value.IsNull() || !value.IsScalar()) {
    return absl::InvalidArgumentError(path + " must be nvvideoconvert or dsxvideoconvert");
  }
  const std::string element_name = value.as<std::string>();
  if (!hm::deepstream::is_supported_video_converter_element_name(element_name.c_str())) {
    return absl::InvalidArgumentError(path + " must be nvvideoconvert or dsxvideoconvert");
  }
  return element_name;
}

bool is_video_converter_config_option(const std::string& key) {
  std::string normalized = key;
  std::replace(normalized.begin(), normalized.end(), '-', '_');
  return normalized == "runtime.video_converter" || normalized == "pipeline.application.video_converter";
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

absl::StatusOr<int> read_stitch_max_output_width_node(const YAML::Node& node, const std::string& path) {
  if (!node.IsDefined() || node.IsNull())
    return 0;
  if (!node.IsScalar())
    return absl::InvalidArgumentError(path + " must be a non-negative integer");
  try {
    const int value = node.as<int>();
    if (value < 0)
      return absl::InvalidArgumentError(path + " must be a non-negative integer");
    return value;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid " + path + ": " + std::string(error.what()));
  }
}

absl::StatusOr<int> effective_hmstitcher_max_output_width(const YAML::Node& pipeline) {
  if (!pipeline.IsMap())
    return 0;
  YAML::Node stitcher = pipeline["hmstitcher"];
  if (!stitcher.IsMap())
    return 0;
  for (const auto& [node, prefix] : {
           std::pair<YAML::Node, std::string>{stitcher["properties"], "pipeline.hmstitcher.properties."},
           std::pair<YAML::Node, std::string>{
               stitcher["private-properties"], "pipeline.hmstitcher.private-properties."},
       }) {
    if (!node.IsMap())
      continue;
    for (const char* alias :
         {"max-output-width", "max_output_width", "stitch-max-output-width", "stitch_max_output_width"}) {
      if (node[alias].IsDefined()) {
        return read_stitch_max_output_width_node(node[alias], prefix + alias);
      }
    }
  }
  return 0;
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

bool configurator_internal::hmstitcher_owns_stitching_cleanup(const YAML::Node& config) {
  const auto stitcher = get_node(config, "pipeline.hmstitcher");
  if (!stitcher.has_value() || !stitcher->IsMap()) {
    return false;
  }
  auto bool_at = [&](const std::string& path) {
    const auto value = get_node(config, path);
    return value.has_value() && parse_one_pass_bool(*value, false);
  };
  if (bool_at("pipeline.hmstitcher.enable") || bool_at("pipeline.hmstitcher.configure-only") ||
      bool_at("pipeline.hmstitcher.one-pass-mode")) {
    return true;
  }
  if (get_node(config, "pipeline.hmstitcher.enable").has_value()) {
    return false;
  }
  return bool_at("stitching.enabled");
}

std::vector<std::string> configurator_internal::enabled_source_video_uris(const YAML::Node& pipeline) {
  std::vector<std::string> uris;
  for (const auto& item : pipeline) {
    const std::string key = item.first.as<std::string>();
    if (!absl::StartsWith(key, "source") || !is_enabled(item.second)) {
      continue;
    }
    const NvDsSourceType source_type = static_cast<NvDsSourceType>(get_node_value<int>(item.second, "type", 0));
    if (source_type != NV_DS_SOURCE_URI && source_type != NV_DS_SOURCE_URI_MULTIPLE) {
      continue;
    }

    std::optional<YAML::Node> uri_list = get_node(item.second, "uri-list");
    if (!uri_list.has_value() || !uri_list->IsDefined()) {
      uri_list = get_node(item.second, "uri_list");
    }
    if (uri_list.has_value() && uri_list->IsSequence()) {
      for (const YAML::Node& uri : *uri_list) {
        uris.emplace_back(uri.as<std::string>());
      }
    } else if (uri_list.has_value() && uri_list->IsScalar()) {
      for (const absl::string_view uri : absl::StrSplit(uri_list->as<std::string>(), ';', absl::SkipEmpty())) {
        uris.emplace_back(uri.data(), uri.size());
      }
    }
    const std::optional<YAML::Node> uri = get_node(item.second, "uri");
    if (uri.has_value() && uri->IsScalar()) {
      uris.emplace_back(uri->as<std::string>());
    }
  }
  return uris;
}

configurator_internal::ExplicitStitchingVideoSelection configurator_internal::select_explicit_stitching_videos(
    const YAML::Node& config,
    bool force) {
  ExplicitStitchingVideoSelection selection;
  const std::vector<std::string> ui_left = sequence_path_values(config, {"hstream_ui", "video_roles", "left"});
  const std::vector<std::string> ui_right = sequence_path_values(config, {"hstream_ui", "video_roles", "right"});
  selection.ui_roles_are_authoritative = !ui_left.empty() || !ui_right.empty();

  auto normalize_playlist = [](const std::vector<std::string>& files, std::vector<std::string>& normalized) {
    std::map<std::string, std::string> indexed;
    std::set<std::string> schemes;
    std::set<std::string> unique_paths;
    for (const std::string& file : files) {
      if (!unique_paths.insert(file).second) {
        return false;
      }
      const std::string chapter = explicit_file_chapter_key(file);
      if (!chapter.empty() && !indexed.emplace(chapter, file).second) {
        return false;
      }
      if (!chapter.empty()) {
        schemes.insert(chapter.substr(0, chapter.find(':')));
      }
    }
    if (indexed.size() == files.size() && schemes.size() == 1) {
      for (const auto& [_, file] : indexed) {
        normalized.emplace_back(file);
      }
    } else {
      normalized = files;
    }
    return true;
  };
  if ((!ui_left.empty() && !normalize_playlist(ui_left, selection.left)) ||
      (!ui_right.empty() && !normalize_playlist(ui_right, selection.right))) {
    selection.error = "Explicit UI Left/Right roles have incompatible or duplicate chapter names";
    return selection;
  }

  if (!ui_left.empty() && !ui_right.empty()) {
    // Chapter labels describe each camera's physical file boundaries. They are not cross-camera pair IDs: cameras
    // can split the same frame timeline at different points and therefore have different labels, counts, or schemes.
    selection.left_is_explicit = true;
    selection.right_is_explicit = true;
  } else if (!ui_left.empty()) {
    selection.left_is_explicit = true;
  } else if (!ui_right.empty()) {
    selection.right_is_explicit = true;
  } else if (!force) {
    selection.left = sequence_path_values(config, {"game", "videos", "left"});
    selection.right = sequence_path_values(config, {"game", "videos", "right"});
    auto has_duplicate_path = [](const std::vector<std::string>& files) {
      std::set<std::string> unique;
      return std::any_of(
          files.begin(), files.end(), [&](const std::string& file) { return !unique.insert(file).second; });
    };
    if (has_duplicate_path(selection.left) || has_duplicate_path(selection.right)) {
      selection.error = "Persisted Left/Right camera playlists contain a duplicate path";
      return selection;
    }
    selection.left_is_explicit = !selection.left.empty();
    selection.right_is_explicit = !selection.right.empty();
  }
  return selection;
}

absl::StatusOr<YAML::Node> configurator_internal::build_effective_playtracker_config(
    const YAML::Node& effective_config,
    const ConfigLeafRanks& canonical_value_ranks,
    int native_base_rank,
    const YAML::Node& base_playtracker_config) {
  if (!effective_config.IsMap())
    return absl::InvalidArgumentError("Effective application config must be a YAML map");
  if (native_base_rank < 0)
    return absl::InvalidArgumentError("Native playtracker provenance rank must be non-negative");
  if (!base_playtracker_config.IsMap() || !base_playtracker_config["play-tracker"].IsMap())
    return absl::InvalidArgumentError("Playtracker config must contain a play-tracker map");

  YAML::Node result = YAML::Clone(base_playtracker_config);
  YAML::Node play_tracker = result["play-tracker"];
  YAML::Node live_boxes = play_tracker["live-boxes"];
  if (!live_boxes.IsSequence() || live_boxes.size() == 0)
    return absl::InvalidArgumentError("Playtracker config must contain at least one live-box");
  PlayTrackerLiveBoxRoles roles;
  HM_ASSIGN_OR_RETURN(roles, resolve_playtracker_live_box_roles(live_boxes));

  enum class ScalarType { kString, kBool, kInt, kDouble };
  auto validate_scalar = [](const YAML::Node& value, ScalarType type, const std::string& path) -> absl::Status {
    if (!value.IsDefined() || value.IsNull() || !value.IsScalar())
      return absl::InvalidArgumentError(path + " must be a non-null scalar");
    try {
      switch (type) {
        case ScalarType::kString:
          (void)value.as<std::string>();
          break;
        case ScalarType::kBool:
          (void)value.as<bool>();
          break;
        case ScalarType::kInt:
          (void)value.as<int>();
          break;
        case ScalarType::kDouble: {
          const double parsed = value.as<double>();
          if (!std::isfinite(parsed))
            return absl::InvalidArgumentError(path + " must be finite");
          break;
        }
      }
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + path + ": " + error.what());
    }
    return absl::OkStatus();
  };

  auto copy_required = [&](YAML::Node destination,
                           const char* destination_key,
                           const char* source_path,
                           ScalarType type,
                           const std::string& destination_path) {
    const std::optional<YAML::Node> source = get_node(effective_config, source_path);
    if (!source || !source->IsDefined() || source->IsNull())
      return absl::InvalidArgumentError(std::string("Effective baseline is missing required key ") + source_path);
    const YAML::Node current = destination[destination_key];
    const auto source_rank_it = canonical_value_ranks.find(source_path);
    const int source_rank = source_rank_it == canonical_value_ranks.end() ? 0 : source_rank_it->second;
    if (!current.IsDefined() || current.IsNull() || source_rank > native_base_rank) {
      HM_RETURN_IF_ERROR(validate_scalar(*source, type, source_path));
      destination[destination_key] = YAML::Clone(*source);
    } else {
      HM_RETURN_IF_ERROR(validate_scalar(current, type, destination_path));
    }
    return absl::OkStatus();
  };

  auto copy_global = [&](const char* destination_key, const char* source_path, ScalarType type) {
    return copy_required(
        play_tracker, destination_key, source_path, type, std::string("play-tracker.") + destination_key);
  };

  HM_RETURN_IF_ERROR(copy_global("camera-name", "camera.name", ScalarType::kString));
  HM_RETURN_IF_ERROR(copy_global("no-wide-start", "play_tracker.no_wide_start", ScalarType::kBool));
  HM_RETURN_IF_ERROR(copy_global("ignore-largest-bbox", "rink.tracking.cam_ignore_largest", ScalarType::kBool));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "min-considered-group-velocity",
      "rink.camera.breakaway_detection.min_considered_group_velocity",
      ScalarType::kDouble,
      "play-tracker.min-considered-group-velocity"));
  HM_RETURN_IF_ERROR(copy_global(
      "group-ratio-threshold", "rink.camera.breakaway_detection.group_ratio_threshold", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "group-velocity-speed-ratio",
      "rink.camera.breakaway_detection.group_velocity_speed_ratio",
      ScalarType::kDouble,
      "play-tracker.group-velocity-speed-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "scale-speed-constraints",
      "rink.camera.breakaway_detection.scale_speed_constraints",
      ScalarType::kDouble,
      "play-tracker.scale-speed-constraints"));
  HM_RETURN_IF_ERROR(
      copy_global("nonstop-delay-count", "rink.camera.breakaway_detection.nonstop_delay_count", ScalarType::kInt));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "overshoot-scale-speed-ratio",
      "rink.camera.breakaway_detection.overshoot_scale_speed_ratio",
      ScalarType::kDouble,
      "play-tracker.overshoot-scale-speed-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      play_tracker,
      "overshoot-stop-delay-count",
      "rink.camera.breakaway_detection.overshoot_stop_delay_count",
      ScalarType::kInt,
      "play-tracker.overshoot-stop-delay-count"));
  HM_RETURN_IF_ERROR(copy_global("max-speed-ratio-x", "rink.camera.max_speed_ratio_x", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-speed-ratio-y", "rink.camera.max_speed_ratio_y", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-accel-ratio-x", "rink.camera.max_accel_ratio_x", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("max-accel-ratio-y", "rink.camera.max_accel_ratio_y", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(
      copy_global("follower-box-min-height-ratio", "rink.camera.follower_box_min_height_ratio", ScalarType::kDouble));
  HM_RETURN_IF_ERROR(copy_global("zoom-in-aggressiveness", "rink.camera.zoom_in_aggressiveness", ScalarType::kInt));
  const int zoom_in_aggressiveness = play_tracker["zoom-in-aggressiveness"].as<int>();
  if (zoom_in_aggressiveness < 0 || zoom_in_aggressiveness > 100) {
    return absl::InvalidArgumentError("rink.camera.zoom_in_aggressiveness must be from 0 through 100");
  }

  for (size_t index = 0; index < live_boxes.size(); ++index) {
    YAML::Node box = live_boxes[index];
    const YAML::Node name = box["name"];
    if (name.IsDefined() && !name.IsNull()) {
      HM_RETURN_IF_ERROR(validate_scalar(name, ScalarType::kString, "play-tracker.live-boxes.name"));
    }
    const std::string box_path = "play-tracker.live-boxes[" + std::to_string(index) + "].";
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "time-to-dest-speed-limit-frames",
        "rink.camera.time_to_dest_speed_limit_frames",
        ScalarType::kInt,
        box_path + "time-to-dest-speed-limit-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "time-to-dest-stop-speed-threshold",
        "rink.camera.time_to_dest_stop_speed_threshold",
        ScalarType::kDouble,
        box_path + "time-to-dest-stop-speed-threshold"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-on-dir-change-delay",
        "rink.camera.resizing_stop_on_dir_change_delay",
        ScalarType::kInt,
        box_path + "resizing-stop-on-dir-change-delay"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-cancel-stop-on-opposite-dir",
        "rink.camera.resizing_cancel_stop_on_opposite_dir",
        ScalarType::kBool,
        box_path + "resizing-cancel-stop-on-opposite-dir"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-cancel-hysteresis-frames",
        "rink.camera.resizing_stop_cancel_hysteresis_frames",
        ScalarType::kInt,
        box_path + "resizing-stop-cancel-hysteresis-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-stop-delay-cooldown-frames",
        "rink.camera.resizing_stop_delay_cooldown_frames",
        ScalarType::kInt,
        box_path + "resizing-stop-delay-cooldown-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-time-to-dest-speed-limit-frames",
        "rink.camera.resizing_time_to_dest_speed_limit_frames",
        ScalarType::kInt,
        box_path + "resizing-time-to-dest-speed-limit-frames"));
    HM_RETURN_IF_ERROR(copy_required(
        box,
        "resizing-time-to-dest-stop-speed-threshold",
        "rink.camera.resizing_time_to_dest_stop_speed_threshold",
        ScalarType::kDouble,
        box_path + "resizing-time-to-dest-stop-speed-threshold"));
  }
  YAML::Node follower = live_boxes[roles.follower_index];
  const YAML::Node follower_name = follower["name"];
  const bool named_follower = follower_name.IsScalar() && follower_name.as<std::string>() == "current_roi_aspect";
  const std::string follower_path =
      named_follower ? "play-tracker.live-boxes[current_roi_aspect]." : "play-tracker.live-boxes[follower].";
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "stop-translation-on-dir-change-delay",
      "rink.camera.stop_on_dir_change_delay",
      ScalarType::kInt,
      follower_path + "stop-translation-on-dir-change-delay"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "cancel-stop-on-opposite-dir",
      "rink.camera.cancel_stop_on_opposite_dir",
      ScalarType::kBool,
      follower_path + "cancel-stop-on-opposite-dir"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "cancel-stop-hysteresis-frames",
      "rink.camera.stop_cancel_hysteresis_frames",
      ScalarType::kInt,
      follower_path + "cancel-stop-hysteresis-frames"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "stop-delay-cooldown-frames",
      "rink.camera.stop_delay_cooldown_frames",
      ScalarType::kInt,
      follower_path + "stop-delay-cooldown-frames"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "post-nonstop-stop-delay-count",
      "rink.camera.breakaway_detection.post_nonstop_stop_delay_count",
      ScalarType::kInt,
      follower_path + "post-nonstop-stop-delay-count"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "sticky-size-ratio-to-frame-width",
      "rink.camera.sticky_size_ratio_to_frame_width",
      ScalarType::kDouble,
      follower_path + "sticky-size-ratio-to-frame-width"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "sticky-translation-gaussian-mult",
      "rink.camera.sticky_translation_gaussian_mult",
      ScalarType::kDouble,
      follower_path + "sticky-translation-gaussian-mult"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "unsticky-translation-size-ratio",
      "rink.camera.unsticky_translation_size_ratio",
      ScalarType::kDouble,
      follower_path + "unsticky-translation-size-ratio"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "scale-dest-width",
      "rink.camera.follower_box_scale_width",
      ScalarType::kDouble,
      follower_path + "scale-dest-width"));
  HM_RETURN_IF_ERROR(copy_required(
      follower,
      "scale-dest-height",
      "rink.camera.follower_box_scale_height",
      ScalarType::kDouble,
      follower_path + "scale-dest-height"));
  std::vector<size_t> normalized_order;
  HM_ASSIGN_OR_RETURN(normalized_order, normalized_playtracker_live_box_order(live_boxes));
  YAML::Node normalized_boxes(YAML::NodeType::Sequence);
  for (const size_t index : normalized_order)
    normalized_boxes.push_back(YAML::Clone(live_boxes[index]));
  play_tracker["live-boxes"] = normalized_boxes;
  return result;
}

absl::Status configurator_internal::validate_mixed_explicit_auto_playlists(
    bool left_is_explicit,
    bool right_is_explicit,
    size_t auto_left_chapters,
    size_t auto_right_chapters) {
  if (left_is_explicit == right_is_explicit) {
    return absl::OkStatus();
  }
  const size_t auto_chapters = left_is_explicit ? auto_right_chapters : auto_left_chapters;
  if (auto_chapters <= 1) {
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      "Mixed Explicit/Auto camera selection is ambiguous when the Auto side has multiple physical chapter files. "
      "Select both Left and Right explicitly, or use Auto for both cameras.");
}

absl::StatusOr<double> configurator_internal::effective_stitch_output_rotation(const YAML::Node& config) {
  for (const char* path : {
           "pipeline.hmstitcher.post-stitch-rotate-degrees",
           "pipeline.hmstitcher.post_stitch_rotate_degrees",
           "stitching.post_stitch_rotate_degrees",
       }) {
    const auto value = get_node(config, path);
    if (!value.has_value() || value->IsNull()) {
      continue;
    }
    try {
      const double rotation = value->as<double>();
      if (!std::isfinite(rotation)) {
        return absl::InvalidArgumentError(std::string(path) + " must be finite");
      }
      return rotation == 0.0 ? 0.0 : rotation;
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError("Invalid " + std::string(path) + ": " + error.what());
    }
  }
  return 0.0;
}

namespace {

bool same_file_identity(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

absl::StatusOr<std::optional<struct stat>> inspect_archive_entry(const fs::path& path, const char* description) {
  struct stat entry_stat{};
  if (::lstat(path.c_str(), &entry_stat) == 0)
    return std::optional<struct stat>(entry_stat);
  const int saved_errno = errno;
  if (saved_errno == ENOENT)
    return std::optional<struct stat>();
  return absl::InternalError(
      TO_STRING("Failed to inspect " << description << " \"" << path.string() << "\": " << std::strerror(saved_errno)));
}

int rename_archive_entry_no_replace(
    int source_directory_fd,
    const char* source_name,
    int destination_directory_fd,
    const char* destination_name) {
  constexpr unsigned int kRenameNoReplace = 1;
  return static_cast<int>(::syscall(
      SYS_renameat2, source_directory_fd, source_name, destination_directory_fd, destination_name, kRenameNoReplace));
}

absl::Status remove_archive_entry_if_owned(
    const fs::path& path,
    const struct stat& expected_stat,
    const char* description,
    const fs::path* required_published_path = nullptr,
    const struct stat* required_published_stat = nullptr,
    const fs::path* second_required_published_path = nullptr,
    const struct stat* second_required_published_stat = nullptr) {
  const auto validate_required_publications = [&]() -> absl::Status {
    const std::array<std::pair<const fs::path*, const struct stat*>, 2> required = {
        std::make_pair(required_published_path, required_published_stat),
        std::make_pair(second_required_published_path, second_required_published_stat)};
    for (const auto& [required_path, required_stat] : required) {
      if (!required_path || !required_stat)
        continue;
      auto published_stat = inspect_archive_entry(*required_path, "published recovery entry");
      if (!published_stat.ok())
        return published_stat.status();
      if (!published_stat->has_value() || !same_file_identity(published_stat->value(), *required_stat)) {
        return absl::FailedPreconditionError(
            TO_STRING("Published recovery identity changed at \"" << required_path->string() << "\""));
      }
    }
    return absl::OkStatus();
  };
  auto current_stat = inspect_archive_entry(path, description);
  if (!current_stat.ok())
    return current_stat.status();
  if (!current_stat->has_value())
    return absl::OkStatus();
  if (!same_file_identity(current_stat->value(), expected_stat)) {
    return absl::FailedPreconditionError(
        TO_STRING("Refusing to remove replaced " << description << " \"" << path.string() << "\""));
  }

  const int pinned_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct stat pinned_stat{};
  if (pinned_fd < 0 || ::fstat(pinned_fd, &pinned_stat) != 0 || !S_ISREG(pinned_stat.st_mode) ||
      !same_file_identity(pinned_stat, expected_stat)) {
    const int saved_errno = pinned_fd < 0 ? errno : ESTALE;
    if (pinned_fd >= 0)
      ::close(pinned_fd);
    return absl::FailedPreconditionError(
        TO_STRING("Failed to pin " << description << " \"" << path.string() << "\": " << std::strerror(saved_errno)));
  }

  const fs::path parent_path = path.parent_path().empty() ? fs::path(".") : path.parent_path();
  const std::string filename = path.filename().string();
  const int parent_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) {
    ::close(pinned_fd);
    return absl::InternalError(TO_STRING(
        "Failed to open parent directory for " << description << " \"" << path.string()
                                               << "\": " << std::strerror(errno)));
  }

  std::string cleanup_name;
  for (int attempt = 0; attempt < 10; ++attempt) {
    gchar* uuid = g_uuid_string_random();
    cleanup_name = std::string(".hstream-cleanup-") + uuid;
    g_free(uuid);
    if (::mkdirat(parent_fd, cleanup_name.c_str(), S_IRWXU) == 0)
      break;
    if (errno != EEXIST) {
      const int saved_errno = errno;
      ::close(pinned_fd);
      ::close(parent_fd);
      return absl::InternalError(TO_STRING(
          "Failed to create protected cleanup directory for " << description << " \"" << path.string()
                                                              << "\": " << std::strerror(saved_errno)));
    }
    cleanup_name.clear();
  }
  if (cleanup_name.empty()) {
    ::close(pinned_fd);
    ::close(parent_fd);
    return absl::ResourceExhaustedError(TO_STRING(
        "Failed to choose protected cleanup directory for " << description << " \"" << path.string() << "\""));
  }

  const int cleanup_fd = ::openat(parent_fd, cleanup_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (cleanup_fd < 0) {
    const int saved_errno = errno;
    ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
    ::close(pinned_fd);
    ::close(parent_fd);
    return absl::InternalError(TO_STRING(
        "Failed to open protected cleanup directory for " << description << " \"" << path.string()
                                                          << "\": " << std::strerror(saved_errno)));
  }

  if (rename_archive_entry_no_replace(parent_fd, filename.c_str(), cleanup_fd, "entry") != 0) {
    const int saved_errno = errno;
    ::close(cleanup_fd);
    ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (saved_errno == ENOENT)
      return absl::OkStatus();
    return absl::InternalError(TO_STRING(
        "Failed to quarantine " << description << " \"" << path.string() << "\": " << std::strerror(saved_errno)));
  }

  struct stat quarantined_stat{};
  const int inspect_result = ::fstatat(cleanup_fd, "entry", &quarantined_stat, AT_SYMLINK_NOFOLLOW);
  if (inspect_result != 0 || !same_file_identity(quarantined_stat, expected_stat)) {
    const int inspect_errno = inspect_result != 0 ? errno : 0;
    const int restore_result = rename_archive_entry_no_replace(cleanup_fd, "entry", parent_fd, filename.c_str());
    const int restore_errno = errno;
    ::close(cleanup_fd);
    if (restore_result == 0)
      ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
    ::close(pinned_fd);
    ::close(parent_fd);
    if (restore_result != 0) {
      return absl::FailedPreconditionError(TO_STRING(
          "Quarantined a replaced " << description << " from \"" << path.string()
                                    << "\" and could not restore it: " << std::strerror(restore_errno)));
    }
    return absl::FailedPreconditionError(
        inspect_errno != 0
            ? TO_STRING(
                  "Failed to inspect quarantined " << description << " \"" << path.string()
                                                   << "\": " << std::strerror(inspect_errno))
            : TO_STRING("Refusing to remove replaced " << description << " \"" << path.string() << "\""));
  }

  {
    const absl::Status required_status = validate_required_publications();
    if (!required_status.ok()) {
      const int restore_result = rename_archive_entry_no_replace(cleanup_fd, "entry", parent_fd, filename.c_str());
      const int restore_errno = errno;
      ::close(cleanup_fd);
      if (restore_result == 0)
        ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
      ::close(pinned_fd);
      ::close(parent_fd);
      if (restore_result != 0) {
        return absl::FailedPreconditionError(TO_STRING(
            "Published recovery was replaced while quarantining "
            << description << " \"" << path.string()
            << "\", and the source could not be restored: " << std::strerror(restore_errno)));
      }
      return absl::FailedPreconditionError(TO_STRING(
          "Published recovery was replaced while quarantining " << description << " \"" << path.string()
                                                                << "\": " << required_status.message()));
    }
  }

  if (::linkat(cleanup_fd, "entry", cleanup_fd, "guard", 0) != 0) {
    const int saved_errno = errno;
    const int restore_result = rename_archive_entry_no_replace(cleanup_fd, "entry", parent_fd, filename.c_str());
    ::close(cleanup_fd);
    if (restore_result == 0)
      ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
    ::close(pinned_fd);
    ::close(parent_fd);
    return absl::InternalError(TO_STRING(
        "Failed to guard quarantined " << description << " \"" << path.string()
                                       << "\": " << std::strerror(saved_errno)));
  }

  if (::unlinkat(cleanup_fd, "entry", 0) != 0) {
    const int saved_errno = errno;
    ::close(cleanup_fd);
    ::close(pinned_fd);
    ::close(parent_fd);
    return absl::InternalError(TO_STRING(
        "Failed to remove quarantined " << description << " \"" << path.string()
                                        << "\": " << std::strerror(saved_errno)));
  }

  const int remove_guard_result = ::unlinkat(cleanup_fd, "guard", 0);
  const int remove_guard_errno = errno;
  if (remove_guard_result == 0 &&
      ((required_published_path && required_published_stat) ||
       (second_required_published_path && second_required_published_stat))) {
    if (std::strcmp(description, "archive work file") == 0 &&
        g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE")) {
      ::unlink(required_published_path->c_str());
      const int replacement_fd =
          ::open(required_published_path->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign archive after quarantine";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    const absl::Status required_status = validate_required_publications();
    if (!required_status.ok()) {
      const int restore_result = ::linkat(pinned_fd, "", parent_fd, filename.c_str(), AT_EMPTY_PATH);
      const int restore_errno = errno;
      const std::string rescue_name = filename + ".hstream-pin";
      const int rescue_result =
          restore_result == 0 ? 0 : ::linkat(pinned_fd, "", parent_fd, rescue_name.c_str(), AT_EMPTY_PATH);
      const int rescue_errno = errno;
      ::close(cleanup_fd);
      if (rescue_result == 0)
        ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
      ::close(pinned_fd);
      ::close(parent_fd);
      return absl::FailedPreconditionError(TO_STRING(
          "Published recovery was replaced after quarantining "
          << description << " \"" << path.string() << "\"; source restore "
          << (restore_result == 0
                  ? "succeeded"
                  : (rescue_result == 0 ? TO_STRING(
                                              "failed (" << std::strerror(restore_errno) << "); retained guard at \""
                                                         << rescue_name << "\"")
                                        : TO_STRING("and guard retention failed: " << std::strerror(rescue_errno))))
          << "; " << required_status.message()));
    }
  }
  const int close_cleanup_result = ::close(cleanup_fd);
  const int close_cleanup_errno = errno;
  const int remove_cleanup_result = ::unlinkat(parent_fd, cleanup_name.c_str(), AT_REMOVEDIR);
  const int remove_cleanup_errno = errno;
  int directory_sync_result = 0;
  int directory_sync_errno = 0;
  int restore_after_sync_result = 0;
  int restore_after_sync_errno = 0;
  if (remove_guard_result == 0 && close_cleanup_result == 0 && remove_cleanup_result == 0) {
    directory_sync_result = ::fsync(parent_fd);
    directory_sync_errno = errno;
    if (directory_sync_result != 0) {
      restore_after_sync_result = ::linkat(pinned_fd, "", parent_fd, filename.c_str(), AT_EMPTY_PATH);
      restore_after_sync_errno = errno;
      if (restore_after_sync_result == 0)
        ::fsync(parent_fd);
    }
  }
  const int close_parent_result = ::close(parent_fd);
  const int close_parent_errno = errno;
  const int close_pinned_result = ::close(pinned_fd);
  const int close_pinned_errno = errno;
  if (remove_guard_result != 0 || close_cleanup_result != 0 || remove_cleanup_result != 0 ||
      directory_sync_result != 0 || close_parent_result != 0 || close_pinned_result != 0) {
    const int saved_errno = remove_guard_result != 0
        ? remove_guard_errno
        : (close_cleanup_result != 0
               ? close_cleanup_errno
               : (remove_cleanup_result != 0
                      ? remove_cleanup_errno
                      : (directory_sync_result != 0
                             ? directory_sync_errno
                             : (close_parent_result != 0 ? close_parent_errno : close_pinned_errno))));
    return absl::InternalError(TO_STRING(
        "Removed " << description << " \"" << path.string()
                   << "\" but failed to make cleanup durable: " << std::strerror(saved_errno)
                   << (directory_sync_result != 0
                           ? (restore_after_sync_result == 0
                                  ? "; the original pathname was restored"
                                  : TO_STRING("; pathname restore failed: " << std::strerror(restore_after_sync_errno)))
                           : "")));
  }
  return absl::OkStatus();
}

absl::Status create_archive_recovery_link(
    int source_fd,
    const fs::path& source,
    const struct stat& expected_source_stat,
    const fs::path& destination,
    const char* description) {
  struct stat pinned_source_stat{};
  if (source_fd < 0 || ::fstat(source_fd, &pinned_source_stat) != 0 || !S_ISREG(pinned_source_stat.st_mode) ||
      !same_file_identity(pinned_source_stat, expected_source_stat)) {
    return absl::FailedPreconditionError(
        TO_STRING("Refusing to link replaced " << description << " \"" << source.string() << "\""));
  }
  if (::linkat(source_fd, "", AT_FDCWD, destination.c_str(), AT_EMPTY_PATH) != 0) {
    const int saved_errno = errno;
    if (saved_errno == EEXIST)
      return absl::AlreadyExistsError(
          TO_STRING(description << " already exists at \"" << destination.string() << "\""));
    return absl::InternalError(TO_STRING(
        "Failed to publish " << description << " at \"" << destination.string()
                             << "\": " << std::strerror(saved_errno)));
  }
  auto destination_stat = inspect_archive_entry(destination, description);
  if (!destination_stat.ok())
    return destination_stat.status();
  if (!destination_stat->has_value() || !same_file_identity(destination_stat->value(), expected_source_stat)) {
    return absl::FailedPreconditionError(
        TO_STRING("Published " << description << " was replaced at \"" << destination.string() << "\""));
  }
  return absl::OkStatus();
}

absl::Status retire_archive_recovery_guards(
    const fs::path& recovery_path,
    const struct stat& recovery_stat,
    int recovery_fd,
    const fs::path& recovery_log_path,
    const struct stat* recovery_log_stat,
    int recovery_log_fd,
    const fs::path* video_guard_override = nullptr,
    const fs::path* log_guard_override = nullptr,
    bool allow_test_injection = true) {
  const fs::path default_recovery_guard_path = recovery_path.string() + ".hstream-pin";
  const fs::path default_recovery_log_guard_path = recovery_log_path.string() + ".hstream-pin";
  const fs::path& recovery_guard_path = video_guard_override ? *video_guard_override : default_recovery_guard_path;
  const fs::path& recovery_log_guard_path = log_guard_override ? *log_guard_override : default_recovery_log_guard_path;
  const auto validate_pair = [&]() -> absl::Status {
    auto visible_video = inspect_archive_entry(recovery_path, "committed archive recovery file");
    if (!visible_video.ok())
      return visible_video.status();
    if (!visible_video->has_value() || !same_file_identity(visible_video->value(), recovery_stat)) {
      return absl::FailedPreconditionError(
          TO_STRING("Committed archive recovery video was replaced at \"" << recovery_path.string() << "\""));
    }
    if (recovery_log_stat) {
      auto visible_log = inspect_archive_entry(recovery_log_path, "committed archive recovery log");
      if (!visible_log.ok())
        return visible_log.status();
      if (!visible_log->has_value() || !same_file_identity(visible_log->value(), *recovery_log_stat)) {
        return absl::FailedPreconditionError(
            TO_STRING("Committed archive recovery log was replaced at \"" << recovery_log_path.string() << "\""));
      }
    }
    return absl::OkStatus();
  };
  const auto restore_missing_guard = [&](int fd,
                                         const fs::path& source,
                                         const struct stat& expected_stat,
                                         const fs::path& guard,
                                         const char* description) -> absl::Status {
    auto current_guard = inspect_archive_entry(guard, description);
    if (!current_guard.ok())
      return current_guard.status();
    if (current_guard->has_value()) {
      return same_file_identity(current_guard->value(), expected_stat)
          ? absl::OkStatus()
          : absl::FailedPreconditionError(
                TO_STRING("Could not restore " << description << " because \"" << guard.string() << "\" is occupied"));
    }
    HM_RETURN_IF_ERROR(create_archive_recovery_link(fd, source, expected_stat, guard, description));
    return sync_parent_directory(guard);
  };

  HM_RETURN_IF_ERROR(validate_pair());
  const absl::Status video_guard_cleanup = remove_archive_entry_if_owned(
      recovery_guard_path,
      recovery_stat,
      "committed archive recovery identity guard",
      &recovery_path,
      &recovery_stat,
      recovery_log_stat ? &recovery_log_path : nullptr,
      recovery_log_stat);
  if (!video_guard_cleanup.ok()) {
    return video_guard_cleanup;
  }
  if (allow_test_injection && g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_DURING_ARCHIVE_GUARD_RETIREMENT")) {
    g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_DURING_ARCHIVE_GUARD_RETIREMENT");
    return absl::UnavailableError("archive recovery interruption requested during guard retirement");
  }
  if (allow_test_injection && recovery_log_stat &&
      g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_LOG_BETWEEN_GUARDS")) {
    g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_LOG_BETWEEN_GUARDS");
    ::unlink(recovery_log_path.c_str());
    const int replacement_fd =
        ::open(recovery_log_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (replacement_fd >= 0) {
      constexpr char kReplacement[] = "injected foreign archive log between guard retirements";
      const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
      (void)replacement_bytes;
      ::close(replacement_fd);
    }
  }
  const absl::Status pair_after_video_guard = validate_pair();
  if (!pair_after_video_guard.ok()) {
    const absl::Status video_guard_restored = restore_missing_guard(
        recovery_fd, recovery_path, recovery_stat, recovery_guard_path, "archive recovery identity guard");
    if (!video_guard_restored.ok())
      return video_guard_restored;
    return pair_after_video_guard;
  }
  if (recovery_log_stat) {
    const absl::Status log_guard_cleanup = remove_archive_entry_if_owned(
        recovery_log_guard_path,
        *recovery_log_stat,
        "committed archive log recovery identity guard",
        &recovery_path,
        &recovery_stat,
        &recovery_log_path,
        recovery_log_stat);
    if (!log_guard_cleanup.ok()) {
      const absl::Status restored = restore_missing_guard(
          recovery_fd, recovery_path, recovery_stat, recovery_guard_path, "archive recovery identity guard");
      return restored.ok() ? log_guard_cleanup : restored;
    }
  }
  const absl::Status committed_pair = validate_pair();
  if (!committed_pair.ok()) {
    const absl::Status video_guard_restored = restore_missing_guard(
        recovery_fd, recovery_path, recovery_stat, recovery_guard_path, "archive recovery identity guard");
    if (!video_guard_restored.ok())
      return video_guard_restored;
    if (recovery_log_stat) {
      const absl::Status log_guard_restored = restore_missing_guard(
          recovery_log_fd,
          recovery_log_path,
          *recovery_log_stat,
          recovery_log_guard_path,
          "archive log recovery identity guard");
      if (!log_guard_restored.ok())
        return log_guard_restored;
    }
    return committed_pair;
  }
  return sync_parent_directory(recovery_path);
}

std::optional<fs::path> provisional_archive_log_sidecar(
    const fs::path& output_path,
    const fs::path& recovery_name_base) {
  if (output_path.parent_path() != recovery_name_base.parent_path() ||
      output_path.extension() != recovery_name_base.extension()) {
    return std::nullopt;
  }
  const std::string filename = output_path.filename().string();
  const std::string prefix = recovery_name_base.stem().string() + ".hstream-run-";
  const std::string extension = recovery_name_base.extension().string();
  if (filename.size() <= prefix.size() + extension.size() || !absl::StartsWith(filename, prefix) ||
      !absl::EndsWith(filename, extension)) {
    return std::nullopt;
  }
  const std::string ownership = filename.substr(prefix.size(), filename.size() - prefix.size() - extension.size());
  std::smatch ownership_match;
  static const std::regex versioned_backend_and_ui_owner(
      R"(^v3-[0-9]+-([0-9]+-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$)");
  if (!std::regex_match(ownership, ownership_match, versioned_backend_and_ui_owner))
    return std::nullopt;
  return recovery_name_base.parent_path() /
      (recovery_name_base.stem().string() + ".hstream-run-ui-" + ownership_match[1].str() + extension + ".log");
}

absl::StatusOr<std::optional<fs::path>> preserve_archive_work_file(
    const fs::path& output_path,
    const fs::path& recovery_name_base) {
  struct stat output_stat{};
  const int output_fd = ::open(output_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (output_fd < 0) {
    if (errno == ENOENT)
      return std::optional<fs::path>();
    return absl::InternalError(
        TO_STRING("Failed to inspect archive work file \"" << output_path.string() << "\": " << std::strerror(errno)));
  }
  absl::Cleanup close_output = [output_fd]() { ::close(output_fd); };
  struct stat named_output_stat{};
  if (::fstat(output_fd, &output_stat) != 0 || ::lstat(output_path.c_str(), &named_output_stat) != 0 ||
      !same_file_identity(output_stat, named_output_stat)) {
    return absl::FailedPreconditionError(
        TO_STRING("Archive work file was replaced while being pinned: \"" << output_path.string() << "\""));
  }
  if (!S_ISREG(output_stat.st_mode) || output_stat.st_size <= 0)
    return std::optional<fs::path>();

  const fs::path resolved_output_log_path = archive_log_sidecar(output_path);
  const std::optional<fs::path> provisional_log = provisional_archive_log_sidecar(output_path, recovery_name_base);
  const auto restore_guarded_log_name = [](const fs::path& log_path) -> absl::Status {
    auto visible_log = inspect_archive_entry(log_path, "guarded archive log sidecar");
    if (!visible_log.ok())
      return visible_log.status();
    if (visible_log->has_value())
      return absl::OkStatus();
    const fs::path guard_path = log_path.string() + ".hstream-pin";
    auto guard_stat = inspect_archive_entry(guard_path, "guard-only archive log sidecar");
    if (!guard_stat.ok())
      return guard_stat.status();
    if (!guard_stat->has_value())
      return absl::OkStatus();
    if (!S_ISREG(guard_stat->value().st_mode)) {
      return absl::FailedPreconditionError(
          TO_STRING("Archive log identity guard is not a regular file: \"" << guard_path.string() << "\""));
    }
    const int guard_fd = ::open(guard_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat pinned_guard_stat{};
    if (guard_fd < 0 || ::fstat(guard_fd, &pinned_guard_stat) != 0 ||
        !same_file_identity(pinned_guard_stat, guard_stat->value())) {
      if (guard_fd >= 0)
        ::close(guard_fd);
      return absl::FailedPreconditionError(
          TO_STRING("Archive log identity guard changed at \"" << guard_path.string() << "\""));
    }
    const absl::Status restored = create_archive_recovery_link(
        guard_fd, guard_path, pinned_guard_stat, log_path, "restored guarded archive log sidecar");
    ::close(guard_fd);
    HM_RETURN_IF_ERROR(restored);
    return sync_archive_and_parent(log_path, &pinned_guard_stat, "restored guarded archive log sidecar");
  };
  HM_RETURN_IF_ERROR(restore_guarded_log_name(resolved_output_log_path));
  if (provisional_log.has_value())
    HM_RETURN_IF_ERROR(restore_guarded_log_name(*provisional_log));
  struct stat resolved_output_log_stat{};
  const bool has_resolved_output_log = ::lstat(resolved_output_log_path.c_str(), &resolved_output_log_stat) == 0;
  const int resolved_output_log_errno = has_resolved_output_log ? 0 : errno;
  if (!has_resolved_output_log && resolved_output_log_errno != ENOENT) {
    return absl::InternalError(TO_STRING(
        "Failed to inspect archive log sidecar \"" << resolved_output_log_path.string()
                                                   << "\": " << std::strerror(resolved_output_log_errno)));
  }

  fs::path output_log_path = resolved_output_log_path;
  struct stat output_log_stat = resolved_output_log_stat;
  bool has_output_log = has_resolved_output_log;
  int output_log_inspection_errno = resolved_output_log_errno;
  bool resolved_log_is_guarded = false;
  if (has_resolved_output_log && S_ISREG(resolved_output_log_stat.st_mode)) {
    const fs::path resolved_guard = resolved_output_log_path.string() + ".hstream-pin";
    auto resolved_guard_stat = inspect_archive_entry(resolved_guard, "resolved archive log identity guard");
    if (!resolved_guard_stat.ok())
      return resolved_guard_stat.status();
    resolved_log_is_guarded =
        resolved_guard_stat->has_value() && same_file_identity(resolved_guard_stat->value(), resolved_output_log_stat);
  }
  if (provisional_log.has_value()) {
    struct stat provisional_log_stat{};
    const bool has_provisional_log = ::lstat(provisional_log->c_str(), &provisional_log_stat) == 0;
    const int provisional_log_errno = has_provisional_log ? 0 : errno;
    if (!has_provisional_log && provisional_log_errno != ENOENT) {
      return absl::InternalError(TO_STRING(
          "Failed to inspect provisional archive log sidecar \"" << provisional_log->string()
                                                                 << "\": " << std::strerror(provisional_log_errno)));
    }
    bool provisional_log_is_guarded = false;
    if (has_provisional_log && S_ISREG(provisional_log_stat.st_mode)) {
      const fs::path provisional_guard = provisional_log->string() + ".hstream-pin";
      auto provisional_guard_stat = inspect_archive_entry(provisional_guard, "provisional archive log identity guard");
      if (!provisional_guard_stat.ok())
        return provisional_guard_stat.status();
      provisional_log_is_guarded = provisional_guard_stat->has_value() &&
          same_file_identity(provisional_guard_stat->value(), provisional_log_stat);
    }
    if (provisional_log_is_guarded && resolved_log_is_guarded &&
        !same_file_identity(provisional_log_stat, resolved_output_log_stat)) {
      return absl::FailedPreconditionError(TO_STRING(
          "Archive recovery found distinct guarded provisional and resolved logs at \""
          << provisional_log->string() << "\" and \"" << resolved_output_log_path.string() << "\""));
    }
    if (provisional_log_is_guarded && !resolved_log_is_guarded) {
      output_log_path = *provisional_log;
      output_log_stat = provisional_log_stat;
      has_output_log = true;
      output_log_inspection_errno = 0;
    } else if (!provisional_log_is_guarded && !resolved_log_is_guarded) {
      // Versioned UI jobs always protect their active log before publishing
      // its pathname.  Do not turn an arbitrary unguarded sidecar into a
      // trusted recovery log merely because it occupies a conventional name.
      has_output_log = false;
      output_log_inspection_errno = ENOENT;
    }
  }
  if (!has_output_log && output_log_inspection_errno != ENOENT) {
    return absl::InternalError(TO_STRING(
        "Failed to inspect archive log sidecar \"" << output_log_path.string()
                                                   << "\": " << std::strerror(output_log_inspection_errno)));
  }
  if (has_output_log && !S_ISREG(output_log_stat.st_mode)) {
    return absl::FailedPreconditionError(
        TO_STRING("Archive log sidecar is not a regular file: \"" << output_log_path.string() << "\""));
  }
  int output_log_fd = -1;
  if (has_output_log) {
    output_log_fd = ::open(output_log_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat pinned_output_log_stat{};
    if (output_log_fd < 0 || ::fstat(output_log_fd, &pinned_output_log_stat) != 0 ||
        !S_ISREG(pinned_output_log_stat.st_mode) || !same_file_identity(pinned_output_log_stat, output_log_stat)) {
      if (output_log_fd >= 0)
        ::close(output_log_fd);
      return absl::FailedPreconditionError(
          TO_STRING("Archive log sidecar was replaced while being pinned: \"" << output_log_path.string() << "\""));
    }
  }
  absl::Cleanup close_output_log = [&output_log_fd]() {
    if (output_log_fd >= 0)
      ::close(output_log_fd);
  };
  const fs::path output_guard_path = output_path.string() + ".hstream-pin";
  const fs::path output_log_guard_path = output_log_path.string() + ".hstream-pin";
  auto output_guard_stat = inspect_archive_entry(output_guard_path, "archive work-file identity guard");
  if (!output_guard_stat.ok())
    return output_guard_stat.status();
  if (!output_guard_stat->has_value()) {
    HM_RETURN_IF_ERROR(create_archive_recovery_link(
        output_fd, output_path, output_stat, output_guard_path, "archive work-file identity guard"));
    output_guard_stat = std::optional<struct stat>(output_stat);
  }
  if (output_guard_stat->has_value() && !same_file_identity(output_guard_stat->value(), output_stat)) {
    return absl::FailedPreconditionError(
        TO_STRING("Archive work-file identity guard was replaced: \"" << output_guard_path.string() << "\""));
  }
  absl::StatusOr<std::optional<struct stat>> output_log_guard_stat = std::optional<struct stat>();
  if (has_output_log) {
    output_log_guard_stat = inspect_archive_entry(output_log_guard_path, "archive log identity guard");
    if (!output_log_guard_stat.ok())
      return output_log_guard_stat.status();
    if (!output_log_guard_stat->has_value()) {
      HM_RETURN_IF_ERROR(create_archive_recovery_link(
          output_log_fd, output_log_path, output_log_stat, output_log_guard_path, "archive log identity guard"));
      output_log_guard_stat = std::optional<struct stat>(output_log_stat);
    }
    if (output_log_guard_stat->has_value() && !same_file_identity(output_log_guard_stat->value(), output_log_stat)) {
      return absl::FailedPreconditionError(
          TO_STRING("Archive log identity guard was replaced: \"" << output_log_guard_path.string() << "\""));
    }
  }
  if (g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_SOURCE_BEFORE_LINK")) {
    ::unlink(output_path.c_str());
    const int replacement_fd = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (replacement_fd >= 0) {
      constexpr char kReplacement[] = "injected foreign archive source before link";
      const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
      (void)replacement_bytes;
      ::close(replacement_fd);
    }
    g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_SOURCE_BEFORE_LINK");
  }

  const auto complete_recovery = [&](const fs::path& recovery_path, const fs::path& recovery_log_path) -> absl::Status {
    const fs::path recovery_guard_path = recovery_path.string() + ".hstream-pin";
    const fs::path recovery_log_guard_path = recovery_log_path.string() + ".hstream-pin";
    HM_RETURN_IF_ERROR(sync_archive_and_parent(recovery_path, &output_stat, "archive recovery file"));
    if (has_output_log)
      HM_RETURN_IF_ERROR(sync_archive_and_parent(recovery_log_path, &output_log_stat, "archive log recovery file"));
    else
      HM_RETURN_IF_ERROR(sync_archive_and_parent(recovery_log_path, &output_stat, "archive no-log recovery marker"));

    if (g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY")) {
      ::unlink(recovery_path.c_str());
      const int replacement_fd =
          ::open(recovery_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign archive recovery";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }
    if (!has_output_log && g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RECOVERY_MARKER")) {
      ::unlink(recovery_log_path.c_str());
      const int replacement_fd =
          ::open(recovery_log_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (replacement_fd >= 0) {
        constexpr char kReplacement[] = "injected foreign archive recovery marker";
        const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
        (void)replacement_bytes;
        ::close(replacement_fd);
      }
    }

    auto published_video_stat = inspect_archive_entry(recovery_path, "published archive recovery file");
    auto published_log_stat = inspect_archive_entry(recovery_log_path, "published archive log recovery file");
    const bool published_video_is_ours = published_video_stat.ok() && published_video_stat->has_value() &&
        same_file_identity(published_video_stat->value(), output_stat);
    const struct stat& expected_published_log_stat = has_output_log ? output_log_stat : output_stat;
    const bool published_log_is_ours = published_log_stat.ok() && published_log_stat->has_value() &&
        same_file_identity(published_log_stat->value(), expected_published_log_stat);
    if (!published_video_is_ours || !published_log_is_ours) {
      if (published_video_is_ours)
        HM_RETURN_IF_ERROR(
            remove_archive_entry_if_owned(recovery_path, output_stat, "partial archive video recovery link"));
      if (published_log_is_ours) {
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_log_path,
            expected_published_log_stat,
            has_output_log ? "partial archive log recovery link" : "archive no-log recovery marker"));
      }
      if (!published_video_stat.ok())
        return published_video_stat.status();
      if (!published_log_stat.ok())
        return published_log_stat.status();
      return absl::FailedPreconditionError(TO_STRING(
          "Published archive recovery pair was replaced before source cleanup at \"" << recovery_path.string()
                                                                                     << "\""));
    }

    auto recovery_guard_stat = inspect_archive_entry(recovery_guard_path, "archive recovery identity guard");
    if (!recovery_guard_stat.ok())
      return recovery_guard_stat.status();
    if (!recovery_guard_stat->has_value()) {
      HM_RETURN_IF_ERROR(create_archive_recovery_link(
          output_fd, output_path, output_stat, recovery_guard_path, "archive recovery identity guard"));
    } else if (!same_file_identity(recovery_guard_stat->value(), output_stat)) {
      return absl::FailedPreconditionError(
          TO_STRING("Archive recovery identity guard was replaced: \"" << recovery_guard_path.string() << "\""));
    }
    if (has_output_log) {
      auto recovery_log_guard_stat =
          inspect_archive_entry(recovery_log_guard_path, "archive log recovery identity guard");
      if (!recovery_log_guard_stat.ok())
        return recovery_log_guard_stat.status();
      if (!recovery_log_guard_stat->has_value()) {
        HM_RETURN_IF_ERROR(create_archive_recovery_link(
            output_log_fd,
            output_log_path,
            output_log_stat,
            recovery_log_guard_path,
            "archive log recovery identity guard"));
      } else if (!same_file_identity(recovery_log_guard_stat->value(), output_log_stat)) {
        return absl::FailedPreconditionError(TO_STRING(
            "Archive log recovery identity guard was replaced: \"" << recovery_log_guard_path.string() << "\""));
      }
    }
    // The recovery guards are the durable transaction record once the source
    // pathnames are retired.  Persist them before crossing that boundary.
    HM_RETURN_IF_ERROR(sync_parent_directory(recovery_path));

    const absl::Status source_cleanup =
        remove_archive_entry_if_owned(output_path, output_stat, "archive work file", &recovery_path, &output_stat);
    if (!source_cleanup.ok()) {
      auto current_recovery_log = inspect_archive_entry(recovery_log_path, "partial recovery sidecar after cleanup");
      if (current_recovery_log.ok() && current_recovery_log->has_value() &&
          same_file_identity(current_recovery_log->value(), expected_published_log_stat)) {
        const absl::Status sidecar_cleanup = remove_archive_entry_if_owned(
            recovery_log_path,
            expected_published_log_stat,
            has_output_log ? "partial archive log recovery link" : "archive no-log recovery marker");
        if (!sidecar_cleanup.ok())
          return sidecar_cleanup;
      }
      auto current_recovery_guard = inspect_archive_entry(recovery_guard_path, "partial recovery identity guard");
      if (current_recovery_guard.ok() && current_recovery_guard->has_value() &&
          same_file_identity(current_recovery_guard->value(), output_stat)) {
        HM_RETURN_IF_ERROR(
            remove_archive_entry_if_owned(recovery_guard_path, output_stat, "partial archive recovery identity guard"));
      }
      if (has_output_log) {
        auto current_log_guard = inspect_archive_entry(recovery_log_guard_path, "partial log recovery identity guard");
        if (current_log_guard.ok() && current_log_guard->has_value() &&
            same_file_identity(current_log_guard->value(), output_log_stat)) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_log_guard_path, output_log_stat, "partial archive log recovery identity guard"));
        }
      }
      return source_cleanup;
    }
    HM_RETURN_IF_ERROR(sync_parent_directory(output_path));
    if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP")) {
      g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_SOURCE_CLEANUP");
      return absl::UnavailableError("archive recovery interruption requested after source cleanup");
    }
    if (has_output_log) {
      HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
          output_log_path, output_log_stat, "archive log sidecar", &recovery_log_path, &output_log_stat));
      HM_RETURN_IF_ERROR(sync_parent_directory(output_log_path));
    } else {
      HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
          recovery_log_path, output_stat, "archive no-log recovery marker", &recovery_path, &output_stat));
      HM_RETURN_IF_ERROR(sync_parent_directory(recovery_log_path));
    }
    if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_LOG_CLEANUP")) {
      g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_LOG_CLEANUP");
      return absl::UnavailableError("archive recovery interruption requested after log cleanup");
    }
    // Retire the source transaction record while the recovery guards still
    // protect both identities.  The recovery video guard is then retired
    // before its log guard, so a crash never leaves a discoverable video
    // transaction without a durable log identity.
    if (output_guard_stat->has_value()) {
      HM_RETURN_IF_ERROR(retire_archive_recovery_guards(
          recovery_path,
          output_stat,
          output_fd,
          recovery_log_path,
          has_output_log ? &output_log_stat : nullptr,
          output_log_fd,
          &output_guard_path,
          has_output_log ? &output_log_guard_path : nullptr,
          false));
    }
    if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_RECOVERY_GUARD_RETIREMENT")) {
      g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_RECOVERY_GUARD_RETIREMENT");
      return absl::UnavailableError("archive recovery interruption requested between guard-pair retirements");
    }
    HM_RETURN_IF_ERROR(retire_archive_recovery_guards(
        recovery_path,
        output_stat,
        output_fd,
        recovery_log_path,
        has_output_log ? &output_log_stat : nullptr,
        output_log_fd));
    HM_RETURN_IF_ERROR(sync_parent_directory(recovery_path));
    return absl::OkStatus();
  };

  for (int suffix = 0; suffix < 1000; ++suffix) {
    const fs::path recovery_path = archive_recovery_candidate(recovery_name_base, suffix);
    const fs::path recovery_log_path = archive_log_sidecar(recovery_path);
    const fs::path recovery_guard_path = recovery_path.string() + ".hstream-pin";
    const fs::path recovery_log_guard_path = recovery_log_path.string() + ".hstream-pin";
    auto recovery_stat = inspect_archive_entry(recovery_path, "archive recovery path");
    if (!recovery_stat.ok())
      return recovery_stat.status();
    auto recovery_log_stat = inspect_archive_entry(recovery_log_path, "archive log recovery path");
    if (!recovery_log_stat.ok())
      return recovery_log_stat.status();
    auto recovery_guard_stat = inspect_archive_entry(recovery_guard_path, "archive recovery identity guard");
    if (!recovery_guard_stat.ok())
      return recovery_guard_stat.status();
    auto recovery_log_guard_stat =
        inspect_archive_entry(recovery_log_guard_path, "archive log recovery identity guard");
    if (!recovery_log_guard_stat.ok())
      return recovery_log_guard_stat.status();
    const bool recovery_guard_is_ours =
        recovery_guard_stat->has_value() && same_file_identity(recovery_guard_stat->value(), output_stat);
    const bool recovery_log_guard_is_ours = has_output_log && recovery_log_guard_stat->has_value() &&
        same_file_identity(recovery_log_guard_stat->value(), output_log_stat);
    if ((recovery_guard_stat->has_value() && !recovery_guard_is_ours) ||
        (recovery_log_guard_stat->has_value() && !recovery_log_guard_is_ours)) {
      continue;
    }
    const auto retire_owned_candidate_guards = [&]() -> absl::Status {
      if (recovery_log_guard_is_ours) {
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_log_guard_path,
            output_log_stat,
            "superseded archive log recovery guard",
            &output_path,
            &output_stat,
            &output_log_path,
            &output_log_stat));
      }
      if (recovery_guard_is_ours) {
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_guard_path,
            output_stat,
            "superseded archive recovery guard",
            &output_path,
            &output_stat,
            has_output_log ? &output_log_path : nullptr,
            has_output_log ? &output_log_stat : nullptr));
      }
      return absl::OkStatus();
    };

    if (has_output_log) {
      const bool recovery_is_ours =
          recovery_stat->has_value() && same_file_identity(recovery_stat->value(), output_stat);
      const bool recovery_log_is_ours =
          recovery_log_stat->has_value() && same_file_identity(recovery_log_stat->value(), output_log_stat);
      const bool recovery_is_foreign = recovery_stat->has_value() && !recovery_is_ours;
      const bool recovery_log_is_foreign = recovery_log_stat->has_value() && !recovery_log_is_ours;
      if (recovery_is_foreign) {
        if (recovery_log_is_ours) {
          HM_RETURN_IF_ERROR(
              remove_archive_entry_if_owned(recovery_log_path, output_log_stat, "partial archive log recovery link"));
        }
        HM_RETURN_IF_ERROR(retire_owned_candidate_guards());
        continue;
      }
      if (recovery_log_is_foreign) {
        if (recovery_is_ours) {
          HM_RETURN_IF_ERROR(
              remove_archive_entry_if_owned(recovery_path, output_stat, "partial archive video recovery link"));
        }
        HM_RETURN_IF_ERROR(retire_owned_candidate_guards());
        continue;
      }
      if (recovery_log_is_ours && !recovery_is_ours) {
        HM_RETURN_IF_ERROR(
            remove_archive_entry_if_owned(recovery_log_path, output_log_stat, "partial archive log recovery link"));
        recovery_log_stat = std::optional<struct stat>();
      }
      if (recovery_is_ours && !recovery_log_stat->has_value()) {
        const absl::Status log_link = create_archive_recovery_link(
            output_log_fd, output_log_path, output_log_stat, recovery_log_path, "archive log sidecar");
        if (!log_link.ok()) {
          if (absl::IsAlreadyExists(log_link))
            continue;
          return log_link;
        }
      }
      if (recovery_is_ours) {
        HM_RETURN_IF_ERROR(complete_recovery(recovery_path, recovery_log_path));
        return std::optional<fs::path>(recovery_path);
      }

      const absl::Status log_link = create_archive_recovery_link(
          output_log_fd, output_log_path, output_log_stat, recovery_log_path, "archive log sidecar");
      if (!log_link.ok()) {
        if (absl::IsAlreadyExists(log_link))
          continue;
        return log_link;
      }
      const absl::Status video_link =
          create_archive_recovery_link(output_fd, output_path, output_stat, recovery_path, "archive work file");
      if (!video_link.ok()) {
        HM_RETURN_IF_ERROR(
            remove_archive_entry_if_owned(recovery_log_path, output_log_stat, "partial archive log recovery link"));
        if (absl::IsAlreadyExists(video_link))
          continue;
        return video_link;
      }
    } else {
      const bool recovery_is_ours =
          recovery_stat->has_value() && same_file_identity(recovery_stat->value(), output_stat);
      const bool recovery_log_is_marker =
          recovery_log_stat->has_value() && same_file_identity(recovery_log_stat->value(), output_stat);
      const bool recovery_is_foreign = recovery_stat->has_value() && !recovery_is_ours;
      const bool recovery_log_is_foreign = recovery_log_stat->has_value() && !recovery_log_is_marker;
      if (recovery_is_foreign) {
        if (recovery_log_is_marker) {
          HM_RETURN_IF_ERROR(
              remove_archive_entry_if_owned(recovery_log_path, output_stat, "archive no-log recovery marker"));
        }
        HM_RETURN_IF_ERROR(retire_owned_candidate_guards());
        continue;
      }
      if (recovery_log_is_foreign) {
        if (recovery_is_ours) {
          HM_RETURN_IF_ERROR(
              remove_archive_entry_if_owned(recovery_path, output_stat, "partial archive video recovery link"));
        }
        HM_RETURN_IF_ERROR(retire_owned_candidate_guards());
        continue;
      }
      if (recovery_is_ours) {
        if (!recovery_log_is_marker) {
          const absl::Status marker_link = create_archive_recovery_link(
              output_fd, output_path, output_stat, recovery_log_path, "archive no-log recovery marker");
          if (!marker_link.ok()) {
            if (absl::IsAlreadyExists(marker_link))
              continue;
            return marker_link;
          }
        }
        HM_RETURN_IF_ERROR(complete_recovery(recovery_path, recovery_log_path));
        return std::optional<fs::path>(recovery_path);
      }

      if (recovery_log_is_marker) {
        const absl::Status video_link =
            create_archive_recovery_link(output_fd, output_path, output_stat, recovery_path, "archive work file");
        if (!video_link.ok()) {
          if (absl::IsAlreadyExists(video_link))
            continue;
          return video_link;
        }
        HM_RETURN_IF_ERROR(complete_recovery(recovery_path, recovery_log_path));
        return std::optional<fs::path>(recovery_path);
      }

      const absl::Status marker_link = create_archive_recovery_link(
          output_fd, output_path, output_stat, recovery_log_path, "archive no-log recovery marker");
      if (!marker_link.ok()) {
        if (absl::IsAlreadyExists(marker_link))
          continue;
        return marker_link;
      }
      const absl::Status video_link =
          create_archive_recovery_link(output_fd, output_path, output_stat, recovery_path, "archive work file");
      if (!video_link.ok()) {
        HM_RETURN_IF_ERROR(
            remove_archive_entry_if_owned(recovery_log_path, output_stat, "archive no-log recovery marker"));
        if (absl::IsAlreadyExists(video_link))
          continue;
        return video_link;
      }
    }

    HM_RETURN_IF_ERROR(complete_recovery(recovery_path, recovery_log_path));
    return std::optional<fs::path>(recovery_path);
  }
  return absl::ResourceExhaustedError(
      TO_STRING("No recovery filename is available for existing archive \"" << output_path.string() << "\""));
}

} // namespace

absl::StatusOr<std::optional<fs::path>> configurator_internal::preserve_existing_archive_work_file(
    const fs::path& output_path) {
  return preserve_archive_work_file(output_path, output_path);
}

absl::StatusOr<fs::path> configurator_internal::reserve_unique_archive_work_file(
    const fs::path& configured_path,
    const std::string& run_id) {
  static const std::regex safe_run_id(R"(^[A-Za-z0-9_-]{1,128}$)");
  if (!std::regex_match(run_id, safe_run_id))
    return absl::InvalidArgumentError("HSTREAM_ARCHIVE_RUN_ID contains unsafe characters");
  const fs::path run_path = configured_path.parent_path() /
      (configured_path.stem().string() + ".hstream-run-" + run_id + configured_path.extension().string());
  const int reservation_fd = ::open(run_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (reservation_fd < 0) {
    const int saved_errno = errno;
    if (saved_errno == EEXIST) {
      return absl::AlreadyExistsError(TO_STRING(
          "Unique archive work path already exists; refusing to share or overwrite \"" << run_path.string() << "\""));
    }
    return absl::InternalError(TO_STRING(
        "Failed to reserve unique archive work path \"" << run_path.string() << "\": " << std::strerror(saved_errno)));
  }
  if (::close(reservation_fd) != 0) {
    const int saved_errno = errno;
    ::unlink(run_path.c_str());
    return absl::InternalError(TO_STRING(
        "Failed to close unique archive work reservation \"" << run_path.string()
                                                             << "\": " << std::strerror(saved_errno)));
  }
  return run_path;
}

fs::path configurator_internal::archive_work_owner_lock_path(const fs::path& work_path) {
  fs::path lock_path = work_path;
  lock_path += ".hstream-owner-lock";
  return lock_path;
}

absl::StatusOr<int> configurator_internal::acquire_archive_work_owner_lock(const fs::path& work_path) {
  const fs::path lock_path = archive_work_owner_lock_path(work_path);
  const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (lock_fd < 0) {
    return absl::InternalError(TO_STRING(
        "Failed to open archive work ownership lock \"" << lock_path.string() << "\": " << std::strerror(errno)));
  }
  if (::flock(lock_fd, LOCK_SH | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(lock_fd);
    return absl::InternalError(TO_STRING(
        "Failed to claim archive work ownership lock \"" << lock_path.string()
                                                         << "\": " << std::strerror(saved_errno)));
  }
  return lock_fd;
}

absl::StatusOr<int> configurator_internal::acquire_archive_output_lock(const fs::path& configured_path) {
  fs::path lock_path = configured_path;
  lock_path += ".hstream-lock";
  const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (lock_fd < 0) {
    return absl::InternalError(
        TO_STRING("Failed to open archive ownership lock \"" << lock_path.string() << "\": " << std::strerror(errno)));
  }
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(lock_fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      return absl::AlreadyExistsError(
          TO_STRING("Another HStream process owns archive output \"" << configured_path.string() << "\""));
    }
    return absl::InternalError(TO_STRING(
        "Failed to lock archive output \"" << configured_path.string() << "\": " << std::strerror(saved_errno)));
  }
  return lock_fd;
}

absl::Status configurator_internal::claim_unique_archive_output_path(
    std::map<std::string, std::string>& claimed_paths,
    const fs::path& configured_path,
    const std::string& sink_name) {
  const std::string normalized_path = configured_path.lexically_normal().string();
  const auto [existing, inserted] = claimed_paths.emplace(normalized_path, sink_name);
  if (!inserted) {
    return absl::InvalidArgumentError(TO_STRING(
        "Enabled encode sinks \"" << existing->second << "\" and \"" << sink_name
                                  << "\" resolve to the same archive output \"" << normalized_path
                                  << "\"; concurrent writers are forbidden"));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<fs::path>> configurator_internal::recover_stale_archive_work_files(
    const fs::path& configured_path) {
  std::vector<fs::path> recovered;
  const std::string prefix = configured_path.stem().string() + ".hstream-run-";
  const std::string extension = configured_path.extension().string();
  std::vector<fs::path> directory_entries;
  std::error_code iterator_error;
  for (fs::directory_iterator it(configured_path.parent_path(), iterator_error), end; it != end;
       it.increment(iterator_error)) {
    if (iterator_error) {
      return absl::InternalError(TO_STRING(
          "Failed to inspect archive work directory \"" << configured_path.parent_path().string()
                                                        << "\": " << iterator_error.message()));
    }
    directory_entries.push_back(it->path());
  }

  // Recovery guards normally remain authoritative until the older source
  // guards are retired.  If a process stops after retiring the recovery
  // guards and the visible recovery name is then lost, restore the absent
  // .hstream-run-* source name from its still-durable guard so the normal
  // stale-source pass below can preserve it again.
  const std::string source_video_guard_suffix = extension + ".hstream-pin";
  for (const fs::path& source_guard_path : directory_entries) {
    const std::string guard_name = source_guard_path.filename().string();
    if (!absl::StartsWith(guard_name, prefix) || !absl::EndsWith(guard_name, source_video_guard_suffix))
      continue;
    const std::string guard_string = source_guard_path.string();
    const fs::path source_path = guard_string.substr(0, guard_string.size() - std::strlen(".hstream-pin"));
    auto source_stat = inspect_archive_entry(source_path, "guarded interrupted archive source");
    auto source_guard_stat = inspect_archive_entry(source_guard_path, "orphan interrupted source video guard");
    if (!source_stat.ok())
      return source_stat.status();
    if (!source_guard_stat.ok())
      return source_guard_stat.status();
    if (source_stat->has_value() || !source_guard_stat->has_value() || !S_ISREG(source_guard_stat->value().st_mode) ||
        source_guard_stat->value().st_size <= 0) {
      continue;
    }

    bool matching_recovery_transaction_exists = false;
    for (const fs::path& entry : directory_entries) {
      fs::path possible_recovery = entry;
      const std::string entry_string = entry.string();
      if (absl::EndsWith(entry_string, ".hstream-pin")) {
        possible_recovery = entry_string.substr(0, entry_string.size() - std::strlen(".hstream-pin"));
      }
      if (!is_archive_recovery_path(possible_recovery, configured_path))
        continue;
      auto possible_recovery_stat = inspect_archive_entry(entry, "guarded interrupted recovery transaction");
      if (!possible_recovery_stat.ok())
        return possible_recovery_stat.status();
      if (possible_recovery_stat->has_value() &&
          same_file_identity(possible_recovery_stat->value(), source_guard_stat->value())) {
        matching_recovery_transaction_exists = true;
        break;
      }
    }
    if (matching_recovery_transaction_exists)
      continue;

    const int source_guard_fd = ::open(source_guard_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat pinned_source_guard_stat{};
    if (source_guard_fd < 0 || ::fstat(source_guard_fd, &pinned_source_guard_stat) != 0 ||
        !same_file_identity(pinned_source_guard_stat, source_guard_stat->value())) {
      if (source_guard_fd >= 0)
        ::close(source_guard_fd);
      return absl::FailedPreconditionError(
          TO_STRING("Interrupted source video guard changed at \"" << source_guard_path.string() << "\""));
    }
    const absl::Status source_link = create_archive_recovery_link(
        source_guard_fd,
        source_guard_path,
        pinned_source_guard_stat,
        source_path,
        "restored interrupted archive source");
    ::close(source_guard_fd);
    HM_RETURN_IF_ERROR(source_link);
    HM_RETURN_IF_ERROR(
        sync_archive_and_parent(source_path, &pinned_source_guard_stat, "restored interrupted archive source"));

    std::vector<fs::path> possible_log_guards{source_path.string() + ".log.hstream-pin"};
    const std::optional<fs::path> provisional_log = provisional_archive_log_sidecar(source_path, configured_path);
    if (provisional_log.has_value()) {
      const fs::path provisional_guard = provisional_log->string() + ".hstream-pin";
      if (provisional_guard != possible_log_guards.front())
        possible_log_guards.push_back(provisional_guard);
    }
    std::optional<struct stat> restored_log_stat;
    for (const fs::path& log_guard_path : possible_log_guards) {
      auto log_guard_stat = inspect_archive_entry(log_guard_path, "orphan interrupted source log guard");
      if (!log_guard_stat.ok())
        return log_guard_stat.status();
      if (!log_guard_stat->has_value())
        continue;
      if (!S_ISREG(log_guard_stat->value().st_mode)) {
        return absl::FailedPreconditionError(
            TO_STRING("Interrupted source log guard is not a regular file: \"" << log_guard_path.string() << "\""));
      }
      if (restored_log_stat.has_value() && !same_file_identity(restored_log_stat.value(), log_guard_stat->value())) {
        return absl::FailedPreconditionError(
            TO_STRING("Interrupted archive source has distinct log guards at \"" << source_path.string() << "\""));
      }
      restored_log_stat = log_guard_stat->value();
      const std::string log_guard_string = log_guard_path.string();
      const fs::path log_path = log_guard_string.substr(0, log_guard_string.size() - std::strlen(".hstream-pin"));
      auto visible_log = inspect_archive_entry(log_path, "restored interrupted source log");
      if (!visible_log.ok())
        return visible_log.status();
      if (visible_log->has_value()) {
        if (!same_file_identity(visible_log->value(), log_guard_stat->value())) {
          return absl::FailedPreconditionError(
              TO_STRING("Interrupted source log path is occupied at \"" << log_path.string() << "\""));
        }
        continue;
      }
      const int log_guard_fd = ::open(log_guard_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      struct stat pinned_log_guard_stat{};
      if (log_guard_fd < 0 || ::fstat(log_guard_fd, &pinned_log_guard_stat) != 0 ||
          !same_file_identity(pinned_log_guard_stat, log_guard_stat->value())) {
        if (log_guard_fd >= 0)
          ::close(log_guard_fd);
        return absl::FailedPreconditionError(
            TO_STRING("Interrupted source log guard changed at \"" << log_guard_path.string() << "\""));
      }
      const absl::Status log_link = create_archive_recovery_link(
          log_guard_fd, log_guard_path, pinned_log_guard_stat, log_path, "restored interrupted archive source log");
      ::close(log_guard_fd);
      HM_RETURN_IF_ERROR(log_link);
      HM_RETURN_IF_ERROR(
          sync_archive_and_parent(log_path, &pinned_log_guard_stat, "restored interrupted archive source log"));
    }
  }

  // Once a source pathname is retired, matching recovery guards are the
  // transaction record.  Discover a transaction from either its visible
  // video or its video guard: a crash or later pathname replacement can leave
  // the guard as the only surviving trusted name.
  std::set<fs::path> recovery_path_set;
  for (const fs::path& entry : directory_entries) {
    if (is_archive_recovery_path(entry, configured_path)) {
      recovery_path_set.insert(entry);
      continue;
    }
    constexpr absl::string_view kGuardSuffix = ".hstream-pin";
    const std::string entry_string = entry.string();
    if (!absl::EndsWith(entry_string, kGuardSuffix))
      continue;
    const fs::path guarded_path = entry_string.substr(0, entry_string.size() - kGuardSuffix.size());
    if (is_archive_recovery_path(guarded_path, configured_path))
      recovery_path_set.insert(guarded_path);
  }
  std::vector<fs::path> recovery_paths(recovery_path_set.begin(), recovery_path_set.end());
  const std::string recovery_stem_prefix = configured_path.stem().string() + "-finalization-failed";
  std::sort(recovery_paths.begin(), recovery_paths.end(), [&](const fs::path& left, const fs::path& right) {
    const auto recovery_index = [&](const fs::path& path) {
      const std::string suffix = path.stem().string().substr(recovery_stem_prefix.size());
      return suffix.empty() ? uint64_t{0} : std::strtoull(suffix.c_str() + 1, nullptr, 10) + 1;
    };
    return recovery_index(left) < recovery_index(right);
  });

  // Never infer ownership from an unguarded visible recovery pair, and use
  // the guards to rescue a pair whose visible names were replaced while the
  // previous process was committing it.
  for (const fs::path& recovery_path : recovery_paths) {
    const fs::path recovery_log_path = archive_log_sidecar(recovery_path);
    const fs::path recovery_guard_path = recovery_path.string() + ".hstream-pin";
    const fs::path recovery_log_guard_path = recovery_log_path.string() + ".hstream-pin";
    auto recovery_guard_stat = inspect_archive_entry(recovery_guard_path, "interrupted archive recovery guard");
    if (!recovery_guard_stat.ok())
      return recovery_guard_stat.status();
    auto recovery_log_guard_stat =
        inspect_archive_entry(recovery_log_guard_path, "interrupted archive log recovery guard");
    if (!recovery_log_guard_stat.ok())
      return recovery_log_guard_stat.status();
    if (!recovery_guard_stat->has_value() || !recovery_log_guard_stat->has_value()) {
      // A process can stop while retiring either guard pair.  Recreate every
      // missing recovery identity from the still-durable source pair before
      // validating the visible recovery names below.  In particular, do not
      // interpret a surviving video guard as a no-log transaction merely
      // because its log guard was the last unlink persisted before a crash.
      auto visible_video = inspect_archive_entry(recovery_path, "unguarded interrupted archive recovery file");
      auto visible_log = inspect_archive_entry(recovery_log_path, "unguarded interrupted archive recovery log");
      if (!visible_video.ok())
        return visible_video.status();
      if (!visible_log.ok())
        return visible_log.status();

      const std::string source_video_guard_suffix = extension + ".hstream-pin";
      for (const fs::path& possible_source_guard : directory_entries) {
        const std::string guard_name = possible_source_guard.filename().string();
        if (!absl::StartsWith(guard_name, prefix) || !absl::EndsWith(guard_name, source_video_guard_suffix))
          continue;
        auto source_guard_stat = inspect_archive_entry(possible_source_guard, "interrupted source video guard");
        if (!source_guard_stat.ok())
          return source_guard_stat.status();
        if (!source_guard_stat->has_value() || !S_ISREG(source_guard_stat->value().st_mode) ||
            source_guard_stat->value().st_size <= 0) {
          continue;
        }
        if (recovery_guard_stat->has_value() &&
            !same_file_identity(recovery_guard_stat->value(), source_guard_stat->value())) {
          continue;
        }

        const std::string source_guard_string = possible_source_guard.string();
        const fs::path source_path =
            source_guard_string.substr(0, source_guard_string.size() - std::strlen(".hstream-pin"));
        std::vector<fs::path> possible_log_guards{source_path.string() + ".log.hstream-pin"};
        const std::optional<fs::path> provisional_log = provisional_archive_log_sidecar(source_path, configured_path);
        if (provisional_log.has_value()) {
          const fs::path provisional_guard = provisional_log->string() + ".hstream-pin";
          if (provisional_guard != possible_log_guards.front())
            possible_log_guards.push_back(provisional_guard);
        }

        std::optional<fs::path> source_log_guard_path;
        std::optional<struct stat> source_log_guard_stat;
        for (const fs::path& possible_log_guard : possible_log_guards) {
          auto possible_log_stat = inspect_archive_entry(possible_log_guard, "interrupted source log guard");
          if (!possible_log_stat.ok())
            return possible_log_stat.status();
          if (!possible_log_stat->has_value())
            continue;
          if (!S_ISREG(possible_log_stat->value().st_mode)) {
            return absl::FailedPreconditionError(TO_STRING(
                "Interrupted source log guard is not a regular file: \"" << possible_log_guard.string() << "\""));
          }
          if (source_log_guard_stat.has_value() &&
              !same_file_identity(source_log_guard_stat.value(), possible_log_stat->value())) {
            return absl::FailedPreconditionError(TO_STRING(
                "Interrupted archive recovery has distinct source log guards for \"" << source_path.string() << "\""));
          }
          if (!source_log_guard_stat.has_value()) {
            source_log_guard_path = possible_log_guard;
            source_log_guard_stat = possible_log_stat->value();
          }
        }
        const bool visible_video_matches =
            visible_video->has_value() && same_file_identity(source_guard_stat->value(), visible_video->value());
        const bool visible_log_matches = source_log_guard_stat.has_value() && visible_log->has_value() &&
            same_file_identity(source_log_guard_stat.value(), visible_log->value());
        const bool recovery_video_matches = recovery_guard_stat->has_value() &&
            same_file_identity(recovery_guard_stat->value(), source_guard_stat->value());
        if (!visible_video_matches && !visible_log_matches && !recovery_video_matches) {
          continue;
        }
        if (recovery_log_guard_stat->has_value() &&
            (!source_log_guard_stat.has_value() ||
             !same_file_identity(recovery_log_guard_stat->value(), source_log_guard_stat.value()))) {
          return absl::FailedPreconditionError(TO_STRING(
              "Interrupted archive recovery has mismatched recovery and source log guards at \""
              << recovery_log_guard_path.string() << "\""));
        }

        int source_log_guard_fd = -1;
        if (source_log_guard_path.has_value() && !recovery_log_guard_stat->has_value()) {
          source_log_guard_fd = ::open(source_log_guard_path->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
          struct stat pinned_source_log_guard_stat{};
          if (source_log_guard_fd < 0 || ::fstat(source_log_guard_fd, &pinned_source_log_guard_stat) != 0 ||
              !same_file_identity(pinned_source_log_guard_stat, source_log_guard_stat.value())) {
            if (source_log_guard_fd >= 0)
              ::close(source_log_guard_fd);
            return absl::FailedPreconditionError(
                TO_STRING("Interrupted source log guard changed at \"" << source_log_guard_path->string() << "\""));
          }
          const absl::Status log_guard_link = create_archive_recovery_link(
              source_log_guard_fd,
              *source_log_guard_path,
              source_log_guard_stat.value(),
              recovery_log_guard_path,
              "restored archive log recovery guard");
          ::close(source_log_guard_fd);
          HM_RETURN_IF_ERROR(log_guard_link);
          recovery_log_guard_stat = source_log_guard_stat;
        }

        if (!recovery_guard_stat->has_value()) {
          const int source_guard_fd =
              ::open(possible_source_guard.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
          struct stat pinned_source_guard_stat{};
          if (source_guard_fd < 0 || ::fstat(source_guard_fd, &pinned_source_guard_stat) != 0 ||
              !same_file_identity(pinned_source_guard_stat, source_guard_stat->value())) {
            if (source_guard_fd >= 0)
              ::close(source_guard_fd);
            return absl::FailedPreconditionError(
                TO_STRING("Interrupted source video guard changed at \"" << possible_source_guard.string() << "\""));
          }
          const absl::Status video_guard_link = create_archive_recovery_link(
              source_guard_fd,
              possible_source_guard,
              source_guard_stat->value(),
              recovery_guard_path,
              "restored archive recovery guard");
          ::close(source_guard_fd);
          HM_RETURN_IF_ERROR(video_guard_link);
          recovery_guard_stat = source_guard_stat;
        }
        HM_RETURN_IF_ERROR(sync_parent_directory(recovery_path));
        break;
      }
    }
    if (!recovery_guard_stat->has_value() && recovery_log_guard_stat->has_value() &&
        S_ISREG(recovery_log_guard_stat->value().st_mode)) {
      // The video guard is retired first.  A lone log guard therefore records
      // a committed visible pair whose final guard cleanup was interrupted.
      // It can also be the superseded half of a rescue transaction; in that
      // case, prefer the other fully guarded pair and retire only this guard.
      const struct stat retired_log_stat = recovery_log_guard_stat->value();
      bool superseded_by_guarded_pair = false;
      for (const fs::path& other_path : recovery_paths) {
        if (other_path == recovery_path)
          continue;
        const fs::path other_log_path = archive_log_sidecar(other_path);
        const fs::path other_guard_path = other_path.string() + ".hstream-pin";
        const fs::path other_log_guard_path = other_log_path.string() + ".hstream-pin";
        auto other_video = inspect_archive_entry(other_path, "completed guarded recovery video");
        auto other_video_guard = inspect_archive_entry(other_guard_path, "completed recovery video guard");
        auto other_log = inspect_archive_entry(other_log_path, "completed guarded recovery log");
        auto other_log_guard = inspect_archive_entry(other_log_guard_path, "completed recovery log guard");
        if (!other_video.ok())
          return other_video.status();
        if (!other_video_guard.ok())
          return other_video_guard.status();
        if (!other_log.ok())
          return other_log.status();
        if (!other_log_guard.ok())
          return other_log_guard.status();
        if (!other_video->has_value() || !other_video_guard->has_value() ||
            !same_file_identity(other_video->value(), other_video_guard->value()) || !other_log->has_value() ||
            !other_log_guard->has_value() || !same_file_identity(other_log->value(), retired_log_stat) ||
            !same_file_identity(other_log_guard->value(), retired_log_stat)) {
          continue;
        }
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_log_guard_path,
            retired_log_stat,
            "superseded retired archive log guard",
            &other_path,
            &other_video_guard->value(),
            &other_log_path,
            &retired_log_stat));
        superseded_by_guarded_pair = true;
        break;
      }
      if (superseded_by_guarded_pair)
        continue;

      auto visible_video = inspect_archive_entry(recovery_path, "committed recovery video after guard retirement");
      auto visible_log = inspect_archive_entry(recovery_log_path, "committed recovery log after guard retirement");
      if (!visible_video.ok())
        return visible_video.status();
      if (!visible_log.ok())
        return visible_log.status();
      if (visible_video->has_value() && S_ISREG(visible_video->value().st_mode) && visible_video->value().st_size > 0 &&
          visible_log->has_value() && same_file_identity(visible_log->value(), retired_log_stat)) {
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_log_guard_path,
            retired_log_stat,
            "retired archive log recovery guard",
            &recovery_log_path,
            &retired_log_stat));
        recovered.push_back(recovery_path);
        continue;
      }
    }
    if (!recovery_guard_stat->has_value() || !S_ISREG(recovery_guard_stat->value().st_mode) ||
        recovery_guard_stat->value().st_size <= 0) {
      continue;
    }
    const struct stat trusted_video_stat = recovery_guard_stat->value();
    const bool has_trusted_log = recovery_log_guard_stat->has_value();
    if (has_trusted_log && !S_ISREG(recovery_log_guard_stat->value().st_mode))
      continue;
    struct stat trusted_log_stat{};
    if (has_trusted_log)
      trusted_log_stat = recovery_log_guard_stat->value();

    bool source_video_still_exists = false;
    for (const fs::path& possible_source : directory_entries) {
      const std::string source_name = possible_source.filename().string();
      if (!absl::StartsWith(source_name, prefix) || !absl::EndsWith(source_name, extension))
        continue;
      auto source_stat = inspect_archive_entry(possible_source, "active interrupted archive source");
      if (!source_stat.ok())
        return source_stat.status();
      if (source_stat->has_value() && same_file_identity(source_stat->value(), trusted_video_stat)) {
        source_video_still_exists = true;
        break;
      }
    }
    if (source_video_still_exists)
      continue;

    const int trusted_video_fd = ::open(recovery_guard_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat pinned_video_stat{};
    if (trusted_video_fd < 0 || ::fstat(trusted_video_fd, &pinned_video_stat) != 0 ||
        !same_file_identity(pinned_video_stat, trusted_video_stat)) {
      if (trusted_video_fd >= 0)
        ::close(trusted_video_fd);
      return absl::FailedPreconditionError(
          TO_STRING("Interrupted archive recovery guard changed at \"" << recovery_guard_path.string() << "\""));
    }
    absl::Cleanup close_trusted_video = [trusted_video_fd]() { ::close(trusted_video_fd); };
    int trusted_log_fd = -1;
    if (has_trusted_log) {
      trusted_log_fd = ::open(recovery_log_guard_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      struct stat pinned_log_stat{};
      if (trusted_log_fd < 0 || ::fstat(trusted_log_fd, &pinned_log_stat) != 0 ||
          !same_file_identity(pinned_log_stat, trusted_log_stat)) {
        if (trusted_log_fd >= 0)
          ::close(trusted_log_fd);
        return absl::FailedPreconditionError(TO_STRING(
            "Interrupted archive log recovery guard changed at \"" << recovery_log_guard_path.string() << "\""));
      }
    }
    absl::Cleanup close_trusted_log = [&trusted_log_fd]() {
      if (trusted_log_fd >= 0)
        ::close(trusted_log_fd);
    };

    auto visible_video = inspect_archive_entry(recovery_path, "interrupted archive recovery file");
    auto visible_log = inspect_archive_entry(recovery_log_path, "interrupted archive recovery log");
    if (!visible_video.ok())
      return visible_video.status();
    if (!visible_log.ok())
      return visible_log.status();
    const bool visible_video_is_trusted =
        visible_video->has_value() && same_file_identity(visible_video->value(), trusted_video_stat);
    const bool visible_log_is_trusted = has_trusted_log
        ? (visible_log->has_value() && same_file_identity(visible_log->value(), trusted_log_stat))
        : !visible_log->has_value() || same_file_identity(visible_log->value(), trusted_video_stat);

    bool duplicate_committed_elsewhere = false;
    if (!visible_video_is_trusted || !visible_log_is_trusted) {
      for (const fs::path& other_path : recovery_paths) {
        if (other_path == recovery_path)
          continue;
        const fs::path other_log_path = archive_log_sidecar(other_path);
        const fs::path other_guard_path = other_path.string() + ".hstream-pin";
        const fs::path other_log_guard_path = other_log_path.string() + ".hstream-pin";
        auto other_guard = inspect_archive_entry(other_guard_path, "duplicate archive recovery guard");
        auto other_visible_video = inspect_archive_entry(other_path, "duplicate archive recovery file");
        auto other_log_guard = inspect_archive_entry(other_log_guard_path, "duplicate archive log recovery guard");
        auto other_visible_log = inspect_archive_entry(other_log_path, "duplicate archive recovery log");
        if (!other_guard.ok())
          return other_guard.status();
        if (!other_visible_video.ok())
          return other_visible_video.status();
        if (!other_log_guard.ok())
          return other_log_guard.status();
        if (!other_visible_log.ok())
          return other_visible_log.status();
        if (!other_guard->has_value() || !other_visible_video->has_value() ||
            !same_file_identity(other_guard->value(), trusted_video_stat) ||
            !same_file_identity(other_visible_video->value(), trusted_video_stat)) {
          continue;
        }

        std::optional<struct stat> duplicate_log_stat;
        if (has_trusted_log) {
          if (!other_log_guard->has_value() || !other_visible_log->has_value() ||
              !same_file_identity(other_log_guard->value(), trusted_log_stat) ||
              !same_file_identity(other_visible_log->value(), trusted_log_stat)) {
            continue;
          }
          duplicate_log_stat = trusted_log_stat;
        } else if (visible_log->has_value() && !same_file_identity(visible_log->value(), trusted_video_stat)) {
          if (!other_log_guard->has_value() || !other_visible_log->has_value() ||
              !same_file_identity(other_log_guard->value(), visible_log->value()) ||
              !same_file_identity(other_visible_log->value(), visible_log->value())) {
            continue;
          }
          duplicate_log_stat = visible_log->value();
        } else if (other_log_guard->has_value() || other_visible_log->has_value()) {
          continue;
        }

        if (visible_log->has_value() &&
            ((duplicate_log_stat.has_value() && same_file_identity(visible_log->value(), duplicate_log_stat.value())) ||
             (!duplicate_log_stat.has_value() && same_file_identity(visible_log->value(), trusted_video_stat)))) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_log_path,
              visible_log->value(),
              "superseded duplicate archive recovery log",
              &other_path,
              &trusted_video_stat,
              duplicate_log_stat.has_value() ? &other_log_path : nullptr,
              duplicate_log_stat.has_value() ? &duplicate_log_stat.value() : nullptr));
        }
        if (visible_video_is_trusted) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_path,
              trusted_video_stat,
              "superseded duplicate archive recovery file",
              &other_path,
              &trusted_video_stat,
              duplicate_log_stat.has_value() ? &other_log_path : nullptr,
              duplicate_log_stat.has_value() ? &duplicate_log_stat.value() : nullptr));
        }
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_guard_path,
            trusted_video_stat,
            "superseded duplicate archive recovery guard",
            &other_path,
            &trusted_video_stat,
            duplicate_log_stat.has_value() ? &other_log_path : nullptr,
            duplicate_log_stat.has_value() ? &duplicate_log_stat.value() : nullptr));
        if (has_trusted_log) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_log_guard_path,
              trusted_log_stat,
              "superseded duplicate archive log recovery guard",
              &other_path,
              &trusted_video_stat,
              &other_log_path,
              &trusted_log_stat));
        }
        duplicate_committed_elsewhere = true;
        break;
      }
    }
    if (duplicate_committed_elsewhere)
      continue;

    fs::path committed_path = recovery_path;
    fs::path committed_log_path = recovery_log_path;
    if (!visible_video_is_trusted || !visible_log_is_trusted) {
      bool rescued = false;
      for (int suffix = 0; suffix < 1000; ++suffix) {
        const fs::path candidate = archive_recovery_candidate(configured_path, suffix);
        const fs::path candidate_log = archive_log_sidecar(candidate);
        const fs::path candidate_guard = candidate.string() + ".hstream-pin";
        const fs::path candidate_log_guard = candidate_log.string() + ".hstream-pin";
        auto candidate_stat = inspect_archive_entry(candidate, "archive recovery rescue path");
        auto candidate_log_stat = inspect_archive_entry(candidate_log, "archive log recovery rescue path");
        auto candidate_guard_stat = inspect_archive_entry(candidate_guard, "archive recovery rescue guard");
        auto candidate_log_guard_stat = inspect_archive_entry(candidate_log_guard, "archive log rescue guard");
        if (!candidate_stat.ok())
          return candidate_stat.status();
        if (!candidate_log_stat.ok())
          return candidate_log_stat.status();
        if (!candidate_guard_stat.ok())
          return candidate_guard_stat.status();
        if (!candidate_log_guard_stat.ok())
          return candidate_log_guard_stat.status();
        const bool candidate_is_ours =
            candidate_stat->has_value() && same_file_identity(candidate_stat->value(), trusted_video_stat);
        const bool candidate_guard_is_ours =
            candidate_guard_stat->has_value() && same_file_identity(candidate_guard_stat->value(), trusted_video_stat);
        const bool candidate_log_is_ours = has_trusted_log && candidate_log_stat->has_value() &&
            same_file_identity(candidate_log_stat->value(), trusted_log_stat);
        const bool candidate_log_guard_is_ours = has_trusted_log && candidate_log_guard_stat->has_value() &&
            same_file_identity(candidate_log_guard_stat->value(), trusted_log_stat);
        const bool candidate_is_foreign = candidate_stat->has_value() && !candidate_is_ours;
        const bool candidate_guard_is_foreign = candidate_guard_stat->has_value() && !candidate_guard_is_ours;
        const bool candidate_log_is_foreign =
            candidate_log_stat->has_value() && (!has_trusted_log || !candidate_log_is_ours);
        const bool candidate_log_guard_is_foreign =
            candidate_log_guard_stat->has_value() && (!has_trusted_log || !candidate_log_guard_is_ours);
        if (candidate_is_foreign || candidate_guard_is_foreign || candidate_log_is_foreign ||
            candidate_log_guard_is_foreign) {
          continue;
        }

        if (has_trusted_log && !candidate_log_guard_is_ours) {
          HM_RETURN_IF_ERROR(create_archive_recovery_link(
              trusted_log_fd,
              recovery_log_guard_path,
              trusted_log_stat,
              candidate_log_guard,
              "rescued log recovery guard"));
          if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_GUARD")) {
            g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_GUARD");
            return absl::UnavailableError("archive recovery interruption requested after rescue log guard");
          }
        }
        if (!candidate_guard_is_ours) {
          HM_RETURN_IF_ERROR(create_archive_recovery_link(
              trusted_video_fd, recovery_guard_path, trusted_video_stat, candidate_guard, "rescued recovery guard"));
          if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_GUARD")) {
            g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_GUARD");
            return absl::UnavailableError("archive recovery interruption requested after rescue video guard");
          }
        }
        // The video guard is the transaction-discovery record.  Persist it
        // only after the optional log guard exists, then fill the visible
        // names.  Restart can safely complete any partial owned candidate.
        HM_RETURN_IF_ERROR(sync_parent_directory(candidate));
        if (has_trusted_log && !candidate_log_is_ours) {
          HM_RETURN_IF_ERROR(create_archive_recovery_link(
              trusted_log_fd,
              recovery_log_guard_path,
              trusted_log_stat,
              candidate_log,
              "rescued archive recovery log"));
          if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_LINK")) {
            g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_LOG_LINK");
            return absl::UnavailableError("archive recovery interruption requested after rescue log link");
          }
        }
        if (!candidate_is_ours) {
          HM_RETURN_IF_ERROR(create_archive_recovery_link(
              trusted_video_fd, recovery_guard_path, trusted_video_stat, candidate, "rescued archive recovery file"));
          if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_LINK")) {
            g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_RESCUE_VIDEO_LINK");
            return absl::UnavailableError("archive recovery interruption requested after rescue video link");
          }
        }
        HM_RETURN_IF_ERROR(sync_archive_and_parent(candidate, &trusted_video_stat, "rescued archive recovery file"));
        if (has_trusted_log)
          HM_RETURN_IF_ERROR(sync_archive_and_parent(candidate_log, &trusted_log_stat, "rescued archive recovery log"));
        HM_RETURN_IF_ERROR(sync_parent_directory(candidate));
        if (!has_trusted_log && visible_log->has_value() &&
            same_file_identity(visible_log->value(), trusted_video_stat)) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_log_path,
              trusted_video_stat,
              "superseded archive no-log recovery marker",
              &candidate,
              &trusted_video_stat));
        }
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            recovery_guard_path,
            trusted_video_stat,
            "superseded archive recovery guard",
            &candidate,
            &trusted_video_stat,
            has_trusted_log ? &candidate_log : nullptr,
            has_trusted_log ? &trusted_log_stat : nullptr));
        if (has_trusted_log) {
          if (g_getenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_BETWEEN_SUPERSEDED_GUARD_REMOVALS")) {
            g_unsetenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_BETWEEN_SUPERSEDED_GUARD_REMOVALS");
            return absl::UnavailableError("archive recovery interruption requested between superseded guard removals");
          }
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              recovery_log_guard_path,
              trusted_log_stat,
              "superseded archive log recovery guard",
              &candidate,
              &trusted_video_stat,
              &candidate_log,
              &trusted_log_stat));
        }
        committed_path = candidate;
        committed_log_path = candidate_log;
        rescued = true;
        break;
      }
      if (!rescued) {
        return absl::ResourceExhaustedError(TO_STRING(
            "Could not rescue interrupted archive recovery guarded at \"" << recovery_guard_path.string() << "\""));
      }
    } else if (!has_trusted_log && visible_log->has_value()) {
      HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
          recovery_log_path,
          trusted_video_stat,
          "completed archive no-log recovery marker",
          &recovery_path,
          &trusted_video_stat));
    }

    if (has_trusted_log) {
      const std::string source_log_suffix = extension + ".log";
      for (const fs::path& possible_source_log : directory_entries) {
        const std::string source_log_name = possible_source_log.filename().string();
        if (!absl::StartsWith(source_log_name, prefix) || !absl::EndsWith(source_log_name, source_log_suffix))
          continue;
        auto source_log_stat = inspect_archive_entry(possible_source_log, "interrupted archive source log");
        if (!source_log_stat.ok())
          return source_log_stat.status();
        if (source_log_stat->has_value() && same_file_identity(source_log_stat->value(), trusted_log_stat)) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              possible_source_log,
              trusted_log_stat,
              "interrupted archive source log",
              &committed_path,
              &trusted_video_stat,
              &committed_log_path,
              &trusted_log_stat));
        }
      }
    }
    if (has_trusted_log) {
      const std::string source_log_guard_suffix = extension + ".log.hstream-pin";
      for (const fs::path& possible_source_log_guard : directory_entries) {
        const std::string guard_name = possible_source_log_guard.filename().string();
        if (!absl::StartsWith(guard_name, prefix) || !absl::EndsWith(guard_name, source_log_guard_suffix))
          continue;
        auto source_log_guard = inspect_archive_entry(possible_source_log_guard, "interrupted source log guard");
        if (!source_log_guard.ok())
          return source_log_guard.status();
        if (source_log_guard->has_value() && same_file_identity(source_log_guard->value(), trusted_log_stat)) {
          HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
              possible_source_log_guard,
              trusted_log_stat,
              "interrupted source log guard",
              &committed_path,
              &trusted_video_stat,
              &committed_log_path,
              &trusted_log_stat));
        }
      }
    }
    const std::string source_video_guard_suffix = extension + ".hstream-pin";
    for (const fs::path& possible_source_guard : directory_entries) {
      const std::string guard_name = possible_source_guard.filename().string();
      if (!absl::StartsWith(guard_name, prefix) || !absl::EndsWith(guard_name, source_video_guard_suffix))
        continue;
      auto source_guard = inspect_archive_entry(possible_source_guard, "interrupted source video guard");
      if (!source_guard.ok())
        return source_guard.status();
      if (source_guard->has_value() && same_file_identity(source_guard->value(), trusted_video_stat)) {
        HM_RETURN_IF_ERROR(remove_archive_entry_if_owned(
            possible_source_guard,
            trusted_video_stat,
            "interrupted source video guard",
            &committed_path,
            &trusted_video_stat,
            has_trusted_log ? &committed_log_path : nullptr,
            has_trusted_log ? &trusted_log_stat : nullptr));
      }
    }
    HM_RETURN_IF_ERROR(retire_archive_recovery_guards(
        committed_path,
        trusted_video_stat,
        trusted_video_fd,
        committed_log_path,
        has_trusted_log ? &trusted_log_stat : nullptr,
        trusted_log_fd));
    recovered.push_back(committed_path);
  }

  iterator_error.clear();
  for (fs::directory_iterator it(configured_path.parent_path(), iterator_error), end; it != end;
       it.increment(iterator_error)) {
    if (iterator_error) {
      return absl::InternalError(TO_STRING(
          "Failed to inspect archive work directory \"" << configured_path.parent_path().string()
                                                        << "\": " << iterator_error.message()));
    }
    const fs::path candidate = it->path();
    const std::string filename = candidate.filename().string();
    if (filename.size() <= prefix.size() + extension.size() || !absl::StartsWith(filename, prefix) ||
        !absl::EndsWith(filename, extension)) {
      continue;
    }

    const std::string ownership = filename.substr(prefix.size(), filename.size() - prefix.size() - extension.size());
    bool owner_is_live = false;
    const bool has_v3_ownership = absl::StartsWith(ownership, "v3-");
    int recovery_lock_fd = -1;
    struct stat recovery_lock_stat{};
    bool have_recovery_lock_identity = false;
    bool recovery_lock_is_absent = false;
    fs::path recovery_lock_path;
    if (has_v3_ownership) {
      recovery_lock_path = archive_work_owner_lock_path(candidate);
      recovery_lock_fd = ::open(recovery_lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
      if (recovery_lock_fd >= 0) {
        have_recovery_lock_identity =
            ::fstat(recovery_lock_fd, &recovery_lock_stat) == 0 && S_ISREG(recovery_lock_stat.st_mode);
        if (!have_recovery_lock_identity) {
          const int saved_errno = errno;
          ::close(recovery_lock_fd);
          return absl::InternalError(TO_STRING(
              "Failed to pin archive work ownership lock \"" << recovery_lock_path.string()
                                                             << "\": " << std::strerror(saved_errno)));
        }
        if (::flock(recovery_lock_fd, LOCK_EX | LOCK_NB) != 0) {
          const int saved_errno = errno;
          ::close(recovery_lock_fd);
          recovery_lock_fd = -1;
          if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            owner_is_live = true;
          } else {
            return absl::InternalError(TO_STRING(
                "Failed to inspect archive work ownership lock \"" << recovery_lock_path.string()
                                                                   << "\": " << std::strerror(saved_errno)));
          }
        } else {
          std::smatch versioned_match;
          static const std::regex versioned_backend_and_ui_owner(
              R"(^v3-[0-9]+-([0-9]+)-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
          if (std::regex_match(ownership, versioned_match, versioned_backend_and_ui_owner)) {
            const long long ui_pid = std::strtoll(versioned_match[1].str().c_str(), nullptr, 10);
            if (ui_pid > 0 && ui_pid <= std::numeric_limits<pid_t>::max())
              owner_is_live = ::kill(static_cast<pid_t>(ui_pid), 0) == 0 || errno == EPERM;
          }
        }
      } else {
        recovery_lock_is_absent = errno == ENOENT;
        if (!recovery_lock_is_absent) {
          return absl::InternalError(TO_STRING(
              "Failed to open archive work ownership lock \"" << recovery_lock_path.string()
                                                              << "\": " << std::strerror(errno)));
        }
      }
    } else {
      std::smatch ownership_match;
      static const std::regex versioned_backend_and_ui_owner(
          R"(^v2-([0-9]+)-([0-9]+)-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
      static const std::regex pre_version_backend_and_ui_owner(
          R"(^([0-9]+)-([0-9]+)-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
      static const std::regex legacy_ui_owner(
          R"(^([0-9]+)-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)");
      static const std::regex fallback_owner(R"(^([0-9]+)(?:-|$).*$)");
      const bool has_backend_and_ui = std::regex_match(ownership, ownership_match, versioned_backend_and_ui_owner) ||
          std::regex_match(ownership, ownership_match, pre_version_backend_and_ui_owner);
      if (has_backend_and_ui || std::regex_match(ownership, ownership_match, legacy_ui_owner) ||
          std::regex_match(ownership, ownership_match, fallback_owner)) {
        const long long parsed_pid = std::strtoll(ownership_match[1].str().c_str(), nullptr, 10);
        if (parsed_pid > 0 && parsed_pid <= std::numeric_limits<pid_t>::max())
          owner_is_live = ::kill(static_cast<pid_t>(parsed_pid), 0) == 0 || errno == EPERM;
        if (!owner_is_live && has_backend_and_ui) {
          const long long ui_pid = std::strtoll(ownership_match[2].str().c_str(), nullptr, 10);
          if (ui_pid > 0 && ui_pid <= std::numeric_limits<pid_t>::max())
            owner_is_live = ::kill(static_cast<pid_t>(ui_pid), 0) == 0 || errno == EPERM;
        }
      }
    }

    struct stat candidate_stat{};
    if (::lstat(candidate.c_str(), &candidate_stat) != 0) {
      const int saved_errno = errno;
      if (recovery_lock_fd >= 0)
        ::close(recovery_lock_fd);
      if (saved_errno == ENOENT)
        continue;
      return absl::InternalError(TO_STRING(
          "Failed to inspect stale archive work file \"" << candidate.string()
                                                         << "\": " << std::strerror(saved_errno)));
    }
    if (has_v3_ownership && (recovery_lock_fd >= 0 || recovery_lock_is_absent) && S_ISREG(candidate_stat.st_mode) &&
        candidate_stat.st_size == 0) {
      int cleanup_errno = 0;
      if (g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RESERVATION_BEFORE_CLEANUP")) {
        ::unlink(candidate.c_str());
        const int replacement_fd =
            ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (replacement_fd >= 0) {
          constexpr char kReplacement[] = "injected foreign archive reservation";
          const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
          (void)replacement_bytes;
          ::close(replacement_fd);
        }
        g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_RESERVATION_BEFORE_CLEANUP");
      }
      const absl::Status candidate_cleanup =
          remove_archive_entry_if_owned(candidate, candidate_stat, "abandoned archive work reservation");
      if (!candidate_cleanup.ok()) {
        if (recovery_lock_fd >= 0)
          ::close(recovery_lock_fd);
        recovery_lock_fd = -1;
        return candidate_cleanup;
      }
      absl::Status lock_cleanup = absl::OkStatus();
      if (have_recovery_lock_identity) {
        if (g_getenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_OWNER_LOCK")) {
          ::unlink(recovery_lock_path.c_str());
          const int replacement_fd =
              ::open(recovery_lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
          if (replacement_fd >= 0) {
            constexpr char kReplacement[] = "injected foreign archive owner lock";
            const ssize_t replacement_bytes = ::write(replacement_fd, kReplacement, sizeof(kReplacement) - 1);
            (void)replacement_bytes;
            ::close(replacement_fd);
          }
          g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_OWNER_LOCK");
        }
        lock_cleanup =
            remove_archive_entry_if_owned(recovery_lock_path, recovery_lock_stat, "archive work ownership lock");
      }
      if (recovery_lock_fd >= 0 && ::close(recovery_lock_fd) != 0 && cleanup_errno == 0)
        cleanup_errno = errno;
      recovery_lock_fd = -1;
      HM_RETURN_IF_ERROR(sync_parent_directory(candidate));
      if (!lock_cleanup.ok())
        return lock_cleanup;
      if (cleanup_errno != 0) {
        return absl::InternalError(TO_STRING(
            "Failed to remove abandoned archive work reservation \"" << candidate.string()
                                                                     << "\": " << std::strerror(cleanup_errno)));
      }
      continue;
    }
    if (owner_is_live) {
      if (recovery_lock_fd >= 0)
        ::close(recovery_lock_fd);
      continue;
    }
    if (!S_ISREG(candidate_stat.st_mode) || candidate_stat.st_size <= 0) {
      if (recovery_lock_fd >= 0)
        ::close(recovery_lock_fd);
      continue;
    }
    // Publish outside the .hstream-run-* namespace so subsequent startups do
    // not repeatedly recover and rename the same durable file.
    auto recovery = preserve_archive_work_file(candidate, configured_path);
    if (!recovery.ok()) {
      if (recovery_lock_fd >= 0)
        ::close(recovery_lock_fd);
      return recovery.status();
    }
    if (recovery_lock_fd >= 0) {
      if (have_recovery_lock_identity) {
        const absl::Status lock_cleanup =
            remove_archive_entry_if_owned(recovery_lock_path, recovery_lock_stat, "archive work ownership lock");
        if (!lock_cleanup.ok()) {
          ::close(recovery_lock_fd);
          return lock_cleanup;
        }
      }
      ::close(recovery_lock_fd);
    }
    if (recovery->has_value())
      recovered.push_back(recovery->value());
  }
  if (iterator_error) {
    return absl::InternalError(TO_STRING(
        "Failed to inspect archive work directory \"" << configured_path.parent_path().string()
                                                      << "\": " << iterator_error.message()));
  }
  return recovered;
}

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
  const bool has_hmstitcher_section = get_node(pipeline, "hmstitcher")->IsDefined();
  has_hmstitcher = has_hmstitcher_section && get_node_value(pipeline, "hmstitcher.enable", FALSE);
  if (has_hmstitcher_section) {
    auto calibration_frame_count_or = saved_or_environment_stitching_calibration_frame_count(config_);
    if (!calibration_frame_count_or.ok())
      return calibration_frame_count_or.status();
    const size_t calibration_frame_count = *calibration_frame_count_or;
    pipeline["hmstitcher"]["calibration-frame-count"] = calibration_frame_count;
    const bool enabled = get_node_value(pipeline, "hmstitcher.enable", FALSE);
    const bool configure_only = get_node_value(pipeline, "hmstitcher.configure-only", FALSE);
    const bool one_pass_mode = get_node_value(pipeline, "hmstitcher.one-pass-mode", FALSE);
    if (enabled && (configure_only || one_pass_mode)) {
      int max_output_width = 0;
      HM_ASSIGN_OR_RETURN(max_output_width, effective_hmstitcher_max_output_width(pipeline));
      bool is_configured;
      HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(game_dir, max_output_width));
      const bool calibrate_field_mask = StitcherCalibratesFieldMask(pipeline);
      const bool field_mask_configured = calibrate_field_mask && is_configured &&
          stitching::is_field_mask_configured_for_stitching_config(game_dir.string(), max_output_width);
      const char* calibration_pending = g_getenv("HSTREAM_CALIBRATION_PENDING");
      const bool calibration_completion_requested =
          calibration_pending && *calibration_pending && g_strcmp0(calibration_pending, "0") != 0;
      stitching_calibration_required_ = OnePassCalibrationRequiredForMode(
          one_pass_mode, is_configured, field_mask_configured, calibrate_field_mask, calibration_completion_requested);
      if (configure_only && is_configured && !force) {
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

absl::Status Configurator::map_common_config_keys() {
  YAML::Node pipeline = config_["pipeline"];
  if (!pipeline.IsMap())
    return absl::OkStatus();

  auto canonical_source = [&](const std::string& source_path,
                              const std::string& destination_path,
                              const YAML::Node& destination,
                              bool required) -> absl::StatusOr<std::optional<YAML::Node>> {
    const std::optional<YAML::Node> source = get_node(config_, source_path);
    if (!source.has_value() || !source->IsDefined()) {
      if (required)
        return absl::InvalidArgumentError("Effective baseline is missing supported key " + source_path);
      return std::nullopt;
    }
    const int source_rank = std::max(0, explicit_value_rank(source_path));
    const int destination_rank = explicit_value_rank(destination_path);
    if (destination_rank >= source_rank && destination_rank >= 1)
      return std::nullopt;
    if (destination.IsDefined() && !destination.IsNull() && source_rank == 0 && destination_rank < 0)
      return std::nullopt;
    return YAML::Clone(*source);
  };

  auto map_bool = [&](const std::string& source_path,
                      const std::string& destination_path,
                      YAML::Node destination,
                      const char* key,
                      bool invert = false) -> absl::Status {
    std::optional<YAML::Node> source;
    HM_ASSIGN_OR_RETURN(source, canonical_source(source_path, destination_path, destination[key], true));
    if (!source.has_value())
      return absl::OkStatus();
    if ((*source).IsNull() || !(*source).IsScalar())
      return absl::InvalidArgumentError(source_path + " must be a non-null boolean");
    try {
      const bool value = (*source).as<bool>();
      destination[key] = (invert ? !value : value) ? 1 : 0;
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + source_path + ": " + error.what());
    }
    return absl::OkStatus();
  };

  auto map_bool_or = [&](const std::string& first_source_path,
                         const std::string& second_source_path,
                         const std::string& destination_path,
                         YAML::Node destination,
                         const char* key) -> absl::Status {
    const std::optional<YAML::Node> first = get_node(config_, first_source_path);
    const std::optional<YAML::Node> second = get_node(config_, second_source_path);
    if (!first.has_value() || !first->IsDefined())
      return absl::InvalidArgumentError("Effective baseline is missing supported key " + first_source_path);
    if (!second.has_value() || !second->IsDefined())
      return absl::InvalidArgumentError("Effective baseline is missing supported key " + second_source_path);

    const int first_rank = std::max(0, explicit_value_rank(first_source_path));
    const int second_rank = std::max(0, explicit_value_rank(second_source_path));
    const int destination_rank = explicit_value_rank(destination_path);
    const int maximum_source_rank = std::max(first_rank, second_rank);
    if (destination_rank >= maximum_source_rank && destination_rank >= 1)
      return absl::OkStatus();
    if (destination[key].IsDefined() && !destination[key].IsNull() && maximum_source_rank == 0 &&
        destination_rank < 0) {
      return absl::OkStatus();
    }

    auto parse = [](const YAML::Node& value, const std::string& path) -> absl::StatusOr<bool> {
      if (value.IsNull() || !value.IsScalar())
        return absl::InvalidArgumentError(path + " must be a non-null boolean");
      try {
        return value.as<bool>();
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError("Invalid " + path + ": " + error.what());
      }
    };
    bool first_value = false;
    bool second_value = false;
    HM_ASSIGN_OR_RETURN(first_value, parse(*first, first_source_path));
    HM_ASSIGN_OR_RETURN(second_value, parse(*second, second_source_path));
    const bool mapped_value = first_value || second_value;
    const int mapped_rank =
        mapped_value ? std::max(first_value ? first_rank : -1, second_value ? second_rank : -1) : maximum_source_rank;
    if (destination_rank >= mapped_rank && destination_rank >= 1)
      return absl::OkStatus();
    if (destination[key].IsDefined() && !destination[key].IsNull() && mapped_rank == 0 && destination_rank < 0)
      return absl::OkStatus();
    destination[key] = mapped_value ? 1 : 0;
    return absl::OkStatus();
  };

  const std::optional<YAML::Node> configured_video_converter = get_node(config_, "runtime.video_converter");
  if (pipeline["application"].IsMap() ||
      (configured_video_converter.has_value() && configured_video_converter->IsDefined())) {
    if (!pipeline["application"].IsDefined() || pipeline["application"].IsNull()) {
      pipeline["application"] = YAML::Node(YAML::NodeType::Map);
    } else if (!pipeline["application"].IsMap()) {
      return absl::InvalidArgumentError("pipeline.application must be a map");
    }
    YAML::Node app = pipeline["application"];
    const int dashed_rank = explicit_value_rank("pipeline.application.video-converter");
    const int underscored_rank = explicit_value_rank("pipeline.application.video_converter");
    if (app["video_converter"].IsDefined() && (!app["video-converter"].IsDefined() || underscored_rank > dashed_rank)) {
      app["video-converter"] = YAML::Clone(app["video_converter"]);
      if (underscored_rank >= 0)
        explicit_value_ranks_["pipeline.application.video-converter"] = underscored_rank;
    }

    std::optional<YAML::Node> video_converter;
    HM_ASSIGN_OR_RETURN(
        video_converter,
        canonical_source(
            "runtime.video_converter", "pipeline.application.video-converter", app["video-converter"], true));
    if (video_converter.has_value()) {
      std::string element_name;
      HM_ASSIGN_OR_RETURN(element_name, validated_video_converter_element(*video_converter, "runtime.video_converter"));
      app["video-converter"] = element_name;
      app.remove("video_converter");
      const int source_rank = explicit_value_rank("runtime.video_converter");
      if (source_rank >= 1)
        explicit_value_ranks_["pipeline.application.video-converter"] = source_rank;
    } else if (app["video-converter"].IsDefined()) {
      std::string element_name;
      HM_ASSIGN_OR_RETURN(
          element_name,
          validated_video_converter_element(app["video-converter"], "pipeline.application.video-converter"));
      app["video-converter"] = element_name;
      app.remove("video_converter");
    }
  }

  if (pipeline["hmstitcher"].IsMap()) {
    YAML::Node stitcher = pipeline["hmstitcher"];
    if (!stitcher["properties"].IsDefined() || stitcher["properties"].IsNull()) {
      stitcher["properties"] = YAML::Node(YAML::NodeType::Map);
    } else if (!stitcher["properties"].IsMap()) {
      return absl::InvalidArgumentError("pipeline.hmstitcher.properties must be a map");
    }
    YAML::Node stitcher_properties = stitcher["properties"];
    YAML::Node stitcher_private_properties = stitcher["private-properties"];
    HM_RETURN_IF_ERROR(map_bool("stitching.enabled", "pipeline.hmstitcher.enable", stitcher, "enable"));
    HM_RETURN_IF_ERROR(
        map_bool("stitching.minimize_blend", "pipeline.hmstitcher.minimize-blend", stitcher, "minimize-blend"));
    struct MaxOutputWidthCandidate {
      std::string path;
      YAML::Node node;
      YAML::Node container;
      std::string key;
      int rank;
      int effective_rank;
      int priority;
      bool canonical;
      bool private_property;
    };
    std::vector<MaxOutputWidthCandidate> max_output_width_candidates;
    auto add_max_output_width_candidate = [&](const std::string& path,
                                              const YAML::Node& node,
                                              YAML::Node container,
                                              const std::string& key,
                                              int priority,
                                              bool canonical,
                                              bool private_property) {
      if (!node.IsDefined())
        return;
      const int rank = explicit_value_rank(path);
      if (rank < 1 && node.IsNull())
        priority = -1;
      max_output_width_candidates.push_back(
          {path, node, container, key, rank, std::max(0, rank), priority, canonical, private_property});
    };
    if (const std::optional<YAML::Node> canonical = get_node(config_, "stitching.max_output_width");
        canonical.has_value() && canonical->IsDefined()) {
      add_max_output_width_candidate("stitching.max_output_width", *canonical, YAML::Node(), "", 4, true, false);
    }
    for (const char* alias :
         {"max-output-width", "max_output_width", "stitch-max-output-width", "stitch_max_output_width"}) {
      const std::string path = std::string("pipeline.hmstitcher.properties.") + alias;
      const std::optional<YAML::Node> node = get_node(config_, path);
      if (!node.has_value() || !node->IsDefined())
        continue;
      add_max_output_width_candidate(
          path, *node, stitcher_properties, alias, std::string(alias) == "max-output-width" ? 3 : 2, false, false);
    }
    if (stitcher_private_properties.IsMap()) {
      for (const char* alias :
           {"max-output-width", "max_output_width", "stitch-max-output-width", "stitch_max_output_width"}) {
        const std::string path = std::string("pipeline.hmstitcher.private-properties.") + alias;
        const std::optional<YAML::Node> node = get_node(config_, path);
        if (!node.has_value() || !node->IsDefined())
          continue;
        add_max_output_width_candidate(path, *node, stitcher_private_properties, alias, 1, false, true);
      }
    }
    auto parse_max_output_width = [](const YAML::Node& node,
                                     const std::string& path) -> absl::StatusOr<std::optional<int>> {
      if (node.IsNull())
        return std::optional<int>();
      if (!node.IsScalar())
        return absl::InvalidArgumentError(path + " must be null or a non-negative integer");
      try {
        const int value = node.as<int>();
        if (value < 0)
          return absl::InvalidArgumentError(path + " must be null or a non-negative integer");
        return value;
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError("Invalid " + path + ": " + std::string(error.what()));
      }
    };
    if (!max_output_width_candidates.empty()) {
      const MaxOutputWidthCandidate* winner = &max_output_width_candidates.front();
      for (const MaxOutputWidthCandidate& candidate : max_output_width_candidates) {
        if (candidate.effective_rank > winner->effective_rank ||
            (candidate.effective_rank == winner->effective_rank && candidate.priority > winner->priority)) {
          winner = &candidate;
        }
      }
      std::optional<int> value;
      HM_ASSIGN_OR_RETURN(value, parse_max_output_width(winner->node, winner->path));
      auto remove_lower_ranked_aliases = [&](bool preserve_public_property) {
        for (MaxOutputWidthCandidate& candidate : max_output_width_candidates) {
          if (candidate.canonical || !candidate.container.IsMap()) {
            continue;
          }
          if (preserve_public_property && !candidate.private_property && candidate.key == "max-output-width")
            continue;
          if (candidate.effective_rank <= winner->effective_rank)
            candidate.container.remove(candidate.key);
        }
      };
      if (!value.has_value()) {
        if (winner->rank >= 1) {
          stitcher_properties.remove("max-output-width");
          remove_lower_ranked_aliases(false);
        }
      } else if (winner->private_property && winner->rank < 1) {
        for (MaxOutputWidthCandidate& candidate : max_output_width_candidates) {
          if (!candidate.canonical && !candidate.private_property && candidate.container.IsMap() &&
              candidate.effective_rank < winner->effective_rank) {
            candidate.container.remove(candidate.key);
          }
        }
      } else {
        stitcher_properties["max-output-width"] = *value;
        if (winner->rank >= 1)
          explicit_value_ranks_["pipeline.hmstitcher.properties.max-output-width"] = winner->rank;
        remove_lower_ranked_aliases(true);
      }
    }

    std::optional<YAML::Node> dtype;
    HM_ASSIGN_OR_RETURN(
        dtype,
        canonical_source(
            "stitching.dtype",
            "pipeline.hmstitcher.stitch-compute-precision",
            stitcher["stitch-compute-precision"],
            true));
    if (dtype.has_value()) {
      if ((*dtype).IsNull() || !(*dtype).IsScalar())
        return absl::InvalidArgumentError("stitching.dtype must be float32 or float16");
      const std::string value = (*dtype).as<std::string>();
      if (value == "float32" || value == "fp32")
        stitcher["stitch-compute-precision"] = "fp32";
      else if (value == "float16" || value == "fp16" || value == "half")
        stitcher["stitch-compute-precision"] = "fp16";
      else
        return absl::InvalidArgumentError("stitching.dtype must be float32 or float16");
    }

    // Promote the highest-ranked legacy canonical spelling before mapping it
    // to the native stitcher property.
    const char* canonical_rotation_path = "stitching.post_stitch_rotate_degrees";
    int rotation_rank = explicit_value_rank(canonical_rotation_path);
    std::optional<YAML::Node> rotation = get_node(config_, canonical_rotation_path);
    for (const char* alias : {
             "stitching.stitch_rotate_degrees",
             "stitching.stitch-rotate-degrees",
             "game.stitching.post_stitch_rotate_degrees",
             "game.stitching.stitch_rotate_degrees",
             "game.stitching.stitch-rotate-degrees",
         }) {
      const int alias_rank = explicit_value_rank(alias);
      const std::optional<YAML::Node> alias_value = get_node(config_, alias);
      if (alias_value.has_value() && alias_rank > rotation_rank) {
        rotation = YAML::Clone(*alias_value);
        rotation_rank = alias_rank;
      }
    }
    if (rotation.has_value() && rotation_rank >= 0) {
      config_["stitching"]["post_stitch_rotate_degrees"] = YAML::Clone(*rotation);
      explicit_value_ranks_[canonical_rotation_path] = rotation_rank;
    }
    const int dashed_rank = explicit_value_rank("pipeline.hmstitcher.post-stitch-rotate-degrees");
    const int underscored_rank = explicit_value_rank("pipeline.hmstitcher.post_stitch_rotate_degrees");
    if (stitcher["post_stitch_rotate_degrees"].IsDefined() &&
        (!stitcher["post-stitch-rotate-degrees"].IsDefined() || underscored_rank > dashed_rank)) {
      stitcher["post-stitch-rotate-degrees"] = YAML::Clone(stitcher["post_stitch_rotate_degrees"]);
      if (underscored_rank >= 0)
        explicit_value_ranks_["pipeline.hmstitcher.post-stitch-rotate-degrees"] = underscored_rank;
    }
    std::optional<YAML::Node> mapped_rotation;
    HM_ASSIGN_OR_RETURN(
        mapped_rotation,
        canonical_source(
            canonical_rotation_path,
            "pipeline.hmstitcher.post-stitch-rotate-degrees",
            stitcher["post-stitch-rotate-degrees"],
            false));
    if (mapped_rotation.has_value()) {
      if ((*mapped_rotation).IsNull()) {
        stitcher.remove("post-stitch-rotate-degrees");
        stitcher.remove("post_stitch_rotate_degrees");
      } else {
        if (!(*mapped_rotation).IsScalar())
          return absl::InvalidArgumentError("stitching.post_stitch_rotate_degrees must be null or numeric");
        try {
          const double value = (*mapped_rotation).as<double>();
          if (!std::isfinite(value))
            return absl::InvalidArgumentError("stitching.post_stitch_rotate_degrees must be finite");
          stitcher["post-stitch-rotate-degrees"] = value;
        } catch (const YAML::Exception& error) {
          return absl::InvalidArgumentError(
              "Invalid stitching.post_stitch_rotate_degrees: " + std::string(error.what()));
        }
      }
    }
  }

  if (pipeline["hmplaycropper"].IsMap()) {
    YAML::Node cropper = pipeline["hmplaycropper"];
    HM_RETURN_IF_ERROR(
        map_bool("apply_camera.crop_output_image", "pipeline.hmplaycropper.no-crop", cropper, "no-crop", true));
    HM_RETURN_IF_ERROR(map_bool_or(
        "plot.debug_play_tracker",
        "plot.plot_moving_boxes",
        "pipeline.hmplaycropper.plot-play-tracking",
        cropper,
        "plot-play-tracking"));
    HM_RETURN_IF_ERROR(map_bool_or(
        "plot.debug_play_tracker",
        "plot.plot_individual_player_tracking",
        "pipeline.hmplaycropper.plot-player-tracking",
        cropper,
        "plot-player-tracking"));
  }

  if (pipeline["ds-playtracker"].IsMap()) {
    YAML::Node tracker = pipeline["ds-playtracker"];
    HM_RETURN_IF_ERROR(map_bool_or(
        "plot.debug_play_tracker", "plot.plot_moving_boxes", "pipeline.ds-playtracker.draw", tracker, "draw"));
  }

  if (pipeline["ds-fieldmask"].IsMap()) {
    YAML::Node fieldmask = pipeline["ds-fieldmask"];
    if (!fieldmask["properties"].IsMap())
      fieldmask["properties"] = YAML::Node(YAML::NodeType::Map);
    YAML::Node properties = fieldmask["properties"];
    for (const auto& [canonical_key, native_key] : {
             std::pair<const char*, const char*>(
                 "raise_bbox_center_by_height_ratio", "raise-bbox-center-by-height-ratio"),
             std::pair<const char*, const char*>(
                 "lower_bbox_bottom_by_height_ratio", "lower-bbox-bottom-by-height-ratio"),
         }) {
      const std::string source_path = std::string("ice_boundaries.") + canonical_key;
      const std::string destination_path = std::string("pipeline.ds-fieldmask.properties.") + native_key;
      std::optional<YAML::Node> source;
      HM_ASSIGN_OR_RETURN(source, canonical_source(source_path, destination_path, properties[native_key], true));
      if (!source.has_value())
        continue;
      if ((*source).IsNull() || !(*source).IsScalar())
        return absl::InvalidArgumentError(source_path + " must be a finite number");
      try {
        const double value = (*source).as<double>();
        if (!std::isfinite(value))
          return absl::InvalidArgumentError(source_path + " must be finite");
        properties[native_key] = value;
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError("Invalid " + source_path + ": " + error.what());
      }
    }
  }

  for (const auto& entry : pipeline) {
    const std::string section_name = entry.first.as<std::string>();
    YAML::Node sink = entry.second;
    if (!absl::StartsWith(section_name, "sink") || !sink.IsMap() || !sink["type"].IsScalar())
      continue;
    int sink_type = 0;
    try {
      sink_type = sink["type"].as<int>();
    } catch (const YAML::Exception&) {
      continue;
    }
    if (sink_type != NV_DS_SINK_ENCODE_FILE)
      continue;

    const std::string sink_path = "pipeline." + section_name + ".";
    std::optional<YAML::Node> bitrate;
    HM_ASSIGN_OR_RETURN(bitrate, canonical_source("video_out.bit_rate", sink_path + "bitrate", sink["bitrate"], true));
    if (bitrate.has_value()) {
      if ((*bitrate).IsNull() || !(*bitrate).IsScalar())
        return absl::InvalidArgumentError("video_out.bit_rate must be a positive integer");
      try {
        const int64_t value = (*bitrate).as<int64_t>();
        if (value <= 0 || value > G_MAXINT)
          return absl::InvalidArgumentError("video_out.bit_rate must fit a positive native encoder bitrate");
        sink["bitrate"] = value;
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError("Invalid video_out.bit_rate: " + std::string(error.what()));
      }
    }

    std::optional<YAML::Node> output_path;
    HM_ASSIGN_OR_RETURN(
        output_path,
        canonical_source("video_out.output_video_path", sink_path + "output-file", sink["output-file"], false));
    if (output_path.has_value()) {
      if ((*output_path).IsNull()) {
        if (explicit_value_rank("video_out.output_video_path") >= 1)
          sink.remove("output-file");
      } else {
        if (!(*output_path).IsScalar() || (*output_path).as<std::string>().empty())
          return absl::InvalidArgumentError("video_out.output_video_path must be null or a non-empty path");
        sink["output-file"] = (*output_path).as<std::string>();
      }
    }

    for (const auto& [canonical_key, native_key] : {
             std::pair<const char*, const char*>("output_width", "width"),
             std::pair<const char*, const char*>("output_height", "height"),
         }) {
      const std::string source_path = std::string("video_out.") + canonical_key;
      const std::string destination_path = sink_path + native_key;
      std::optional<YAML::Node> dimension;
      HM_ASSIGN_OR_RETURN(dimension, canonical_source(source_path, destination_path, sink[native_key], false));
      if (!dimension.has_value())
        continue;
      const int source_rank = explicit_value_rank(source_path);
      if ((*dimension).IsNull() || ((*dimension).IsScalar() && (*dimension).as<std::string>() == "auto")) {
        if (source_rank >= 1)
          sink.remove(native_key);
        continue;
      }
      if (!(*dimension).IsScalar())
        return absl::InvalidArgumentError(source_path + " must be auto, null, or a positive integer");
      try {
        const int value = (*dimension).as<int>();
        if (value <= 0)
          return absl::InvalidArgumentError(source_path + " must be auto, null, or a positive integer");
        sink[native_key] = value;
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError("Invalid " + source_path + ": " + error.what());
      }
    }
  }

  const std::optional<YAML::Node> fixed_edge_rotation = get_node(config_, "rink.camera.fixed_edge_rotation_angle");
  if (!fixed_edge_rotation.has_value() || !fixed_edge_rotation->IsDefined())
    return absl::InvalidArgumentError(
        "Effective baseline is missing supported key rink.camera.fixed_edge_rotation_angle");
  auto validate_fixed_edge_value = [](const YAML::Node& value, const std::string& path) -> absl::Status {
    if (!value.IsScalar())
      return absl::InvalidArgumentError(path + " must be numeric");
    try {
      if (!std::isfinite(value.as<double>()))
        return absl::InvalidArgumentError(path + " must be finite");
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + path + ": " + error.what());
    }
    return absl::OkStatus();
  };
  if (!fixed_edge_rotation->IsNull()) {
    if (fixed_edge_rotation->IsSequence()) {
      if (fixed_edge_rotation->size() != 2)
        return absl::InvalidArgumentError("rink.camera.fixed_edge_rotation_angle must be numeric or [left, right]");
      HM_RETURN_IF_ERROR(
          validate_fixed_edge_value((*fixed_edge_rotation)[0], "rink.camera.fixed_edge_rotation_angle[0]"));
      HM_RETURN_IF_ERROR(
          validate_fixed_edge_value((*fixed_edge_rotation)[1], "rink.camera.fixed_edge_rotation_angle[1]"));
    } else {
      HM_RETURN_IF_ERROR(validate_fixed_edge_value(*fixed_edge_rotation, "rink.camera.fixed_edge_rotation_angle"));
    }
  }
  for (const char* stage : {"hmplaycropper", "ds-playtracker"}) {
    YAML::Node stage_config = pipeline[stage];
    if (!stage_config.IsMap())
      continue;
    const std::string prefix = std::string("pipeline.") + stage + ".";
    const int source_rank = std::max(0, explicit_value_rank("rink.camera.fixed_edge_rotation_angle"));
    auto may_replace = [&](const char* key) {
      const int destination_rank = explicit_value_rank(prefix + key);
      return destination_rank < source_rank &&
          !(destination_rank < 0 && source_rank == 0 && stage_config[key].IsDefined());
    };
    if (fixed_edge_rotation->IsNull()) {
      if (source_rank >= 1) {
        for (const char* key : {
                 "fixed-edge-rotation-angle",
                 "fixed-edge-rotation-angle-left",
                 "fixed-edge-rotation-angle-right",
             }) {
          if (may_replace(key))
            stage_config.remove(key);
        }
      }
    } else if (fixed_edge_rotation->IsSequence()) {
      const char* generic_key = "fixed-edge-rotation-angle";
      const int generic_rank = explicit_value_rank(prefix + generic_key);
      const bool generic_native_wins = !may_replace(generic_key);
      if (generic_native_wins) {
        // The native parser applies the generic property before side-specific
        // properties. A winning generic value therefore owns both sides; any
        // lower-ranked side values must be removed rather than allowed to
        // override it by parser order.
        if (generic_rank >= 1) {
          for (const char* side_key : {"fixed-edge-rotation-angle-left", "fixed-edge-rotation-angle-right"}) {
            if (explicit_value_rank(prefix + side_key) < generic_rank)
              stage_config.remove(side_key);
          }
        }
        continue;
      }
      stage_config.remove(generic_key);
      if (may_replace("fixed-edge-rotation-angle-left"))
        stage_config["fixed-edge-rotation-angle-left"] = (*fixed_edge_rotation)[0];
      if (may_replace("fixed-edge-rotation-angle-right"))
        stage_config["fixed-edge-rotation-angle-right"] = (*fixed_edge_rotation)[1];
    } else if (fixed_edge_rotation->IsScalar()) {
      if (may_replace("fixed-edge-rotation-angle"))
        stage_config["fixed-edge-rotation-angle"] = YAML::Clone(*fixed_edge_rotation);
      if (source_rank >= 1) {
        if (may_replace("fixed-edge-rotation-angle-left"))
          stage_config.remove("fixed-edge-rotation-angle-left");
        if (may_replace("fixed-edge-rotation-angle-right"))
          stage_config.remove("fixed-edge-rotation-angle-right");
      }
    } else {
      return absl::InvalidArgumentError("rink.camera.fixed_edge_rotation_angle must be numeric or [left, right]");
    }
  }
  return absl::OkStatus();
}

absl::Status Configurator::apply_supported_baseline_mappings() {
  HM_RETURN_IF_ERROR(map_common_config_keys());
  YAML::Node pipeline = config_["pipeline"];
  if (pipeline.IsMap())
    HM_RETURN_IF_ERROR(apply_scoreboard_perspective(pipeline));
  return absl::OkStatus();
}

namespace {

constexpr size_t kPlaytrackerRuntimeRetentionCount = 8;
constexpr auto kPlaytrackerRuntimeGracePeriod = std::chrono::hours(24);

fs::path playtracker_runtime_directory(const fs::path& game_dir) {
  if (!game_dir.empty())
    return game_dir / ".hstream-runtime";
  if (const char* runtime_root = std::getenv("XDG_RUNTIME_DIR"); runtime_root && *runtime_root) {
    const fs::path configured(runtime_root);
    if (configured.is_absolute())
      return configured / "hstream";
  }
  std::error_code error;
  fs::path temporary = fs::temp_directory_path(error);
  if (error)
    temporary = "/tmp";
  return temporary / ("hstream-" + std::to_string(::getuid()));
}

absl::StatusOr<int> acquire_playtracker_runtime_lock(const fs::path& path) {
  // The YAML file itself is the lock object. Cleanup verifies inode identity
  // after locking, so an opener racing an unlink will retry the published path
  // instead of retaining a lock on an unreachable inode.
  for (int attempt = 0; attempt < 4; ++attempt) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      if (errno == ENOENT)
        continue;
      return absl::InternalError(
          "Could not open playtracker runtime config " + path.string() + ": " + std::strerror(errno));
    }
    if (::flock(fd, LOCK_SH) != 0) {
      const int saved_errno = errno;
      ::close(fd);
      return absl::InternalError(
          "Could not lock playtracker runtime config " + path.string() + ": " + std::strerror(saved_errno));
    }
    struct stat opened_stat{};
    struct stat path_stat{};
    const bool current_inode = ::fstat(fd, &opened_stat) == 0 && ::lstat(path.c_str(), &path_stat) == 0 &&
        opened_stat.st_dev == path_stat.st_dev && opened_stat.st_ino == path_stat.st_ino &&
        S_ISREG(opened_stat.st_mode);
    if (current_inode)
      return fd;
    ::close(fd);
  }
  return absl::UnavailableError("Playtracker runtime config changed repeatedly while acquiring its reader lock");
}

absl::Status prune_playtracker_runtime_configs(const fs::path& runtime_dir, const fs::path& current_path) {
  struct Candidate {
    fs::path path;
    fs::file_time_type modified;
  };
  std::vector<Candidate> candidates;
  const std::regex generated_name(R"(^play_tracker_config-[0-9a-f]+\.yaml$)");
  std::error_code error;
  for (fs::directory_iterator it(runtime_dir, error), end; !error && it != end; it.increment(error)) {
    const fs::path path = it->path();
    if (!std::regex_match(path.filename().string(), generated_name) || !it->is_regular_file(error)) {
      error.clear();
      continue;
    }
    const fs::file_time_type modified = fs::last_write_time(path, error);
    if (error) {
      error.clear();
      continue;
    }
    candidates.push_back({path, modified});
  }
  if (error)
    return absl::InternalError("Could not inspect playtracker runtime configs: " + error.message());
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    return left.modified > right.modified;
  });
  const fs::file_time_type stale_before = fs::file_time_type::clock::now() - kPlaytrackerRuntimeGracePeriod;
  for (size_t index = 0; index < candidates.size(); ++index) {
    const Candidate& candidate = candidates[index];
    if (candidate.path == current_path || index < kPlaytrackerRuntimeRetentionCount ||
        candidate.modified > stale_before) {
      continue;
    }
    const int fd = ::open(candidate.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      if (errno == ENOENT)
        continue;
      return absl::InternalError(
          "Could not inspect stale playtracker runtime config " + candidate.path.string() + ": " +
          std::strerror(errno));
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int saved_errno = errno;
      ::close(fd);
      if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
        continue;
      return absl::InternalError(
          "Could not lock stale playtracker runtime config " + candidate.path.string() + ": " +
          std::strerror(saved_errno));
    }
    struct stat opened_stat{};
    struct stat path_stat{};
    const bool current_inode = ::fstat(fd, &opened_stat) == 0 && ::lstat(candidate.path.c_str(), &path_stat) == 0 &&
        opened_stat.st_dev == path_stat.st_dev && opened_stat.st_ino == path_stat.st_ino &&
        S_ISREG(opened_stat.st_mode);
    if (current_inode && ::unlink(candidate.path.c_str()) != 0 && errno != ENOENT) {
      const int saved_errno = errno;
      ::close(fd);
      return absl::InternalError(
          "Could not remove stale playtracker runtime config " + candidate.path.string() + ": " +
          std::strerror(saved_errno));
    }
    ::close(fd);
  }
  return absl::OkStatus();
}

} // namespace

absl::Status Configurator::materialize_playtracker_config(
    YAML::Node& pipeline,
    const fs::path& game_dir,
    const fs::path& pipeline_config_dir) {
  YAML::Node section = pipeline["ds-playtracker"];
  if (!section.IsDefined())
    return absl::OkStatus();
  bool enabled = false;
  try {
    enabled = get_node_value(section, "enable", false);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid pipeline.ds-playtracker.enable: " + std::string(error.what()));
  }
  if (!enabled)
    return absl::OkStatus();
  YAML::Node configured_file = section["config-file"];
  if (!configured_file.IsScalar())
    return absl::InvalidArgumentError("pipeline.ds-playtracker.config-file must name a base YAML file");

  const fs::path configured_path = configured_file.as<std::string>();
  std::vector<fs::path> candidates;
  if (configured_path.is_absolute()) {
    candidates.push_back(configured_path);
  } else {
    auto append_candidate = [&](const fs::path& root) {
      if (root.empty())
        return;
      const fs::path candidate = root / configured_path;
      if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        candidates.push_back(candidate);
    };
    const int configured_file_rank = explicit_value_rank("pipeline.ds-playtracker.config-file");
    if (configured_file_rank == 3) {
      append_candidate(fs::current_path());
    } else if (configured_file_rank == 2) {
      append_candidate(game_dir);
    } else if (configured_file_rank == 1) {
      const auto user_config_path = user_config::file_path();
      if (user_config_path.ok())
        append_candidate(user_config_path->parent_path());
    } else {
      // Structural app defaults are owned by the app config, not by an
      // arbitrary same-named file left in the selected game directory.
      append_candidate(pipeline_config_dir);
      append_candidate(fs::path(config_root_dir_));
    }
    append_candidate(pipeline_config_dir);
    append_candidate(fs::path(config_root_dir_));
    append_candidate(game_dir);
    append_candidate(fs::current_path());
    append_candidate(fs::current_path() / "configs");
  }
  fs::path base_path;
  for (const fs::path& candidate : candidates) {
    std::error_code error;
    if (fs::is_regular_file(candidate, error) && !error) {
      base_path = candidate;
      break;
    }
  }
  if (base_path.empty()) {
    std::string searched;
    for (const fs::path& candidate : candidates) {
      if (!searched.empty())
        searched += ", ";
      searched += candidate.string();
    }
    return absl::NotFoundError("Could not locate pipeline.ds-playtracker.config-file; searched: " + searched);
  }

  YAML::Node base;
  try {
    base = YAML::LoadFile(base_path.string());
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(
        "Failed to load playtracker base config " + base_path.string() + ": " + error.what());
  }
  YAML::Node effective;
  const int native_base_rank = std::max(0, explicit_value_rank("pipeline.ds-playtracker.config-file"));
  HM_ASSIGN_OR_RETURN(
      effective,
      configurator_internal::build_effective_playtracker_config(
          config_, explicit_value_ranks_, native_base_rank, base));
  const std::string contents = YAML::Dump(effective) + "\n";

  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : contents) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream filename;
  filename << "play_tracker_config-" << std::hex << hash << ".yaml";
  const fs::path runtime_dir = playtracker_runtime_directory(game_dir);
  std::error_code directory_error;
  fs::create_directories(runtime_dir, directory_error);
  if (directory_error) {
    return absl::InternalError(
        "Could not create playtracker runtime config directory " + runtime_dir.string() + ": " +
        directory_error.message());
  }
  const fs::path effective_path = runtime_dir / filename.str();
  const std::string effective_path_key = effective_path.string();
  if (playtracker_runtime_lock_fds_.find(effective_path_key) == playtracker_runtime_lock_fds_.end()) {
    absl::StatusOr<int> reader_lock = absl::UnavailableError("Playtracker runtime config was not published");
    for (int attempt = 0; attempt < 4 && !reader_lock.ok(); ++attempt) {
      bool already_current = false;
      {
        std::ifstream existing(effective_path, std::ios::binary);
        if (existing) {
          const std::string existing_contents(
              (std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
          already_current = existing_contents == contents;
        }
      }
      if (!already_current)
        HM_RETURN_IF_ERROR(stitching::publish_named_file(effective_path, contents));
      reader_lock = acquire_playtracker_runtime_lock(effective_path);
    }
    if (!reader_lock.ok())
      return reader_lock.status();
    playtracker_runtime_lock_fds_.emplace(effective_path_key, *reader_lock);
  }
  const absl::Status prune_status = prune_playtracker_runtime_configs(runtime_dir, effective_path);
  if (!prune_status.ok())
    std::cerr << "Warning: " << prune_status << '\n';
  section["config-file"] = effective_path.string();
  return absl::OkStatus();
}

absl::Status Configurator::invalidate_rotation_dependent_cache_if_needed(const fs::path& game_dir) {
  double desired_rotation = 0.0;
  HM_ASSIGN_OR_RETURN(desired_rotation, configurator_internal::effective_stitch_output_rotation(config_));
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

  remove_rotation_dependent_rink_cache_keys(config_);
  remove_rotation_dependent_rink_cache_keys(private_config_);
  private_config_["stitching"]["generated_field_mask_post_stitch_rotate_degrees"] = desired_rotation;
  auto save_status =
      save_private_config(private_config_, active_stitching_invalidation_id_, /*remove_rink_masks=*/true);
  if (!save_status.ok()) {
    if (!active_stitching_invalidation_id_.empty())
      return save_status;
    std::cerr << "Warning: failed to save post-stitch rotation cache marker: " << save_status << std::endl;
  }
  return absl::OkStatus();
}

absl::Status Configurator::invalidate_canvas_dependent_cache_if_needed(const fs::path& game_dir) {
  bool exceeds_limit = false;
  int max_output_width = 0;
  HM_ASSIGN_OR_RETURN(max_output_width, effective_hmstitcher_max_output_width(config_["pipeline"]));
  HM_ASSIGN_OR_RETURN(
      exceeds_limit, stitching::stitching_artifacts_exceed_live_canvas_limit(game_dir.string(), max_output_width));
  if (!exceeds_limit) {
    return absl::OkStatus();
  }

  std::cout << "Stitching canvas exceeds live-stitch max dimension; clearing canvas-dependent cached rink geometry"
            << std::endl;
  remove_rotation_dependent_rink_cache_keys(config_);
  remove_rotation_dependent_rink_cache_keys(private_config_);
  if (private_config_.IsDefined()) {
    HM_RETURN_IF_ERROR(save_private_config(private_config_, active_stitching_invalidation_id_));
  }
  return absl::OkStatus();
}

absl::Status Configurator::apply_scoreboard_perspective(YAML::Node& pipeline) {
  if (!pipeline["hmplaycropper"].IsDefined()) {
    return absl::OkStatus();
  }
  const auto map_playcropper_scalar = [&](const std::string& dest_key, const std::string& src_key) -> absl::Status {
    std::optional<YAML::Node> src_node = get_node(config_, src_key);
    if (!src_node || !src_node->IsDefined())
      return absl::InvalidArgumentError("Effective baseline is missing supported key " + src_key);
    const std::string destination_path = "pipeline.hmplaycropper." + dest_key;
    const int source_rank = std::max(0, explicit_value_rank(src_key));
    const int destination_rank = explicit_value_rank(destination_path);
    YAML::Node destination = pipeline["hmplaycropper"][dest_key];
    if (destination_rank >= source_rank && destination_rank >= 1)
      return absl::OkStatus();
    if (destination.IsDefined() && !destination.IsNull() && source_rank == 0 && destination_rank < 0)
      return absl::OkStatus();
    if (src_node->IsNull()) {
      if (source_rank >= 1)
        pipeline["hmplaycropper"].remove(dest_key);
      return absl::OkStatus();
    }
    if (!src_node->IsScalar())
      return absl::InvalidArgumentError(src_key + " must be a scalar");
    pipeline["hmplaycropper"][dest_key] = YAML::Clone(*src_node);
    return absl::OkStatus();
  };
  HM_RETURN_IF_ERROR(map_playcropper_scalar("scoreboard-projected-width", "rink.scoreboard.projected_width"));
  HM_RETURN_IF_ERROR(map_playcropper_scalar("scoreboard-projected-height", "rink.scoreboard.projected_height"));
  HM_RETURN_IF_ERROR(map_playcropper_scalar("scoreboard-scale", "rink.scoreboard.scoreboard_scale"));

  const std::string source_path = "rink.scoreboard.perspective_polygon";
  const std::string destination_path = "pipeline.hmplaycropper.scoreboard-perspective-polygon";
  const std::optional<YAML::Node> polygon = get_node(config_, source_path);
  if (!polygon.has_value() || !polygon->IsDefined())
    return absl::InvalidArgumentError("Effective baseline is missing supported key " + source_path);
  const int source_rank = std::max(0, explicit_value_rank(source_path));
  const int destination_rank = explicit_value_rank(destination_path);
  YAML::Node destination = pipeline["hmplaycropper"]["scoreboard-perspective-polygon"];
  const bool source_wins = !(destination_rank >= source_rank && destination_rank >= 1) &&
      !(destination.IsDefined() && !destination.IsNull() && source_rank == 0 && destination_rank < 0);
  if (source_wins && polygon->IsNull()) {
    if (source_rank >= 1)
      pipeline["hmplaycropper"].remove("scoreboard-perspective-polygon");
  } else if (source_wins) {
    if (!polygon->IsSequence())
      return absl::InvalidArgumentError(source_path + " must be null or four [x, y] points");
    std::vector<std::vector<int>> points;
    try {
      points = polygon->as<std::vector<std::vector<int>>>();
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError("Invalid " + source_path + ": " + error.what());
    }
    const bool disabled = points.size() == 4 && std::all_of(points.begin(), points.end(), [](const auto& point) {
                            return point.size() == 2 && point[0] == 0 && point[1] == 0;
                          });
    if (!points.empty() && !disabled) {
      if (points.size() != 4 ||
          !std::all_of(points.begin(), points.end(), [](const auto& point) { return point.size() == 2; })) {
        return absl::InvalidArgumentError(source_path + " must contain four [x, y] points");
      }
      std::stringstream ss;
      for (size_t i = 0, n = points.size(); i < n; ++i) {
        if (i)
          ss << ',';
        ss << std::to_string(points[i].at(0)) << ',' << points[i].at(1);
      }
      pipeline["hmplaycropper"]["scoreboard-perspective-polygon"] = ss.str();
    } else if (source_rank >= 1) {
      pipeline["hmplaycropper"].remove("scoreboard-perspective-polygon");
    }
  }
  return absl::OkStatus();
}

absl::Status Configurator::gather_stitching_videos(
    const fs::path& game_dir,
    bool force,
    std::vector<std::string>& left_files,
    std::vector<std::string>& right_files,
    YAML::Node& offsets) {
  const configurator_internal::ExplicitStitchingVideoSelection explicit_selection =
      configurator_internal::select_explicit_stitching_videos(config_, force);
  if (!explicit_selection.error.empty())
    return absl::InvalidArgumentError(explicit_selection.error);
  left_files = explicit_selection.left;
  right_files = explicit_selection.right;
  bool explicit_left = explicit_selection.left_is_explicit;
  bool explicit_right = explicit_selection.right_is_explicit;
  const bool runtime_videos_owned_by_ui_roles = explicit_selection.ui_roles_are_authoritative;

  stitching::VideosDict videos;
  HM_ASSIGN_OR_RETURN(videos, stitching::get_available_videos(game_dir));
  const bool has_cam_auto =
      std::any_of(videos.begin(), videos.end(), [](const auto& item) { return is_cam_video_key(item.first); });
  const bool has_left_right_auto = videos.count("left") || videos.count("right");
  if (!force && has_cam_auto && !has_left_right_auto && !runtime_videos_owned_by_ui_roles &&
      (explicit_left || explicit_right)) {
    std::cerr << "Ignoring stale generated game.videos left/right because camN Auto video sets are available"
              << std::endl;
    left_files.clear();
    right_files.clear();
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
      auto spp_status = save_private_config(private_config_, active_stitching_invalidation_id_);
      if (!spp_status.ok()) {
        if (!active_stitching_invalidation_id_.empty())
          return spp_status;
        std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
      }
    }
  }

  HM_RETURN_IF_ERROR(
      configurator_internal::validate_mixed_explicit_auto_playlists(
          explicit_left,
          explicit_right,
          videos.count("left") ? videos.at("left").size() : 0,
          videos.count("right") ? videos.at("right").size() : 0));

  auto append_chapters = [](const stitching::VideoChapter& chapters, std::vector<std::string>& files) {
    for (const auto& item : chapters) {
      files.emplace_back(item.second);
    }
  };

  if ((explicit_left || explicit_right) && left_files.empty() && videos.count("left")) {
    // Preserve the complete independently segmented camera playlist. The lossless mux pairs decoded sequence numbers,
    // not physical filenames, after both playlists have been constructed.
    append_chapters(videos.at("left"), left_files);
  }
  if ((explicit_left || explicit_right) && right_files.empty() && videos.count("right")) {
    append_chapters(videos.at("right"), right_files);
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
    private_config_["game"]["videos"]["left"] = left_files;
    private_config_["game"]["videos"]["right"] = right_files;
    auto spp_status = save_private_config(private_config_, active_stitching_invalidation_id_);
    if (!spp_status.ok()) {
      if (!active_stitching_invalidation_id_.empty())
        return spp_status;
      std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
    }
  }

  if (left_files.empty() && right_files.empty() && !videos.count("left") && !videos.count("right")) {
    HM_RETURN_IF_ERROR(stitching::configure_orientation(game_dir, active_stitching_invalidation_id_));
    overlay_config("", (game_dir / "config.yaml").string());
    private_config_["game"]["videos"]["left"] = config_["game"]["videos"]["left"];
    private_config_["game"]["videos"]["right"] = config_["game"]["videos"]["right"];
    left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
    right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
  }

  if (left_files.empty() && right_files.empty() && videos.count("left") && videos.count("right")) {
    const stitching::VideoChapter& left_chapter = videos.at("left");
    const stitching::VideoChapter& right_chapter = videos.at("right");
    // Never intersect chapter-number keys here. Doing so drops whole files when camera chapter boundaries differ.
    append_chapters(left_chapter, left_files);
    append_chapters(right_chapter, right_files);
    if (!left_files.empty() && !right_files.empty()) {
      private_config_["game"]["videos"]["left"] = left_files;
      private_config_["game"]["videos"]["right"] = right_files;
      auto spp_status = save_private_config(private_config_, active_stitching_invalidation_id_);
      if (!spp_status.ok()) {
        if (!active_stitching_invalidation_id_.empty())
          return spp_status;
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
      auto spp_status = save_private_config(private_config_, active_stitching_invalidation_id_);
      if (!spp_status.ok()) {
        if (!active_stitching_invalidation_id_.empty())
          return spp_status;
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
    if (ww && hh) {
      auto wh_tuple = cap_output(ww, hh);
      pipeline["hmplaycropper"]["output-width"] = std::to_string(round_down_even(std::get<0>(wh_tuple)));
      pipeline["hmplaycropper"]["output-height"] = std::to_string(round_down_even(std::get<1>(wh_tuple)));
    }
  } else if (!left_files.empty() && !right_files.empty() && has_hmstitcher) {
    StitcherSizingConfig sizing_cfg = ParseStitcherSizingConfig(pipeline);
    int max_output_width = 0;
    HM_ASSIGN_OR_RETURN(max_output_width, effective_hmstitcher_max_output_width(pipeline));
    std::optional<std::tuple<int, int>> canvas_size_result;
    auto stitching_configured = stitching::is_stitching_configured(game_dir.string(), max_output_width);
    if (!stitching_configured.ok()) {
      return stitching_configured.status();
    }
    if (stitching_configured.value()) {
      auto canvas_size = stitching::stitching_canvas_size(game_dir.string(), max_output_width);
      if (!canvas_size.ok()) {
        return canvas_size.status();
      }
      canvas_size_result = std::make_tuple(static_cast<int>(canvas_size->width), static_cast<int>(canvas_size->height));
    }
    if (canvas_size_result) {
      size_t canvas_width = std::get<0>(*canvas_size_result);
      size_t canvas_height = std::get<1>(*canvas_size_result);
      pipeline["hmstitcher"]["output-width"] = std::to_string(canvas_width);
      pipeline["hmstitcher"]["output-height"] = std::to_string(canvas_height);
      const bool no_crop = get_node_value(pipeline, "hmplaycropper.no-crop", false);
      const auto even_canvas_height = static_cast<long>(round_down_even(static_cast<long>(canvas_height)));
      auto wh_tuple = no_crop ? cap_output(static_cast<long>(canvas_width), even_canvas_height)
                              : cap_output(static_cast<long>((16.0 / 9.0) * even_canvas_height), even_canvas_height);
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
    if (ww && hh) {
      auto wh_tuple = cap_output(ww, hh);
      pipeline["hmplaycropper"]["output-width"] = std::to_string(round_down_even(std::get<0>(wh_tuple)));
      pipeline["hmplaycropper"]["output-height"] = std::to_string(round_down_even(std::get<1>(wh_tuple)));
    }
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
      std::vector<std::string> left_uri_list;
      left_uri_list.reserve(left_files.size());
      for (const auto& f : left_files) {
        left_uri_list.emplace_back(ff + file_maybe_in_game_dir(f));
      }
      src0["uri-list"] = left_uri_list;
      std::vector<std::string> right_uri_list;
      right_uri_list.reserve(right_files.size());
      for (const auto& f : right_files) {
        right_uri_list.emplace_back(ff + file_maybe_in_game_dir(f));
      }
      src1["uri-list"] = right_uri_list;
      // URI-MULTIPLE stitching is the lossless file path: metadata attached at decoder output must survive every
      // conversion and nvstreammux. Requiring it makes total metadata loss a hard error instead of silently trusting
      // a mux-local frame counter that cannot reveal an upstream drop.
      pipeline["hmstitcher"]["private-properties"]["require-decoded-frame-sequence-meta"] = "1";
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

absl::Status Configurator::configure_encode_file_outputs(
    YAML::Node& pipeline,
    const std::vector<std::string>& source_video_paths) const {
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

  struct ArchiveOutput {
    YAML::Node sink_node;
    int sink_id;
    int codec;
    fs::path configured_path;
    bool bitrate_is_explicit;
  };

  std::optional<fs::path> output_work_dir;
  std::map<std::string, std::string> claimed_output_paths;
  std::vector<ArchiveOutput> archive_outputs;

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

    if (!output_work_dir.has_value()) {
      auto configured_output_root = user_config::output_root(config_);
      if (!configured_output_root.ok())
        return configured_output_root.status();
      output_work_dir = *configured_output_root / game_id_;
    }

    const int sink_id = get_node_value<int>(sink_node, "sink-id", -1);
    const int codec = get_node_value<int>(sink_node, "codec", 0);
    std::string output_file = get_node_value<std::string>(sink_node, "output-file", "");
    if (!is_valid_yaml_value_string(output_file) || output_file == kLegacyDefaultOutputName) {
      output_file = kDefaultOutputVideoName;
    }

    fs::path output_path(output_file);
    const bool output_was_rebased = !output_path.has_parent_path();
    if (output_was_rebased) {
      output_path = *output_work_dir / output_path;
    }
    if (output_was_rebased && has_audio_for_sink(sink_id)) {
      output_path = add_audio_suffix_to_output_path(output_path.string());
    }
    if (output_path.is_relative()) {
      output_path = fs::absolute(output_path);
    }

    HM_RETURN_IF_ERROR(configurator_internal::claim_unique_archive_output_path(claimed_output_paths, output_path, key));
    const int canonical_bitrate_rank = explicit_value_rank("video_out.bit_rate");
    const int native_bitrate_rank = explicit_value_rank("pipeline." + key + ".bitrate");
    archive_outputs.push_back(
        {sink_node, sink_id, codec, std::move(output_path), canonical_bitrate_rank >= 1 || native_bitrate_rank >= 1});
  }

  const bool has_auto_bitrate_output =
      std::any_of(archive_outputs.begin(), archive_outputs.end(), [](const ArchiveOutput& output) {
        return !output.bitrate_is_explicit;
      });
  const std::optional<SourceBitrateReference> bitrate_reference =
      has_auto_bitrate_output ? select_source_bitrate_reference(source_video_paths) : std::nullopt;
  if (has_auto_bitrate_output && !bitrate_reference.has_value()) {
    g_printerr(
        "Warning: source video bitrate metadata is unavailable; encode-file sinks will retain their configured "
        "bitrate\n");
  } else if (bitrate_reference.has_value()) {
    g_print(
        "HSTREAM_ARCHIVE_BITRATE_REFERENCE bitrate=%" G_GUINT64_FORMAT " width=%" G_GUINT64_FORMAT
        " height=%" G_GUINT64_FORMAT " ratio=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " source=%s\n",
        bitrate_reference->bitrate,
        bitrate_reference->width,
        bitrate_reference->height,
        bitrate_reference->bitrate_per_pixel.numerator,
        bitrate_reference->bitrate_per_pixel.denominator,
        bitrate_reference->path.c_str());
    std::fflush(stdout);
  }

  // Do not reserve, recover, or mutate any output until the complete sink set
  // has passed the duplicate-writer preflight above.
  for (ArchiveOutput& archive_output : archive_outputs) {
    YAML::Node sink_node = archive_output.sink_node;
    const int sink_id = archive_output.sink_id;
    const int codec = archive_output.codec;
    fs::path output_path = archive_output.configured_path;

    if (bitrate_reference.has_value() && !archive_output.bitrate_is_explicit) {
      sink_node["bitrate"] =
          std::to_string(std::min<uint64_t>(bitrate_reference->bitrate, static_cast<uint64_t>(G_MAXINT)));
      sink_node["bitrate-per-pixel-numerator"] = std::to_string(bitrate_reference->bitrate_per_pixel.numerator);
      sink_node["bitrate-per-pixel-denominator"] = std::to_string(bitrate_reference->bitrate_per_pixel.denominator);
    }

    const fs::path parent = output_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec) {
        return absl::InternalError(
            TO_STRING("Failed to create output directory \"" << parent.string() << "\": " << ec.message()));
      }
    }

    const std::string configured_output_path = output_path.lexically_normal().string();
    if (archive_lock_fds_.find(configured_output_path) == archive_lock_fds_.end()) {
      auto archive_lock = configurator_internal::acquire_archive_output_lock(output_path);
      if (!archive_lock.ok())
        return archive_lock.status();
      archive_lock_fds_.emplace(configured_output_path, *archive_lock);
    }

    auto stale_recoveries = configurator_internal::recover_stale_archive_work_files(output_path);
    if (!stale_recoveries.ok())
      return stale_recoveries.status();
    for (const fs::path& recovery_path : *stale_recoveries) {
      g_print("HSTREAM_OUTPUT_RECOVERY type=archive sink=%d path=%s\n", sink_id, recovery_path.string().c_str());
    }
    if (!stale_recoveries->empty())
      std::fflush(stdout);

    auto recovered_output = configurator_internal::preserve_existing_archive_work_file(output_path);
    if (!recovered_output.ok())
      return recovered_output.status();
    if (recovered_output->has_value()) {
      g_print(
          "HSTREAM_OUTPUT_RECOVERY type=archive sink=%d path=%s\n",
          sink_id,
          recovered_output->value().string().c_str());
      std::fflush(stdout);
    }

    const char* archive_run_id = g_getenv("HSTREAM_ARCHIVE_RUN_ID");
    if (archive_run_id && *archive_run_id) {
      auto existing_run_path = archive_run_paths_.find(configured_output_path);
      if (existing_run_path != archive_run_paths_.end()) {
        output_path = existing_run_path->second;
      } else {
        const std::string owned_run_id = "v3-" + std::to_string(::getpid()) + "-" + archive_run_id;
        auto unique_output = configurator_internal::reserve_unique_archive_work_file(output_path, owned_run_id);
        if (!unique_output.ok())
          return unique_output.status();
        output_path = *unique_output;
        auto work_lock = configurator_internal::acquire_archive_work_owner_lock(output_path);
        if (!work_lock.ok()) {
          ::unlink(output_path.c_str());
          ::unlink(configurator_internal::archive_work_owner_lock_path(output_path).c_str());
          return work_lock.status();
        }
        archive_work_lock_fds_.emplace(configured_output_path, *work_lock);
        archive_run_paths_.emplace(configured_output_path, output_path);
      }
    }

    set_container_from_output_extension(sink_node, output_path);
    sink_node["output-file"] = output_path.string();
    const std::string normalized_output_path = output_path.lexically_normal().string();
    struct stat previous_output{};
    const gboolean output_existed =
        ::stat(normalized_output_path.c_str(), &previous_output) == 0 && S_ISREG(previous_output.st_mode);
    const gint64 previous_size = output_existed ? static_cast<gint64>(previous_output.st_size) : -1;
    const gint64 previous_mtime_ms = output_existed
        ? static_cast<gint64>(previous_output.st_mtim.tv_sec) * 1000 + previous_output.st_mtim.tv_nsec / 1000000
        : -1;
    g_print(
        "HSTREAM_OUTPUT type=archive sink=%d existed=%d size=%" G_GINT64_FORMAT " mtime-ms=%" G_GINT64_FORMAT
        " codec=%s path=%s\n",
        sink_id,
        output_existed,
        previous_size,
        previous_mtime_ms,
        codec == 2 ? "hevc" : (codec == 1 ? "h264" : "unknown"),
        normalized_output_path.c_str());
    std::fflush(stdout);
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
  for (const auto& [_, lock_fd] : playtracker_runtime_lock_fds_)
    ::close(lock_fd);
  for (const auto& [_, lock_fd] : archive_work_lock_fds_)
    ::close(lock_fd);
  for (const auto& [_, lock_fd] : archive_lock_fds_)
    ::close(lock_fd);
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
  const absl::Status snapshot_status = ensure_user_config_snapshot();
  if (!snapshot_status.ok()) {
    std::cerr << snapshot_status << '\n';
    return p.string();
  }
  return (resolved_game_dir() / p).string();
}

absl::Status Configurator::ensure_user_config_snapshot() {
  if (user_config_snapshot_.has_value() && resolved_game_dir_.has_value())
    return absl::OkStatus();

  YAML::Node user_overlay;
  HM_ASSIGN_OR_RETURN(user_overlay, user_config::load_or_create());
  auto root = user_config::game_root(user_overlay);
  if (!root.ok())
    return root.status();
  user_config_snapshot_ = YAML::Clone(user_overlay);
  resolved_game_dir_ = game_id_.empty() ? fs::path() : *root / game_id_;
  return absl::OkStatus();
}

std::filesystem::path Configurator::resolved_game_dir() {
  const absl::Status status = ensure_user_config_snapshot();
  if (!status.ok()) {
    std::cerr << status << '\n';
    return game_id_.empty() ? fs::path() : fs::path("/games") / game_id_;
  }
  return *resolved_game_dir_;
}

std::filesystem::path Configurator::get_game_dir(const std::string& game_id) {
  if (game_id.empty()) {
    return {};
  }
  YAML::Node user_overlay(YAML::NodeType::Map);
  auto loaded = user_config::load_or_create();
  if (loaded.ok()) {
    user_overlay = *loaded;
  } else {
    std::cerr << loaded.status() << '\n';
  }
  auto root = user_config::game_root(user_overlay);
  if (root.ok())
    return *root / game_id;
  std::cerr << root.status() << '\n';
  return fs::path("/games") / game_id;
}

std::filesystem::path Configurator::get_private_config_file_name(const std::string& game_id) {
  return get_game_dir(game_id) / "config.yaml";
}

absl::StatusOr<std::optional<YAML::Node>> Configurator::load_private_config() {
  HM_RETURN_IF_ERROR(ensure_user_config_snapshot());
  const fs::path private_config_file = resolved_game_dir() / "config.yaml";
  if (private_config_file.parent_path().empty() || !fs::is_directory(private_config_file.parent_path())) {
    return std::nullopt;
  }
  return stitching::load_game_config_file(private_config_file);
}

absl::Status Configurator::save_private_config(
    const YAML::Node& private_config,
    const std::string& expected_invalidation_id,
    bool remove_rink_masks) {
  HM_RETURN_IF_ERROR(ensure_user_config_snapshot());
  const fs::path game_dir = resolved_game_dir();
  auto config_lock = stitching::GameConfigTransactionLock::Acquire(game_dir);
  if (!config_lock.ok())
    return config_lock.status();
  const fs::path private_config_file = game_dir / "config.yaml";
  YAML::Node latest;
  try {
    if (fs::is_regular_file(private_config_file))
      latest = YAML::LoadFile(private_config_file.string());
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Failed to merge private config: " + std::string(error.what()));
  }
  HM_RETURN_IF_ERROR(stitching::validate_stitching_generation_owner(latest, expected_invalidation_id));
  const YAML::Node merged = stitching::apply_game_config_diff(persisted_private_config_, private_config, latest);
  std::string contents;
  if (!is_empty_yaml_document(merged))
    contents = YAML::Dump(merged) + "\n";
  absl::Status status;
  if (remove_rink_masks) {
    auto removed = stitching::publish_game_config_without_rink_masks(game_dir, contents);
    status = removed.ok() ? absl::OkStatus() : removed.status();
  } else {
    status = stitching::publish_game_config(game_dir, contents);
  }
  if (status.ok())
    persisted_private_config_ = YAML::Clone(private_config);
  return status;
}

absl::Status Configurator::persist_stitch_frame_time_override(const std::string& normalized_stitch_frame_time) {
  const std::string requested = normalized_stitch_frame_time.empty() ? "00:00:00" : normalized_stitch_frame_time;
  uint64_t requested_time_ns = 0;
  uint64_t lower_layer_time_ns = 0;
  try {
    requested_time_ns = stitch_frame_time_to_nanoseconds(requested);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid stitch-frame override: " + std::string(error.what()));
  }
  HM_ASSIGN_OR_RETURN(lower_layer_time_ns, private_stitch_frame_time(lower_layer_config_));
  config_["stitching"]["stitch_frame_time"] = requested;
  if (requested_time_ns == lower_layer_time_ns) {
    remove_yaml_key_path(private_config_, {"stitching", "stitch_frame_time"});
  } else {
    private_config_["stitching"]["stitch_frame_time"] = requested;
  }

  std::string expected_invalidation_id;
  try {
    const std::string status = get_node_value(config_, "hstream_ui.stitching_calibration.status", std::string());
    const std::string invalidation_id =
        get_node_value(config_, "hstream_ui.stitching_calibration.invalidation_id", std::string());
    if ((status == "pending" || status == "complete") && !invalidation_id.empty()) {
      expected_invalidation_id = invalidation_id;
    }
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Unable to persist stitch-frame override: " + std::string(error.what()));
  }
  return save_private_config(private_config_, expected_invalidation_id);
}

absl::Status Configurator::persist_effective_stitching_backend_choices(const std::string& expected_invalidation_id) {
  const auto canonical_matcher = [](const YAML::Node& root) -> absl::StatusOr<std::string> {
    const auto node = get_node(root, "stitching.control_point_matcher");
    if (node.has_value() && !node->IsScalar()) {
      return absl::InvalidArgumentError("stitching.control_point_matcher must be a scalar value");
    }
    stitching::ControlPointMatcher matcher = stitching::ControlPointMatcher::kSuperPointLightGlue;
    HM_ASSIGN_OR_RETURN(
        matcher, stitching::ParseControlPointMatcher(node.has_value() ? node->as<std::string>() : std::string()));
    return std::string(stitching::ControlPointMatcherName(matcher));
  };
  const auto canonical_backend = [](const YAML::Node& root) -> absl::StatusOr<std::string> {
    const auto node = get_node(root, "stitching.mapping_backend");
    if (node.has_value() && !node->IsScalar()) {
      return absl::InvalidArgumentError("stitching.mapping_backend must be a scalar value");
    }
    stitching::MappingBackend backend = stitching::MappingBackend::kNona;
    HM_ASSIGN_OR_RETURN(
        backend, stitching::ParseMappingBackend(node.has_value() ? node->as<std::string>() : std::string()));
    return std::string(stitching::MappingBackendName(backend));
  };

  std::string matcher_name;
  HM_ASSIGN_OR_RETURN(matcher_name, canonical_matcher(config_));
  std::string backend_name;
  HM_ASSIGN_OR_RETURN(backend_name, canonical_backend(config_));

  const auto generated_matcher =
      get_node(private_config_, "hstream_ui.generated_stitching_backend_choices.control_point_matcher");
  const auto generated_backend =
      get_node(private_config_, "hstream_ui.generated_stitching_backend_choices.mapping_backend");
  const auto private_matcher = get_node(private_config_, "stitching.control_point_matcher");
  const auto private_backend = get_node(private_config_, "stitching.mapping_backend");
  const auto persisted_matcher = get_node(persisted_private_config_, "stitching.control_point_matcher");
  const auto persisted_backend = get_node(persisted_private_config_, "stitching.mapping_backend");
  const auto persisted_generated_matcher =
      get_node(persisted_private_config_, "hstream_ui.generated_stitching_backend_choices.control_point_matcher");
  const auto persisted_generated_backend =
      get_node(persisted_private_config_, "hstream_ui.generated_stitching_backend_choices.mapping_backend");
  const auto persisted_previous_matcher = get_node(
      persisted_private_config_, "hstream_ui.generated_stitching_backend_choices.previous_control_point_matcher");
  const auto persisted_previous_backend =
      get_node(persisted_private_config_, "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend");
  const bool private_values_present = private_matcher.has_value() && private_matcher->IsScalar() &&
      private_backend.has_value() && private_backend->IsScalar();
  const bool persisted_matcher_present = persisted_matcher.has_value() && persisted_matcher->IsScalar();
  const bool persisted_backend_present = persisted_backend.has_value() && persisted_backend->IsScalar();
  const bool persisted_values_present = persisted_matcher_present && persisted_backend_present;
  const bool persisted_values_are_generated = persisted_generated_matcher.has_value() &&
      persisted_generated_matcher->IsScalar() && persisted_generated_backend.has_value() &&
      persisted_generated_backend->IsScalar() && persisted_values_present &&
      persisted_matcher->as<std::string>() == persisted_generated_matcher->as<std::string>() &&
      persisted_backend->as<std::string>() == persisted_generated_backend->as<std::string>();
  const bool generated_private_values = generated_matcher.has_value() && generated_matcher->IsScalar() &&
      generated_backend.has_value() && generated_backend->IsScalar() && private_values_present &&
      private_matcher->as<std::string>() == generated_matcher->as<std::string>() &&
      private_backend->as<std::string>() == generated_backend->as<std::string>();
  const bool effective_values_are_generated_private = generated_private_values &&
      matcher_name == private_matcher->as<std::string>() && backend_name == private_backend->as<std::string>();
  if (effective_values_are_generated_private) {
    HM_ASSIGN_OR_RETURN(matcher_name, canonical_matcher(lower_layer_config_));
    HM_ASSIGN_OR_RETURN(backend_name, canonical_backend(lower_layer_config_));
  }

  config_["stitching"]["control_point_matcher"] = matcher_name;
  config_["stitching"]["mapping_backend"] = backend_name;

  if (restored_generated_stitching_backend_choices_ && explicit_value_rank("stitching.control_point_matcher") < 3 &&
      explicit_value_rank("stitching.mapping_backend") < 3) {
    HM_RETURN_IF_ERROR(save_private_config(private_config_, expected_invalidation_id));
    loaded_generated_stitching_backend_choices_ = false;
    restored_generated_stitching_backend_choices_ = false;
    return absl::OkStatus();
  }

  const bool private_matches = private_matcher.has_value() && private_matcher->IsScalar() &&
      private_matcher->as<std::string>() == matcher_name && private_backend.has_value() &&
      private_backend->IsScalar() && private_backend->as<std::string>() == backend_name;
  if (private_matches) {
    if (loaded_generated_stitching_backend_choices_) {
      HM_RETURN_IF_ERROR(save_private_config(private_config_, expected_invalidation_id));
      loaded_generated_stitching_backend_choices_ = false;
    }
    return absl::OkStatus();
  }

  private_config_["stitching"]["control_point_matcher"] = matcher_name;
  private_config_["stitching"]["mapping_backend"] = backend_name;
  if (private_values_present && !generated_private_values &&
      explicit_value_rank("stitching.control_point_matcher") < 3 &&
      explicit_value_rank("stitching.mapping_backend") < 3) {
    remove_yaml_key_path(private_config_, {"hstream_ui", "generated_stitching_backend_choices"});
  } else {
    private_config_["hstream_ui"]["generated_stitching_backend_choices"]["control_point_matcher"] = matcher_name;
    private_config_["hstream_ui"]["generated_stitching_backend_choices"]["mapping_backend"] = backend_name;
    if (persisted_values_are_generated && persisted_previous_matcher.has_value() &&
        persisted_previous_matcher->IsScalar()) {
      private_config_["hstream_ui"]["generated_stitching_backend_choices"]["previous_control_point_matcher"] =
          persisted_previous_matcher->as<std::string>();
    } else if (persisted_matcher_present && !persisted_values_are_generated) {
      private_config_["hstream_ui"]["generated_stitching_backend_choices"]["previous_control_point_matcher"] =
          persisted_matcher->as<std::string>();
    } else {
      remove_yaml_key_path(
          private_config_, {"hstream_ui", "generated_stitching_backend_choices", "previous_control_point_matcher"});
    }
    if (persisted_values_are_generated && persisted_previous_backend.has_value() &&
        persisted_previous_backend->IsScalar()) {
      private_config_["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"] =
          persisted_previous_backend->as<std::string>();
    } else if (persisted_backend_present && !persisted_values_are_generated) {
      private_config_["hstream_ui"]["generated_stitching_backend_choices"]["previous_mapping_backend"] =
          persisted_backend->as<std::string>();
    } else {
      remove_yaml_key_path(
          private_config_, {"hstream_ui", "generated_stitching_backend_choices", "previous_mapping_backend"});
    }
  }
  HM_RETURN_IF_ERROR(save_private_config(private_config_, expected_invalidation_id));
  loaded_generated_stitching_backend_choices_ = false;
  return absl::OkStatus();
}

absl::StatusOr<bool> Configurator::reconcile_stitch_frame_time_override(
    const std::string& normalized_stitch_frame_time,
    const std::string& expected_invalidation_id) {
  const std::string requested = normalized_stitch_frame_time.empty() ? "00:00:00" : normalized_stitch_frame_time;
  uint64_t requested_time_ns = 0;
  try {
    requested_time_ns = stitch_frame_time_to_nanoseconds(requested);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid stitch-frame override: " + std::string(error.what()));
  }

  // A config-only invocation can use the CLI timestamp for positioning, but
  // has no game-private config.yaml to own. Avoid treating the game-root
  // directory itself as a game and writing state there.
  if (game_id_.empty())
    return false;

  const fs::path game_dir = resolved_game_dir();
  auto config_transaction = stitching::GameConfigTransactionLock::Acquire(game_dir);
  if (!config_transaction.ok())
    return config_transaction.status();

  const fs::path private_config_file = game_dir / "config.yaml";
  YAML::Node latest(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(private_config_file))
      latest = YAML::LoadFile(private_config_file.string());
    if (!latest || latest.IsNull())
      latest = YAML::Node(YAML::NodeType::Map);
    if (!latest.IsMap())
      return absl::InvalidArgumentError("Game-private config.yaml must contain a YAML map");
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Failed to load private stitch-frame state: " + std::string(error.what()));
  }

  uint64_t lower_layer_time_ns = 0;
  HM_ASSIGN_OR_RETURN(lower_layer_time_ns, private_stitch_frame_time(lower_layer_config_));
  const bool private_value_present = get_node(latest, "stitching.stitch_frame_time").has_value();
  uint64_t current_time_ns = lower_layer_time_ns;
  if (private_value_present)
    HM_ASSIGN_OR_RETURN(current_time_ns, private_stitch_frame_time(latest));
  const bool changed = current_time_ns != requested_time_ns;
  const bool should_persist = requested_time_ns != lower_layer_time_ns;
  const bool persistence_changed = private_value_present != should_persist;
  if (changed || persistence_changed) {
    std::string invalidation_id = expected_invalidation_id;
    if (!invalidation_id.empty()) {
      const YAML::Node current_calibration = latest["hstream_ui"]["stitching_calibration"];
      const std::string current_status = current_calibration["status"] && current_calibration["status"].IsScalar()
          ? current_calibration["status"].as<std::string>()
          : std::string();
      const std::string current_owner =
          current_calibration["invalidation_id"] && current_calibration["invalidation_id"].IsScalar()
          ? current_calibration["invalidation_id"].as<std::string>()
          : std::string();
      if ((current_status != "pending" && current_status != "complete") || current_owner != invalidation_id) {
        return absl::AbortedError("Stitch-frame override invalidation owner was superseded before reconciliation");
      }
    } else if (changed) {
      gchar* generated_invalidation_id = g_uuid_string_random();
      if (!generated_invalidation_id)
        return absl::InternalError("Unable to create a stitch-frame invalidation ID");
      invalidation_id = generated_invalidation_id;
      g_free(generated_invalidation_id);
    }

    if (should_persist)
      latest["stitching"]["stitch_frame_time"] = requested;
    else
      remove_yaml_key_path(latest, {"stitching", "stitch_frame_time"});

    if (changed) {
      size_t control_points = kDefaultStitchingControlPoints;
      if (get_node(latest, "hstream_ui.stitching_calibration.control_points").has_value()) {
        HM_ASSIGN_OR_RETURN(control_points, persisted_stitching_control_points(latest));
      } else if (const char* configured = g_getenv("HM_MAX_CONTROL_POINTS"); configured && *configured) {
        if (!std::all_of(configured, configured + std::strlen(configured), [](unsigned char character) {
              return std::isdigit(character);
            })) {
          return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive integer");
        }
        size_t consumed = 0;
        unsigned long long parsed = 0;
        try {
          parsed = std::stoull(configured, &consumed);
        } catch (const std::exception&) {
          return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive integer");
        }
        if (consumed != std::strlen(configured) || parsed == 0 || parsed > std::numeric_limits<size_t>::max()) {
          return absl::InvalidArgumentError("HM_MAX_CONTROL_POINTS must be a positive platform-sized integer");
        }
        control_points = static_cast<size_t>(parsed);
      }
      auto frame_count_or = saved_or_environment_stitching_calibration_frame_count(latest);
      if (!frame_count_or.ok())
        return frame_count_or.status();
      const size_t frame_count = *frame_count_or;

      YAML::Node calibration = latest["hstream_ui"]["stitching_calibration"];
      calibration["control_points"] = control_points;
      calibration["frame_count"] = frame_count;
      calibration["status"] = "pending";
      calibration["rink_mask_status"] = "pending";
      calibration["stale_from"] = "input";
      calibration["artifacts_invalidated"] = false;
      calibration["invalidation_id"] = invalidation_id;
    }

    const absl::Status publish = stitching::publish_game_config(game_dir, YAML::Dump(latest) + "\n");
    if (!publish.ok())
      return publish;
  }

  private_config_ = YAML::Clone(latest);
  persisted_private_config_ = YAML::Clone(latest);
  config_["stitching"]["stitch_frame_time"] = requested;
  const auto calibration = get_node(latest, "hstream_ui.stitching_calibration");
  if (calibration.has_value()) {
    config_["hstream_ui"]["stitching_calibration"] = YAML::Clone(*calibration);
  } else {
    remove_yaml_key_path(config_, {"hstream_ui", "stitching_calibration"});
  }
  return changed;
}

absl::StatusOr<YAML::Node> Configurator::load_config() {
  const auto baseline = baseline_config::load_from_root(config_root_dir_);
  if (!baseline.ok())
    return baseline.status();
  YAML::Node config = YAML::Clone(baseline->values);
  HM_RETURN_IF_ERROR(ensure_user_config_snapshot());
  const YAML::Node user_overlay = YAML::Clone(*user_config_snapshot_);
  explicit_value_ranks_.clear();
  record_explicit_overlay(user_overlay, {}, 1);
  config = merge_nodes(
      config,
      user_overlay,
      /*warn_if_key_not_in_dest=*/false);
  lower_layer_config_ = YAML::Clone(config);
  std::optional<YAML::Node> private_config;
  HM_ASSIGN_OR_RETURN(private_config, load_private_config());
  loaded_generated_stitching_backend_choices_ = false;
  restored_generated_stitching_backend_choices_ = false;
  if (private_config.has_value()) {
    const YAML::Node original_private_config = YAML::Clone(*private_config);
    private_config_ = YAML::Clone(*private_config);
    restored_generated_stitching_backend_choices_ =
        get_node(private_config_, "hstream_ui.generated_stitching_backend_choices.previous_control_point_matcher")
            .has_value() ||
        get_node(private_config_, "hstream_ui.generated_stitching_backend_choices.previous_mapping_backend")
            .has_value();
    loaded_generated_stitching_backend_choices_ = normalize_generated_stitching_backend_choices(private_config_);
    restored_generated_stitching_backend_choices_ =
        restored_generated_stitching_backend_choices_ && loaded_generated_stitching_backend_choices_;
    persisted_private_config_ =
        YAML::Clone(loaded_generated_stitching_backend_choices_ ? original_private_config : private_config_);
    record_explicit_overlay(private_config_, {}, 2);
    config = merge_nodes(
        config,
        private_config_,
        /*warn_if_key_not_in_dest=*/!config);
  } else {
    private_config_ = YAML::Node(YAML::NodeType::Map);
    persisted_private_config_ = YAML::Node(YAML::NodeType::Map);
  }
  return config;
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
  record_explicit_overlay(overlaid_config, node_name, 2);
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

void Configurator::record_explicit_overlay(const YAML::Node& overlay, const std::string& prefix, int rank) {
  if (!overlay.IsMap()) {
    if (!prefix.empty())
      explicit_value_ranks_[prefix] = rank;
    return;
  }
  if (overlay.size() == 0 && !prefix.empty()) {
    explicit_value_ranks_[prefix] = rank;
    return;
  }
  for (const auto& entry : overlay) {
    const std::string key = entry.first.as<std::string>();
    const std::string path = prefix.empty() ? key : prefix + "." + key;
    record_explicit_overlay(entry.second, path, rank);
  }
}

int Configurator::explicit_value_rank(const std::string& path) const {
  const auto found = explicit_value_ranks_.find(path);
  return found == explicit_value_ranks_.end() ? -1 : found->second;
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

absl::Status Configurator::complete_configuration(
    bool force,
    bool clean_stitching_artifacts,
    bool clean_stitching_from_control_points,
    const std::string& clean_expected_invalidation_id,
    bool show_render_sink,
    double show_render_scale,
    const fs::path& pipeline_config_dir) {
  active_stitching_invalidation_id_.clear();
  stitching_calibration_required_ = false;
  const bool clean_requested = clean_stitching_artifacts || clean_stitching_from_control_points;
  const bool clean_from_control_points_only = clean_stitching_from_control_points && !clean_stitching_artifacts;
  const bool complete_configuration_enabled =
      get_node_value(config_, "pipeline.application.complete-configuration", false);
  YAML::Node pipeline = config_["pipeline"];
  if (!pipeline.IsDefined()) {
    return complete_configuration_enabled ? absl::InvalidArgumentError("Configuration has no pipeline section")
                                          : absl::OkStatus();
  }

  if (game_id_.empty() && clean_requested)
    return absl::InvalidArgumentError("No game id specified for cleaning");

  if (!clean_requested) {
    apply_gpu_override(pipeline);
    HM_RETURN_IF_ERROR(apply_supported_baseline_mappings());
  }

  HM_RETURN_IF_ERROR(ensure_user_config_snapshot());
  const fs::path game_dir = resolved_game_dir();
  if (!clean_requested)
    HM_RETURN_IF_ERROR(materialize_playtracker_config(pipeline, game_dir, pipeline_config_dir));

  if (!complete_configuration_enabled || game_id_.empty()) {
    // Raw/no-game and structurally incomplete launches still require the
    // canonical tracker materialization above, but skip game discovery and
    // dependent stitching configuration.
    return absl::OkStatus();
  }

  // Stitching config mask config dir. Cleanup ownership is explicit, while
  // enabled state owns every runtime stitching operation.
  const bool has_hmstitcher_section = has_node(pipeline, "hmstitcher", false);
  const bool has_active_hmstitcher = has_hmstitcher_section && get_node_value(pipeline, "hmstitcher.enable", false);
  const bool has_stitching_cleanup_owner = configurator_internal::hmstitcher_owns_stitching_cleanup(config_);
  if (clean_requested && !has_hmstitcher_section) {
    return absl::FailedPreconditionError("No hmstitcher section is configured; nothing to clean");
  }
  if (clean_requested && !has_stitching_cleanup_owner) {
    return absl::FailedPreconditionError("No active hmstitcher configuration is eligible for cleaning");
  }
  const std::string loaded_invalidation_id =
      get_node_value(config_, "hstream_ui.stitching_calibration.invalidation_id", std::string());
  const std::string loaded_status = get_node_value(config_, "hstream_ui.stitching_calibration.status", std::string());
  const std::string loaded_stale_from =
      get_node_value(config_, "hstream_ui.stitching_calibration.stale_from", std::string());
  const bool resume_pending_invalidation = has_active_hmstitcher && clean_expected_invalidation_id.empty() &&
      loaded_status == "pending" && !loaded_invalidation_id.empty();
  const std::string effective_invalidation_id =
      resume_pending_invalidation ? loaded_invalidation_id : clean_expected_invalidation_id;
  std::optional<size_t> loaded_control_points;
  std::optional<size_t> loaded_frame_count;
  bool control_points_environment_enforced = false;
  bool frame_count_environment_enforced = false;
  bool stitching_artifacts_precleaned = false;
  const bool has_cleanup_owner = clean_requested ? has_stitching_cleanup_owner : has_active_hmstitcher;
  if (!clean_requested && has_active_hmstitcher) {
    HM_RETURN_IF_ERROR(persist_effective_stitching_backend_choices(effective_invalidation_id));
  }
  if (has_cleanup_owner && !effective_invalidation_id.empty()) {
    bool loaded_invalidation_matches = loaded_invalidation_id == effective_invalidation_id;
    bool loaded_artifacts_invalidated = false;
    if (loaded_status == "pending") {
      HM_ASSIGN_OR_RETURN(
          loaded_artifacts_invalidated,
          get_yaml_bool_value(config_, "hstream_ui.stitching_calibration.artifacts_invalidated", false));
    } else {
      loaded_invalidation_matches = loaded_invalidation_matches && loaded_status == "complete" && !clean_requested;
    }
    if (!loaded_invalidation_matches) {
      return absl::AbortedError("Loaded stitching configuration was superseded before configuration");
    }
    if (loaded_status == "pending") {
      size_t control_points = 0;
      HM_ASSIGN_OR_RETURN(control_points, persisted_stitching_control_points(config_));
      loaded_control_points = control_points;
      HM_ASSIGN_OR_RETURN(loaded_frame_count, persisted_stitching_calibration_frame_count(config_));
    }

    // load_config() and this final launch boundary are separated by command-line
    // overlays and sub-config loading. Revalidate the on-disk generation while
    // holding its transaction lock so a newer UI invalidation cannot start a
    // pipeline that owns only the stale in-memory document.
    auto config_transaction = stitching::GameConfigTransactionLock::Acquire(game_dir);
    if (!config_transaction.ok())
      return config_transaction.status();
    const fs::path private_config_file = game_dir / "config.yaml";
    HM_RETURN_IF_ERROR(
        stitching::validate_stitching_generation_owner_file_locked(private_config_file, effective_invalidation_id));
    try {
      const YAML::Node current = YAML::LoadFile(private_config_file.string());
      const std::string current_status =
          get_node_value(current, "hstream_ui.stitching_calibration.status", std::string());
      if (current_status != loaded_status) {
        return absl::AbortedError("Stitching configuration state changed before pipeline launch");
      }
      if (current_status == "pending") {
        size_t current_control_points = 0;
        HM_ASSIGN_OR_RETURN(current_control_points, persisted_stitching_control_points(current));
        if (!loaded_control_points.has_value() || current_control_points != *loaded_control_points) {
          return absl::AbortedError("Stitching control-point limit changed before pipeline launch");
        }
        size_t current_frame_count = 0;
        HM_ASSIGN_OR_RETURN(current_frame_count, persisted_stitching_calibration_frame_count(current));
        if (!loaded_frame_count.has_value() || current_frame_count != *loaded_frame_count) {
          return absl::AbortedError("Stitching calibration frame count changed before pipeline launch");
        }
        bool current_artifacts_invalidated = false;
        HM_ASSIGN_OR_RETURN(
            current_artifacts_invalidated,
            get_yaml_bool_value(current, "hstream_ui.stitching_calibration.artifacts_invalidated", false));
        if (current_artifacts_invalidated != loaded_artifacts_invalidated) {
          return absl::AbortedError("Stitching cleanup state changed before pipeline launch");
        }
        stitching_artifacts_precleaned = current_artifacts_invalidated;
      }
    } catch (const YAML::Exception& error) {
      return absl::InvalidArgumentError(
          "Unable to revalidate stitching configuration before launch: " + std::string(error.what()));
    }
    if (loaded_control_points.has_value()) {
      HM_RETURN_IF_ERROR(enforce_stitching_control_points_environment(*loaded_control_points));
      control_points_environment_enforced = true;
    }
    if (loaded_frame_count.has_value()) {
      HM_RETURN_IF_ERROR(enforce_stitching_calibration_frame_count_environment(*loaded_frame_count));
      frame_count_environment_enforced = true;
    }
    active_stitching_invalidation_id_ = effective_invalidation_id;
  }
  // Any owned pending generation that has not published cleanup yet must be
  // cleaned before dependency inspection. This includes explicit guarded
  // launches: a direct CLI stitch-frame change can reuse the caller's owner
  // while still requiring a full input-stage invalidation.
  const bool auto_clean_pending_invalidation = has_active_hmstitcher && loaded_status == "pending" &&
      !effective_invalidation_id.empty() && !stitching_artifacts_precleaned;
  const bool effective_clean_from_control_points =
      clean_from_control_points_only || (auto_clean_pending_invalidation && loaded_stale_from == "features");
  const bool should_clean_stitching =
      clean_requested || auto_clean_pending_invalidation || (force && !stitching_artifacts_precleaned);
  if (has_cleanup_owner && should_clean_stitching) {
    YAML::Node preserved_pipeline = config_["pipeline"];
    absl::Status clean_status = effective_clean_from_control_points
        ? stitching::clean_stitching_artifacts_from_control_points(game_dir.string(), effective_invalidation_id)
        : stitching::clean_stitching_artifacts(game_dir.string(), effective_invalidation_id);
    if (!clean_status.ok()) {
      if (clean_requested || auto_clean_pending_invalidation || (force && !effective_invalidation_id.empty())) {
        return clean_status;
      }
      std::cerr << "Warning: failed to clean stitching artifacts: " << clean_status << std::endl;
    } else {
      if (effective_clean_from_control_points) {
        remove_control_point_dependent_stitching_cache_keys(config_);
        remove_control_point_dependent_stitching_cache_keys(private_config_);
      } else {
        remove_cleanable_stitching_cache_keys(config_);
        remove_cleanable_stitching_cache_keys(private_config_);
      }
      if (preserved_pipeline.IsDefined()) {
        config_["pipeline"] = preserved_pipeline;
      }
      // clean_stitching_artifacts already published the merged private YAML.
      // Keep this process's snapshot aligned without overwriting concurrent
      // config owners with the stale pre-clean document.
      persisted_private_config_ = YAML::Clone(private_config_);
    }
  }

  if (has_hmstitcher_section) {
    pipeline["hmstitcher"]["force-scoreboard-config"] = force ? "1" : "0";
  }
  if (pipeline["hmplaycropper"].IsDefined()) {
    pipeline["hmplaycropper"]["config-file"] = std::string(game_dir);
  }

  if (clean_requested) {
    return absl::CancelledError("Stitching artifacts cleaned");
  }

  std::map<int, YAML::Node> camera_sources;
  HM_ASSIGN_OR_RETURN(camera_sources, get_camera_sources(pipeline));
  const bool is_camera_source = !camera_sources.empty();

  bool pipeline_has_hmstitcher = has_active_hmstitcher;
  if (pipeline_has_hmstitcher) {
    HM_RETURN_IF_ERROR(invalidate_rotation_dependent_cache_if_needed(game_dir));
    HM_RETURN_IF_ERROR(invalidate_canvas_dependent_cache_if_needed(game_dir));
  }
  // Cache invalidation above can remove a previously valid rink mask. Compute
  // the one-pass startup position only after the effective runtime rotation
  // and canvas constraints have been applied.
  HM_RETURN_IF_ERROR(setup_stitcher_and_masks(pipeline, game_dir, force, pipeline_has_hmstitcher));
  if (stitching_calibration_required_) {
    if (!loaded_control_points.has_value() && loaded_status == "complete") {
      size_t control_points = 0;
      HM_ASSIGN_OR_RETURN(control_points, persisted_stitching_control_points(config_));
      size_t frame_count = 0;
      HM_ASSIGN_OR_RETURN(frame_count, persisted_stitching_calibration_frame_count(config_));

      auto config_transaction = stitching::GameConfigTransactionLock::Acquire(game_dir);
      if (!config_transaction.ok())
        return config_transaction.status();
      const fs::path private_config_file = game_dir / "config.yaml";
      try {
        const YAML::Node current = YAML::LoadFile(private_config_file.string());
        const std::string current_status =
            get_node_value(current, "hstream_ui.stitching_calibration.status", std::string());
        const std::string current_invalidation_id =
            get_node_value(current, "hstream_ui.stitching_calibration.invalidation_id", std::string());
        size_t current_control_points = 0;
        HM_ASSIGN_OR_RETURN(current_control_points, persisted_stitching_control_points(current));
        size_t current_frame_count = 0;
        HM_ASSIGN_OR_RETURN(current_frame_count, persisted_stitching_calibration_frame_count(current));
        if (current_status != loaded_status || current_invalidation_id != loaded_invalidation_id ||
            current_control_points != control_points || current_frame_count != frame_count) {
          return absl::AbortedError("Completed stitching calibration state changed before pipeline launch");
        }
      } catch (const YAML::Exception& error) {
        return absl::InvalidArgumentError(
            "Unable to revalidate completed stitching calibration before launch: " + std::string(error.what()));
      }
      loaded_control_points = control_points;
      loaded_frame_count = frame_count;
    }
    if (loaded_control_points.has_value() && !control_points_environment_enforced) {
      HM_RETURN_IF_ERROR(enforce_stitching_control_points_environment(*loaded_control_points));
    }
    if (loaded_frame_count.has_value() && !frame_count_environment_enforced) {
      HM_RETURN_IF_ERROR(enforce_stitching_calibration_frame_count_environment(*loaded_frame_count));
    }
  }

  YAML::Node offsets = ensure_game_frame_offsets_node(config_);

  size_t area = 0, ww = 0, hh = 0;

  size_t num_video_sources = 0;

  std::vector<std::string> left_files;
  std::vector<std::string> right_files;

  if (!is_camera_source && pipeline_has_hmstitcher) {
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
  HM_RETURN_IF_ERROR(apply_hmstitcher_calibration_sample_span(pipeline, config_));
  if (pipeline_has_hmstitcher && get_node_value<int>(pipeline, "hmstitcher.enable", false)) {
    // URI playlist construction selects HStream's full-batch-only new mux for two-camera file stitching. The legacy
    // mux uses -1 for the same infinite wait. Decode and stitcher sequence guards turn a missing peer into a hard
    // pipeline error instead of allowing either mux to recover by emitting an incomplete camera pair.
    pipeline["streammux"]["sync-inputs"] = "0";
    pipeline["streammux"]["batched-push-timeout"] =
        g_strcmp0(g_getenv("USE_NEW_NVSTREAMMUX"), "yes") == 0 ? "1000000" : "-1";
    pipeline["streammux"]["frame-num-reset-on-stream-reset"] = "0";
    pipeline["streammux"]["frame-num-reset-on-eos"] = "0";
  }
  std::vector<std::string> source_video_paths;
  source_video_paths.reserve(left_files.size() + right_files.size());
  for (const std::string& path : left_files) {
    source_video_paths.emplace_back(file_maybe_in_game_dir(path));
  }
  for (const std::string& path : right_files) {
    source_video_paths.emplace_back(file_maybe_in_game_dir(path));
  }
  const auto append_source_uri = [this, &source_video_paths](const std::string& uri) {
    const std::optional<std::string> path = local_video_path_from_uri(uri);
    if (path.has_value() && !path->empty()) {
      source_video_paths.emplace_back(file_maybe_in_game_dir(*path));
    }
  };
  for (const std::string& uri : configurator_internal::enabled_source_video_uris(pipeline)) {
    append_source_uri(uri);
  }
  HM_RETURN_IF_ERROR(configure_encode_file_outputs(pipeline, source_video_paths));

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
  if (is_video_converter_config_option(key)) {
    return absl::InvalidArgumentError("Video converter selection must be set in YAML config files");
  }
  YAML::Node overlaid_config = YAML::Node(YAML::NodeType::Map);
  overlaid_config = set_node_value(overlaid_config, key, value);
  // std::cout << overlaid_config << std::endl;
  //  std::cout << config_ << std::endl;
  config_ = merge_nodes(config_, overlaid_config, /*warn_if_key_not_in_dest=*/false);
  record_explicit_overlay(overlaid_config, {}, 3);
  // std::cout << config_ << std::endl;
  return absl::OkStatus();
}

absl::Status Configurator::prepare_initial_pipeline_position(
    NvDsPipeline& pipeline,
    const NvDsConfig& config,
    uint64_t start_time_ns) {
  const bool needs_position =
      start_time_ns || config.hmsticher_config.left_frame_offset_ns || config.hmsticher_config.right_frame_offset_ns;
  if (!needs_position) {
    return absl::OkStatus();
  }
  if (pipeline.multi_src_bin.uri_playlist_exact_pairing_enabled) {
    guint audio_source_id = G_MAXUINT;
    // YAML section indices can be sparse (for example hmaudio3 with no hmaudio0), so the parsed count is not a safe
    // upper bound for finding the source-linked audio stream.
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      const NvDsHmAudioConfig& audio_config = config.hmaudio_config[i];
      if (!audio_config.enable || audio_config.src != SRC_SOURCE_BIN) {
        continue;
      }
      if (audio_source_id != G_MAXUINT && audio_source_id != audio_config.source_id) {
        return absl::FailedPreconditionError("Lossless startup requires one selected URI-playlist audio source");
      }
      audio_source_id = audio_config.source_id;
    }
    if (audio_source_id == G_MAXUINT) {
      audio_source_id = config.hmsticher_config.left_frame_offset_ns == 0 ? 0 : 1;
    }
    if (audio_source_id >= 2 || (audio_source_id == 0 && config.hmsticher_config.left_frame_offset_ns != 0) ||
        (audio_source_id == 1 && config.hmsticher_config.right_frame_offset_ns != 0)) {
      return absl::FailedPreconditionError(
          "Lossless startup audio must use the camera with zero stitching frame offset");
    }
    if (!configure_uri_playlist_initial_offsets(
            &pipeline.multi_src_bin,
            config.hmsticher_config.left_frame_offset_ns,
            config.hmsticher_config.right_frame_offset_ns,
            audio_source_id,
            start_time_ns)) {
      return absl::FailedPreconditionError(
          "Could not configure initial camera offsets before lossless sequence admission began");
    }
    return absl::OkStatus();
  }
  const bool all_sources_are_playlists = pipeline.multi_src_bin.num_bins > 0 &&
      std::all_of(pipeline.multi_src_bin.sub_bins,
                  pipeline.multi_src_bin.sub_bins + pipeline.multi_src_bin.num_bins,
                  [](const NvDsSrcBin& source) { return source.uri_list && source.num_uri_list > 0; });
  if (start_time_ns && all_sources_are_playlists) {
    if (!configure_uri_playlist_initial_position(&pipeline.multi_src_bin, start_time_ns)) {
      return absl::FailedPreconditionError("Could not configure URI playlist position before preroll");
    }
    return absl::OkStatus();
  }
  // Non-playlist source topologies retain the established PAUSED/seek path in post_config_pipeline().
  return absl::OkStatus();
}

absl::Status Configurator::post_config_pipeline(
    NvDsPipeline& pipeline,
    const NvDsConfig& config,
    uint64_t start_time_ns) {
  const bool needs_seek =
      start_time_ns || config.hmsticher_config.left_frame_offset_ns || config.hmsticher_config.right_frame_offset_ns;
  if (!needs_seek) {
    return absl::OkStatus();
  }
  if (pipeline.multi_src_bin.uri_playlist_exact_pairing_enabled) {
    // prepare_initial_pipeline_position() configured decoded-pad trimming before preroll. A later flushing seek would
    // revoke a committed pair and violate exact camera/audio continuity. Standalone file audio is not upstream of
    // that barrier, however, and must retain --start-time positioning.
    if (!start_time_ns) {
      return absl::OkStatus();
    }
    bool has_standalone_file_audio = false;
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      const NvDsHmAudioConfig& audio_config = config.hmaudio_config[i];
      has_standalone_file_audio = has_standalone_file_audio || (audio_config.enable && audio_config.src == SRC_FILE);
    }
    if (!has_standalone_file_audio) {
      return absl::OkStatus();
    }
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      if (pipeline.instance_bins[i].hmaudio_bin.bin &&
          !seek_element(pipeline.instance_bins[i].hmaudio_bin.bin, start_time_ns)) {
        return absl::InternalError("Failed to seek standalone audio to the requested start time");
      }
    }
    return absl::OkStatus();
  }
  if (pipeline.multi_src_bin.uri_playlist_initial_offsets_configured) {
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      if (pipeline.instance_bins[i].hmaudio_bin.bin && config.hmaudio_config[i].enable &&
          config.hmaudio_config[i].src == SRC_FILE &&
          !seek_element(pipeline.instance_bins[i].hmaudio_bin.bin, start_time_ns)) {
        return absl::InternalError("Failed to seek standalone audio to the requested start time");
      }
    }
    return absl::OkStatus();
  }

  GstState state = GST_STATE_VOID_PENDING;
  GstState pending = GST_STATE_VOID_PENDING;
  (void)gst_element_get_state(pipeline.pipeline, &state, &pending, 0);
  if (state == GST_STATE_NULL || state == GST_STATE_READY) {
    if (gst_element_set_state(pipeline.pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
      return absl::InternalError("Failed to set pipeline to PAUSED before seeking");
    }
  }
  constexpr GstClockTime kPauseWaitTimeout = 5 * GST_SECOND;
  const GstStateChangeReturn wait_ret = gst_element_get_state(pipeline.pipeline, &state, &pending, kPauseWaitTimeout);
  if (wait_ret == GST_STATE_CHANGE_FAILURE) {
    return absl::InternalError("Failed while waiting for pipeline to PAUSED before seeking");
  }
  if (state != GST_STATE_PAUSED) {
    std::cerr << "Warning: pipeline did not reach PAUSED within " << (kPauseWaitTimeout / GST_SECOND) << "s"
              << " (state=" << gstStateToString(state) << ", pending=" << gstStateToString(pending)
              << "). Skipping initial seeks." << std::endl;
    return absl::OkStatus();
  }
  save_dot_file(pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline_paused");

  std::vector<GstElement*> src_bins;
  src_bins.reserve(MAX_SOURCE_BINS);
  for (size_t i = 0; i < pipeline.multi_src_bin.num_bins; ++i) {
    NvDsSrcBin& source = pipeline.multi_src_bin.sub_bins[i];
    if (source.bin && source.config &&
        (source.config->type == NV_DS_SOURCE_URI || source.config->type == NV_DS_SOURCE_URI_MULTIPLE)) {
      src_bins.push_back(source.bin);
    }
  }
  if (src_bins.size() == 2) {
    const guint64 offsets[2] = {
        config.hmsticher_config.left_frame_offset_ns, config.hmsticher_config.right_frame_offset_ns};
    for (size_t source_index = 0; source_index < 2; ++source_index) {
      if ((start_time_ns || offsets[source_index]) &&
          !seek_element(src_bins[source_index], start_time_ns + offsets[source_index])) {
        return absl::InternalError("Failed to seek a camera source to its requested initial position");
      }
    }
  } else if (start_time_ns) {
    for (GstElement* source : src_bins) {
      if (!seek_element(source, start_time_ns)) {
        return absl::InternalError("Failed to seek video source to the requested start time");
      }
    }
  }

  bool has_standalone_file_audio = false;
  for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
    const NvDsHmAudioConfig& audio_config = config.hmaudio_config[i];
    has_standalone_file_audio = has_standalone_file_audio || (audio_config.enable && audio_config.src == SRC_FILE);
  }
  if (start_time_ns && has_standalone_file_audio) {
    for (size_t i = 0; i < MAX_SOURCE_BINS; ++i) {
      if (pipeline.instance_bins[i].hmaudio_bin.bin &&
          !seek_element(pipeline.instance_bins[i].hmaudio_bin.bin, start_time_ns)) {
        return absl::InternalError("Failed to seek standalone audio to the requested start time");
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
