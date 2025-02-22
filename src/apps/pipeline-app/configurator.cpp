#include "configurator.h"
#include "deepstream_app.h"
// #include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/opencv.hpp>
#include <unistd.h>

namespace hm {

void save_dot_file(GstElement* pipeline, GstDebugGraphDetails details, const std::string& filename) {
  gchar* dot_data = gst_debug_bin_to_dot_data(GST_BIN(pipeline), details);
  if (dot_data) {
    // Print to stdout
    // std::cout << dot_data << std::endl;

    // Or save to a file
    std::ofstream dot_file(filename + ".dot");
    if (dot_file.is_open()) {
      dot_file << dot_data;
      dot_file.close();
      std::cout << "DOT file saved as '" << filename << ".dot'" << std::endl;
    } else {
      std::cerr << "Failed to open file for writing." << std::endl;
    }

    g_free(dot_data);
  } else {
    std::cerr << "Failed to generate DOT data" << std::endl;
  }
}

namespace {

bool seek_element(GstElement* seek_element, size_t seek_to_nanoseconds) {
  // size_t seekTarget = static_cast<size_t>(abs_seconds * 60 * GST_SECOND);
  size_t seekTarget = seek_to_nanoseconds;
  if (!gst_element_seek(
          seek_element,
          1.0, // Normal playback rate.
          GST_FORMAT_TIME,
          // (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          (GstSeekFlags)((int)GST_SEEK_FLAG_FLUSH | (int)GST_SEEK_FLAG_ACCURATE),
          GST_SEEK_TYPE_SET, // Start from the target position.
          seekTarget,
          GST_SEEK_TYPE_NONE, // No specific end position.
          GST_CLOCK_TIME_NONE)) {
    g_printerr("Seek failed\n");
    return false;
  } else {
    g_print("Seek successful to %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(seekTarget));
    return true;
  }
}

} // namespace

Configurator::Configurator(const std::string& game_id, const std::string& config_root_dir)
    : game_id_(game_id), config_root_dir_(config_root_dir) {
  // Constructor
}
Configurator::~Configurator() {
  // Destructor
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
  // std::cout << config << std::endl;
  return std::move(config);
}

static double getVideoFPS(const std::string& videoPath) {
  // Open the video file.
  cv::VideoCapture cap(videoPath);
  if (!cap.isOpened()) {
    std::cerr << "Error: Could not open video file: " << videoPath << std::endl;
    return -1.0;
  }

  // Retrieve the FPS property.
  double fps = cap.get(cv::CAP_PROP_FPS);
  return fps;
}

static int as_int(const YAML::Node& node) {
  // be less asserty than YAML-CPP
  // std::cout << node << std::endl;
  if (!node.IsDefined()) {
    return 0;
  }
  std::string s = node.as<std::string>();
  return std::atoi(s.c_str());
}

void Configurator::complete_configuration() {
  // std::cout << config_ << std::endl;
  YAML::Node pipeline = config_["pipeline"];
  assert(pipeline.IsDefined());
  // Stitching config mask config dir
  auto game_dir = get_game_dir(game_id_);
  pipeline["hmstitcher"]["config-file"] = std::string(game_dir);
  pipeline["ds-fieldmask"]["detection-mask"] = std::string(game_dir / "rink_mask_0.png");
  // Stitching LFO, RFO
  std::vector<std::string> left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
  std::vector<std::string> right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
  auto offsets = config_["game"]["stitching"]["frame_offsets"];
  if (!left_files.empty()) {
    double fps = getVideoFPS(file_maybe_in_game_dir(left_files[0]));
    double lfo = offsets["left"].as<double>(); // this is decimal frames
    set_stream_offsets_ |= lfo != 0.0;
    pipeline["hmstitcher"]["left-frame-offset-ns"] = std::to_string(size_t(lfo / fps * GST_SECOND));
  }
  if (!right_files.empty()) {
    double fps = getVideoFPS(file_maybe_in_game_dir(right_files[0]));
    double rfo = offsets["right"].as<double>(); // this is decimal frames
    set_stream_offsets_ |= rfo != 0.0;
    pipeline["hmstitcher"]["right-frame-offset-ns"] = std::to_string(size_t(rfo / fps * GST_SECOND));
  }
  std::string possible_audio_uri;
  // Source 0 files
  static const std::string ff = "file://";
  size_t source_index = 0;
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
      } else {
        assert(offsets["right"].as<double>() == 0);
        possible_audio_uri = src1["uri"].as<std::string>();
      }
      // std::cout << src0 << std::endl;
      source_index += 2;
    }
  } else {
    auto src0 = pipeline["source0"];
    if (src0.IsDefined() && as_int(src0["enable"])) {
      if (!src0["uri"].IsDefined() || src0["uri"].as<std::string>().empty()) {
        std::string stiched_output = file_maybe_in_game_dir("stitched_output-with-audio.mp4");
        if (std::filesystem::exists(stiched_output)) {
          src0["uri"] = ff + stiched_output;
        }
      }
      source_index += 1;
      possible_audio_uri = src0["uri"].as<std::string>();
    }
  }
  std::string audio_source_key = "source" + std::to_string(source_index);
  if (!possible_audio_uri.empty() && pipeline[audio_source_key].IsDefined()) {
    ++source_index;
    auto audio0 = pipeline[audio_source_key];
    if (audio0["enable"].IsDefined() && audio0["enable"].as<int>() &&
        audio0["type"].as<int>() == NV_DS_SOURCE_AUDIO_URI) {
      if (!audio0["uri"].IsDefined() || audio0["uri"].as<std::string>().empty()) {
        audio0["uri"] = possible_audio_uri;
      }
    }
  }
}

bool Configurator::post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config) {
  // We need to do this get state for some reason
  GstState state, pending;
  GstStateChangeReturn ret = gst_element_get_state(pipeline.pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
  assert(ret == GST_STATE_CHANGE_SUCCESS);
  assert(state == GstState::GST_STATE_PAUSED);

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
    // src_bins.emplace_back(pipeline.multi_src_bin.sub_bins[i].src_elem);
    src_bins.emplace_back(pipeline.multi_src_bin.sub_bins[i].bin);
  }
  // assert(src_bins.size() == 2);
#if 1
  if (src_bins.size() == 2) {
    if (config.hmsticher_config.left_frame_offset_ns) {
      bool result = seek_element(src_bins[0], config.hmsticher_config.left_frame_offset_ns);
      if (!result) {
        g_printerr("Failed to seek source 0\n");
      }
    }
    if (config.hmsticher_config.right_frame_offset_ns) {
      size_t seekTarget = 0.95 * GST_SECOND;
      bool result = seek_element(src_bins[1], seekTarget /*config.hmsticher_config.right_frame_offset_ns*/);
      if (!result) {
        g_printerr("Failed to seek source 1\n");
      }
    }
  }
#endif
  return true;
}

} // namespace hm
