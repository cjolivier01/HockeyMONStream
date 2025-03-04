#include "configurator.h"
#include "cupano/pano/controlMasks.h"
#include "deepstream_app.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include <gstreamer-1.0/gst/gstelement.h>
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <unistd.h>

#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/libs/stitching/ConfigureStitching.h"

namespace hm {

namespace {

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
    // if (n.IsDefined() && n.IsMap()) {
    //   YAML::Node enabled = n["enable"];
    //   if (enabled.IsDefined()) {
    //     if (enabled.as<std::string>() == "true") {
    //       return n;
    //     }
    //     if (enabled.as<int>()) {
    //       return n;
    //     }
    //   }
    // }
  }
  return std::nullopt;
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

absl::Status Configurator::complete_configuration() {
  YAML::Node pipeline = config_["pipeline"];
  assert(pipeline.IsDefined());

  absl::Status status;

  if (game_id_.empty()) {
    return absl::InvalidArgumentError("No game id specified");
  }

  // Stitching config mask config dir
  auto game_dir = get_game_dir(game_id_);

  pipeline["hmstitcher"]["config-file"] = std::string(game_dir);

  if (!stitching::is_stitching_configured(game_dir).value_or(false) && stitching::can_configure_stitching(config_)) {
    // HM_RETURN_IF_ERROR(stitching::configure_stitching(
    //     const std::string& game_id, surface::Surface left_surface, surface::Surface right_surface)
  }

  pipeline["ds-fieldmask"]["detection-mask"] = std::string(game_dir / "rink_mask_0.png");
  // Stitching LFO, RFO
  std::vector<std::string> left_files;
  std::vector<std::string> right_files;

  if (has_node(config_, "game.videos.left", /*non_null=*/true)) {
    std::cout << config_["game"]["videos"]["left"] << std::endl;
    left_files = config_["game"]["videos"]["left"].as<std::vector<std::string>>();
  }
  if (has_node(config_, "game.videos.right", /*non_null=*/true)) {
    right_files = config_["game"]["videos"]["right"].as<std::vector<std::string>>();
  }

  long area = 0, ww = 0, hh = 0;

  size_t num_video_sources = 0;

  auto offsets = config_["game"]["stitching"]["frame_offsets"];
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
    if (right_info.width * right_info.height > area) {
      ww = right_info.width;
      hh = right_info.height;
    }
  }

  const bool is_udb_output = has_enabled_rtsp_sink(pipeline);

  auto maybe_scale_down = [is_udb_output](long width, long height) -> std::tuple<int, int> {
    if (!is_udb_output) {
      return std::make_tuple(width, height);
    }
    // 4k @ 16:9
#if 1
    constexpr long kMaxUdpStreamingWidth = 3840;
    constexpr long kMaxUdpStreamingHeight = 2160;
    if (width > kMaxUdpStreamingWidth) {
      double ar = double(width) / height;
      width = kMaxUdpStreamingWidth;
      height = (long)(width / ar);
      assert(height <= kMaxUdpStreamingHeight);
    }
#endif
    return std::make_tuple(width, height);
  };

  if (!left_files.empty() && !right_files.empty()) {
    auto canvas_size_result = get_canvas_size(game_dir);
    if (canvas_size_result) {
      size_t canvas_width = std::get<0>(*canvas_size_result);
      size_t canvas_height = std::get<1>(*canvas_size_result);
      pipeline["hmstitcher"]["output-width"] = std::to_string(canvas_width);
      pipeline["hmstitcher"]["output-height"] = std::to_string(canvas_height);
      constexpr double ar = 16.0 / 9.0;
      auto wh_tuple = maybe_scale_down(static_cast<long>(ar * canvas_height), canvas_height);
      pipeline["hmvideoprep"]["output-width"] = std::to_string(std::get<0>(wh_tuple));
      pipeline["hmvideoprep"]["output-height"] = std::to_string(std::get<1>(wh_tuple));
    }
  } else {
    auto wh_tuple = maybe_scale_down(ww, hh);
    pipeline["hmvideoprep"]["output-width"] = std::to_string(std::get<0>(wh_tuple));
    pipeline["hmvideoprep"]["output-height"] = std::to_string(std::get<1>(wh_tuple));
  }

  if (area) {
    // Set streammux size
    pipeline["streammux"]["width"] = std::to_string(ww);
    pipeline["streammux"]["height"] = std::to_string(hh);
  }

  std::string possible_audio_uri;
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
      } else {
        assert(offsets["right"].as<double>() == 0);
        possible_audio_uri = src1["uri"].as<std::string>();
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
    // YAML::Node n = pipeline["hmstitcher"];
    // std::cout << n << std::endl;
    // n["enable"] = "0";
    // std::cout << pipeline["hmstitcher"] << std::endl;
  }
  if (!possible_audio_uri.empty()) {
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
      if (audio_uri_opt.has_value()) {
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

bool Configurator::post_config_pipeline(NvDsPipeline& pipeline, const NvDsConfig& config) {
  // We need to do this get state for some reason
  GstState state, pending;
  GstStateChangeReturn ret = gst_element_get_state(pipeline.pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
  assert(ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL);
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
    src_bins.emplace_back(pipeline.multi_src_bin.sub_bins[i].bin);
  }
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
  return true;
}

} // namespace hm
