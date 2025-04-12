#include "configurator.h"

#include <gstreamer-1.0/gst/gstelement.h>
#include <gstreamer-1.0/gst/gstpipeline.h>
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <unistd.h>
#include <yaml-cpp/node/parse.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "absl/status/status.h"
#include "absl/strings/match.h"

#include "cupano/pano/controlMasks.h"
#include "deepstream_app.h"
#include "hstream/src/apps/apps-common/deepstream_config.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/Process.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/Orientation.h"

namespace fs = std::filesystem;

namespace hm {

namespace {

constexpr long kMaxUdpStreamingWidth = 3840;
constexpr long kMaxUdpStreamingHeight = 2160;

constexpr const char* kRinkMaskFilename = "rink_mask_0.png";

const std::vector<const char*> nostitch_video_names = {
    // Prefer mp4 to mkv
    "stitching_output-with-audio.mp4",
    "stitching_output-with-audio.mkv",
};

int as_int(const YAML::Node& node) {
  // be less asserty than YAML-CPP
  // std::cout << node << std::endl;
  if (!node.IsDefined()) {
    return 0;
  }
  std::string s = node.as<std::string>();
  return std::atoi(s.c_str());
}

void remove_whitespace_in_place(std::string& input) {
  int index = 0; // This will keep track of the position in the original string
  for (char c : input) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      input[index++] = c;
    }
  }
  input.resize(index); // Resize the string to the new length, removing the trailing whitespace
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

bool is_enabled(YAML::Node n) {
  if (!n.IsDefined()) {
    return false;
  }
  if (!n.IsMap()) {
    return false;
  }
  YAML::Node enabled = n["enable"];
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

bool has_enabled_rtsp_sink(const YAML::Node& pipeline) {
  for (size_t i = 0; i < MAX_SINK_BINS; ++i) {
    std::string sinkname = "sink" + std::to_string(i);
    if (!pipeline[sinkname].IsDefined()) {
      continue;
    }
    if (!pipeline[sinkname]["enable"].IsDefined()) {
      continue;
    }
    if (!pipeline[sinkname]["enable"].as<int>()) {
      continue;
    }
    if (pipeline[sinkname]["type"].as<int>() == NV_DS_SINK_UDPSINK) {
      return true;
    }
  }
  return false;
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
    if (audio["enable"].IsDefined() && audio["enable"].as<int>() && audio["type"].as<int>() == NV_DS_SOURCE_AUDIO_URI) {
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
      if (!get_node_value(src_node, "enable", 0)) {
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

std::map<int, YAML::Node> replace_sink_source_id(const YAML::Node& pipeline, int from_source_id, int to_source_id) {
  std::map<int, YAML::Node> sinks;
  for (auto kv : pipeline) {
    std::string key = kv.first.as<std::string>();
    if (absl::StartsWith(key, "sink")) {
      YAML::Node sink_node = kv.second;
      if (!get_node_value(sink_node, "enable", 0)) {
        continue;
      }
      if (!has_node(sink_node, "source-id", /*non_null=*/true)) {
        // Default would be zero
        if (from_source_id != 0)
          continue;
        sink_node["source-id"] = from_source_id;
      }
      if (sink_node["source-id"].as<gint>() == from_source_id) {
        sink_node["source-id"] = to_source_id;
      }
      sinks.emplace(sink_node["sink-id"].as<int>(), sink_node);
    }
  }
  return sinks;
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
  SET_LOCATOR(locator, config, enable); // "enable"
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
        src_node["enable"] = "1";
        if (!has_node(src_node, "source-id", /*non_null=*/true)) {
          std::cerr << "No source-id in enabled source section: " << key << std::endl;
          source_ids.emplace_back(std::numeric_limits<size_t>::max());
        } else {
          source_ids.emplace_back(src_node["source-id"].as<int>());
        }
      } else if (disable_others) {
        src_node["enable"] = "0";
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
        src_node["enable"] = "0";
      }
    }
  }
  return count;
}

std::string Configurator::file_maybe_in_game_dir(const std::string& basename) {
  if (std::find(basename.begin(), basename.end(), '/') != basename.end()) {
    return basename;
  }
  return get_game_dir(game_id_) / basename;
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

absl::Status Configurator::save_private_config(const YAML::Node& private_config) {
  std::string private_config_file = get_private_config_file_name(game_id_);
  std::ofstream fout(private_config_file, std::ios::out | std::ios::trunc);
  if (!fout.is_open()) {
    return absl::InternalError(TO_STRING(
        "Failed to open private config file for writing: \"" << private_config_file
                                                             << "\", reason: " << strerror(errno)));
  }
  fout << private_config << "\n";
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
    config = merge_nodes(config, private_config_, /*warn_if_key_not_in_dest=*/!config);
  } else {
    private_config_ = YAML::Node();
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
    // std::cout << config_ << std::endl;
    config_[node_name] = merge_nodes(underlaid_config, config_[node_name], /*warn_if_key_not_in_dest=*/false);
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
    config_ = merge_nodes(config_, overlaid_config, /*warn_if_key_not_in_dest=*/false);
  } else {
    config_[node_name] = merge_nodes(config_[node_name], overlaid_config, /*warn_if_key_not_in_dest=*/false);
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

absl::Status Configurator::complete_configuration(bool force) {
  YAML::Node pipeline = config_["pipeline"];
  assert(pipeline.IsDefined());

  absl::Status status;

  if (override_gpu_id_ != kUseConfigFileGpu) {
    pipeline["application"]["global-gpu-id"] = override_gpu_id_;
    set_all_field_values(pipeline, "gpu-id", std::to_string(override_gpu_id_), /*only_if_exists=*/true);
  }

  if (game_id_.empty()) {
    // return absl::InvalidArgumentError("No game id specified");
    // Just go by what's in the config file(s)
    return absl::OkStatus();
  }

  std::map<int, YAML::Node> camera_sources;
  HM_ASSIGN_OR_RETURN(camera_sources, get_camera_sources(pipeline));
  const bool is_camera_source = !camera_sources.empty();

  // Stitching config mask config dir
  fs::path game_dir = get_game_dir(game_id_);

  // Quick out if we want to configure stitching and it's already configured
  if (get_node_value(pipeline, "hmstitcher.enable", FALSE) &&
      get_node_value(pipeline, "hmstitcher.configure-only", FALSE)) {
    bool is_configured;
    HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(game_dir));
    if (is_configured && !force) {
      return absl::CancelledError("Stitching is already configured.");
    }
  }

  pipeline["hmstitcher"]["config-file"] = std::string(game_dir);

  pipeline["ds-fieldmask"]["detection-mask"] = std::string(game_dir / kRinkMaskFilename);

  pipeline["hmplaycropper"]["fixed-edge-rotation-angle"] = config_["rink"]["camera"]["fixed_edge_rotation_angle"];

  // Scoreboard perspective polygon, if there
  if (has_node(config_, "rink.scoreboard.perspective_polygon", /*non_null=*/true)) {
    auto points = config_["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>();
    if (!points.empty()) {
      assert(points.size() == 4);
      std::stringstream ss;
      for (size_t i = 0, n = points.size(); i < n; ++i) {
        if (i) {
          ss << ',';
        }
        assert(points[i].size() == 2);
        ss << std::to_string(points[i].at(0)) << ',' << points[i].at(1);
      }
      // std::cout << ss.str() << std::endl;
      pipeline["hmplaycropper"]["scoreboard-perspective-polygon"] = ss.str();
    }
  }

  YAML::Node offsets = config_["game"]["stitching"]["frame_offsets"];

  size_t area = 0, ww = 0, hh = 0;

  size_t num_video_sources = 0;

  std::vector<std::string> left_files;
  std::vector<std::string> right_files;

  if (!is_camera_source) {
    std::string stitched_file;

    stitching::VideosDict videos;
    HM_ASSIGN_OR_RETURN(videos, stitching::get_available_videos(game_dir));

    if (videos.count("stitched")) {
      if (fs::exists(videos.at("stitched").at(0))) {
        stitched_file = videos.at("stitched").at(0);
        num_video_sources = 1;
        Videoinfo stitched_info = getVideoInfo(file_maybe_in_game_dir(stitched_file));
        ww = stitched_info.width;
        hh = stitched_info.height;
        area = ww * hh;
      }
    }

    if (stitched_file.empty()) {
      // Stitching LFO, RFO

      if (!force) {
        if (has_node(config_, "game.videos.left", /*non_null=*/true)) {
          left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
        }
        if (has_node(config_, "game.videos.right", /*non_null=*/true)) {
          right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
        }
      }

      if (left_files.empty() && right_files.empty() && !videos.count("left") && !videos.count("right")) {
        HM_RETURN_IF_ERROR(stitching::configure_orientation(game_dir));
        overlay_config("", get_private_config_file_name(game_id_));
        std::cout << config_["game"]["videos"] << std::endl;
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
            // We can continue, so just warn
            std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
          }
        }
      }

      if (!left_files.empty() && !right_files.empty()) {
        if (!has_node(config_, "game.stitching.frame_offsets.left", /*non_null=*/true) || force) {
          stitching::Synchronization sync;
          HM_ASSIGN_OR_RETURN(
              sync,
              stitching::calculate_stitching_synchronization(game_dir / left_files[0], game_dir / right_files[0]));
          offsets["left"] = std::to_string(sync.video1_frame_offset);
          offsets["right"] = std::to_string(sync.video2_frame_offset);
          private_config_["game"]["stitching"]["frame_offsets"]["left"] = std::to_string(sync.video1_frame_offset);
          private_config_["game"]["stitching"]["frame_offsets"]["right"] = std::to_string(sync.video2_frame_offset);
          auto spp_status = save_private_config(private_config_);
          if (!spp_status.ok()) {
            // We can continue, so just warn
            std::cerr << "Warnings: failed to save private config: " << spp_status << std::endl;
          }
        }
      }
      if (!left_files.empty()) {
        Videoinfo left_info = getVideoInfo(file_maybe_in_game_dir(left_files[0]));
        double lfo = offsets["left"].as<double>(); // this is decimal frames
        set_stream_offsets_ |= lfo != 0.0;
        pipeline["hmstitcher"]["left-frame-offset-ns"] = std::to_string(size_t(lfo / left_info.fps * GST_SECOND));
        area = left_info.width * left_info.height;
        ww = left_info.width;
        hh = left_info.height;
      }
      if (!right_files.empty()) {
        Videoinfo right_info = getVideoInfo(file_maybe_in_game_dir(right_files[0]));
        double rfo = offsets["right"].as<double>(); // this is decimal frames
        set_stream_offsets_ |= rfo != 0.0;
        pipeline["hmstitcher"]["right-frame-offset-ns"] = std::to_string(size_t(rfo / right_info.fps * GST_SECOND));
        if (right_info.width * right_info.height > (long)area) {
          ww = right_info.width;
          hh = right_info.height;
        }
      }
    }
  }
  const bool is_udp_output = has_enabled_rtsp_sink(pipeline);

  auto maybe_scale_down = [is_udp_output](long width, long height) -> std::tuple<int, int> {
    if (!is_udp_output) {
      return std::make_tuple(width, height);
    }
    // 4k @ 16:9
    return resize_to_fit(width, height, kMaxUdpStreamingWidth, kMaxUdpStreamingHeight);
  } /* lambda maybe_scale_down() */;

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
    auto wh_tuple = maybe_scale_down(ww, hh);
    pipeline["hmplaycropper"]["output-width"] = std::to_string(std::get<0>(wh_tuple));
    pipeline["hmplaycropper"]["output-height"] = std::to_string(std::get<1>(wh_tuple));
  } else if (!left_files.empty() && !right_files.empty()) {
    auto canvas_size_result = get_canvas_size(game_dir);
    if (canvas_size_result) {
      size_t canvas_width = std::get<0>(*canvas_size_result);
      size_t canvas_height = std::get<1>(*canvas_size_result);
      pipeline["hmstitcher"]["output-width"] = std::to_string(canvas_width);
      pipeline["hmstitcher"]["output-height"] = std::to_string(canvas_height);
      constexpr double ar = 16.0 / 9.0;
      auto wh_tuple = maybe_scale_down(static_cast<long>(ar * canvas_height), canvas_height);
      pipeline["hmplaycropper"]["output-width"] = std::to_string(std::get<0>(wh_tuple));
      pipeline["hmplaycropper"]["output-height"] = std::to_string(std::get<1>(wh_tuple));
    } else {
      // We don't know the canvas size since the mapping hasn't been generatewd yet, so we'll have to
      bool configure_only = get_node_value(pipeline, "hmstitcher.configure-only", false);
      if (!configure_only) {
        return absl::FailedPreconditionError(
            "Unable to determine the canvas size and stitcher is not set to configure-only");
      }
      std::cout << "The stitched canvas size is not yet known, will determine in the ensuing pipeline run" << std::endl;
    }
  } else {
    auto wh_tuple = maybe_scale_down(ww, hh);
    pipeline["hmplaycropper"]["output-width"] = std::to_string(std::get<0>(wh_tuple));
    pipeline["hmplaycropper"]["output-height"] = std::to_string(std::get<1>(wh_tuple));
  }

  if (area) {
    // Set streammux size
    pipeline["streammux"]["width"] = std::to_string(ww);
    pipeline["streammux"]["height"] = std::to_string(hh);
  }

  std::string possible_audio_uri;
  size_t audio_source_id = std::numeric_limits<size_t>::max();
  // Source 0 files
  static const std::string ff = "file://";
  if (!left_files.empty() && !right_files.empty()) {
    auto src0 = pipeline["source0"];
    auto src1 = pipeline["source1"];
    if (src0.IsDefined() && as_int(src0["enable"]) && as_int(src0["type"]) == NV_DS_SOURCE_URI_MULTIPLE &&
        src1.IsDefined() && as_int(src1["enable"]) && as_int(src1["type"]) == NV_DS_SOURCE_URI_MULTIPLE) {
      // Two uri sources, so set them to the stitching files
      // TODO: how to set all of the files and roll them?
      src0["uri"] = ff + file_maybe_in_game_dir(left_files[0]);
      src1["uri"] = ff + file_maybe_in_game_dir(right_files[0]);
      if (offsets["left"].as<double>() == 0) {
        possible_audio_uri = src0["uri"].as<std::string>();
        audio_source_id = src0["source-id"].as<int>();
      } else {
        assert(offsets["right"].as<double>() == 0);
        possible_audio_uri = src1["uri"].as<std::string>();
        audio_source_id = src1["source-id"].as<int>();
      }
      num_video_sources += 2;
    }
  } else {
    auto src0 = pipeline["source0"];
    if (src0.IsDefined() && as_int(src0["enable"])) {
      if (!src0["uri"].IsDefined() || src0["uri"].IsNull() || src0["uri"].as<std::string>().empty()) {
        std::string stiched_output = file_maybe_in_game_dir("stitched_output-with-audio.mp4");
        if (std::filesystem::exists(stiched_output)) {
          src0["uri"] = ff + stiched_output;
        }
      }
      if (src0["uri"].IsDefined() && !src0["uri"].IsNull()) {
        possible_audio_uri = src0["uri"].as<std::string>();
      }
      ++num_video_sources;
    }
  }
  if (num_video_sources < 2) {
    pipeline["hmstitcher"]["enable"] = "0";
  }
  // std::cout << pipeline["hmaudio"] << std::endl;
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
      if (!pipeline[hmaudio_name].IsDefined()) {
        break;
      }
      audio_uri_opt = get_node_if_enabled(pipeline, hmaudio_name);
      if (!audio_uri_opt) {
        continue;
      }
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

  //
  // If RTSP server is active, we may need to downscale
  //
  if (true) {
    std::set<std::string> all_enabled;
    for (const auto& item : pipeline) {
      std::string key = item.first.as<std::string>();
      if (key == "hmstitcher") {
        usleep(0);
      }
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
  // We need to do this get state for some reason
  GstState state, pending;
  gst_element_get_state(pipeline.pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
  if (state == GST_STATE_READY) {
    GstStateChangeReturn ret = gst_element_set_state(pipeline.pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      return absl::InternalError("Failed to get pipeline state to PAUSED");
    }
  } else if (state != GST_STATE_PAUSED) {
    return absl::InternalError(TO_STRING("Pipeline in unexpected state: " << gstStateToString(state)));
  }

  save_dot_file(pipeline.pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline");

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
        merge_nodes(subsection_config, YAML::Clone(parent_node[section_name]), /*warn_if_key_not_in_dest=*/false);
        // Remove the config file entry
        subsection_config.remove("config-file");
        parent_node[new_subsection_name] = subsection_config;
        disable_nodes.emplace(section_name);
      }
      // Now disable the original section since it has been consumed/expanded
      parent_node[section_name]["enable"] = 0;
    }
  }
  // Disable the node that had the config filew since we exploded them already
  for (const std::string& s : disable_nodes) {
    parent_node[s]["enable"] = "0";
  }
  return absl::OkStatus();
}

} // namespace hm
