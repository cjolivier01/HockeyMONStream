#include "hstream/src/libs/program/ProgramOutputConfig.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

namespace hm::program {
namespace {
using Configuration = config::ProgramConfiguration;
int Rank(const Configuration& config, const std::string& path) {
  const auto found = config.ranks.find(path);
  return found == config.ranks.end() ? 0 : found->second;
}
YAML::Node Required(const Configuration& config, const std::string& path) {
  const auto node = get_node(config.values, path);
  if (!node || !node->IsDefined())
    throw std::invalid_argument("Missing output setting: " + path);
  return *node;
}
struct Choice {
  YAML::Node value;
  std::string path;
  int rank, priority;
  void Consider(const Configuration& config, const std::string& candidate, int order) {
    const auto node = get_node(config.values, candidate);
    if (!node || node->IsNull())
      return;
    const int layer = Rank(config, candidate);
    if (layer > rank || (layer == rank && order > priority)) {
      value.reset(YAML::Clone(*node));
      path = candidate;
      rank = layer;
      priority = order;
    }
  }
};
bool Boolean(const YAML::Node& node) {
  if (node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1"))
    return node.Scalar() == "1";
  return node.as<bool>();
}
float Number(const YAML::Node& node, const std::string& path) {
  const float value = node.as<float>();
  if (!std::isfinite(value))
    throw std::invalid_argument("Non-finite output setting: " + path);
  return value;
}
unsigned Positive(const YAML::Node& node, const std::string& path) {
  const auto value = node.as<int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max())
    throw std::invalid_argument("Output setting must fit a positive integer: " + path);
  return static_cast<unsigned>(value);
}
bool Automatic(const YAML::Node& node) {
  return node.IsNull() || (node.IsScalar() && node.Scalar() == "auto");
}
int Dimension(int64_t value) {
  if (value <= 0 || value > std::numeric_limits<int>::max())
    throw std::invalid_argument("Derived output dimension exceeds a positive integer");
  return static_cast<int>(value);
}
Choice CropChoice(const Configuration& config, const std::string& canonical, const std::string& native) {
  Choice result{Required(config, canonical), canonical, Rank(config, canonical), 0};
  result.Consider(config, "pipeline.hmplaycropper." + native, 1);
  result.Consider(config, "pipeline.hmplaycropper.properties." + native, 2);
  result.Consider(config, "pipeline.hmplaycropper.private-properties." + native, 3);
  return result;
}
} // namespace
absl::StatusOr<ProgramOutputConfig> ResolveProgramOutputConfig(const Configuration& config) {
  try {
    ProgramOutputConfig result;
    const std::string root = "runtime.amd.output.";
    const auto base_width = Positive(Required(config, root + "width"), root + "width");
    const auto base_height = Positive(Required(config, root + "height"), root + "height");
    result.width = base_width;
    result.height = base_height;
    const auto width = Required(config, "video_out.output_width"), height = Required(config, "video_out.output_height");
    const bool choose_width =
        !Automatic(width) && Rank(config, "video_out.output_width") > Rank(config, root + "width");
    const bool choose_height =
        !Automatic(height) && Rank(config, "video_out.output_height") > Rank(config, root + "height");
    if (choose_width)
      result.width = Positive(width, "video_out.output_width");
    if (choose_height)
      result.height = Positive(height, "video_out.output_height");
    // A single canonical dimension preserves the configured output aspect.
    if (choose_width && !choose_height && Automatic(height) && Rank(config, root + "height") == 0)
      result.height = Dimension(int64_t(result.width) * base_height / base_width / 2 * 2);
    if (choose_height && !choose_width && Automatic(width) && Rank(config, root + "width") == 0)
      result.width = Dimension(int64_t(result.height) * base_width / base_height / 2 * 2);
    if (result.width <= 0 || result.height <= 0 || ((result.width | result.height) & 1))
      return absl::InvalidArgumentError("AMD Program output dimensions must be positive even integers");
    result.bitrate_kbps = Positive(Required(config, root + "bitrate_kbps"), root + "bitrate_kbps");
    const auto bitrate = Required(config, "video_out.bit_rate");
    if (!bitrate.IsNull() && Rank(config, "video_out.bit_rate") > Rank(config, root + "bitrate_kbps")) {
      const auto bits = Positive(bitrate, "video_out.bit_rate");
      result.bitrate_kbps = (uint64_t(bits) + 999) / 1000;
    }
    result.keyframe_interval = Positive(Required(config, root + "keyframe_interval"), root + "keyframe_interval");
    result.realtime = Boolean(Required(config, root + "realtime"));
    const double start = Required(config, "runtime.amd.playback_start_seconds").as<double>();
    if (!std::isfinite(start) || start < 0 || static_cast<long double>(start) * 1000000000 > INT64_MAX)
      return absl::InvalidArgumentError("AMD playback start must fit nonnegative nanoseconds");
    result.start_ns = static_cast<uint64_t>(static_cast<long double>(start) * 1000000000);
    const auto crop = CropChoice(config, "apply_camera.crop_output_image", "no-crop");
    result.render.no_crop = crop.path == "apply_camera.crop_output_image" ? !Boolean(crop.value) : Boolean(crop.value);
    const std::string rotation = "rink.camera.fixed_edge_rotation_angle";
    const auto canonical_angle = Required(config, rotation);
    if (canonical_angle.IsSequence() && canonical_angle.size() != 2)
      return absl::InvalidArgumentError("Program edge rotation requires a scalar or [left, right]");
    for (unsigned side = 0; side < 2; ++side) {
      const auto value = canonical_angle.IsSequence() ? canonical_angle[side] : canonical_angle;
      Choice choice{value.IsNull() ? YAML::Node(0) : YAML::Clone(value), rotation, Rank(config, rotation), 0};
      int priority = 1;
      for (const auto* bag : {"", "properties.", "private-properties."}) {
        const std::string prefix = std::string("pipeline.hmplaycropper.") + bag;
        choice.Consider(config, prefix + "fixed-edge-rotation-angle", priority++);
        choice.Consider(
            config, prefix + (side ? "fixed-edge-rotation-angle-right" : "fixed-edge-rotation-angle-left"), priority++);
      }
      (side ? result.render.fixed_edge_rotation_angle_right : result.render.fixed_edge_rotation_angle_left) =
          Number(choice.value, choice.path);
    }
    const auto backend =
        stitching::ParseMappingBackend(Required(config, "stitching.mapping_backend").as<std::string>());
    if (!backend.ok())
      return backend.status();
    if (*backend == stitching::MappingBackend::kNona) {
      const auto framing = stitching::read_stitch_projection_framing(config.values);
      if (!framing.ok())
        return framing.status();
      if (framing->has_leveling_rotation()) {
        // The saved NONA maps already level the rays. Match the NVIDIA
        // configurator even when an explicit native property requests a tilt.
        result.render.fixed_edge_rotation_angle_left = 0;
        result.render.fixed_edge_rotation_angle_right = 0;
      }
    }
    const auto tone = [&](const char* ui, const char* native, const char* fallback) {
      const std::string path = std::string("hstream_ui.camera_controls.") + ui;
      Choice choice{Required(config, root + fallback), root + fallback, Rank(config, root + fallback), 0};
      choice.Consider(config, path, 3);
      choice.Consider(config, std::string("pipeline.hmplaycropper.properties.") + native, 1);
      choice.Consider(config, std::string("pipeline.hmstitcher.properties.") + native, 2);
      return choice;
    };
    const auto shadow = tone("Bring_Up_Shadows", "shadow-lift", "shadow_lift_percent");
    result.render.shadow_lift_percent = Number(shadow.value, shadow.path);
    const auto black = tone("Lift_Shadow_Black_Point", "shadow-lift-black-point", "lift_shadow_black_point");
    result.render.lift_shadow_black_point = Boolean(black.value);
    const auto exposure = tone("Exposure_x100", "exposure", "exposure");
    result.render.exposure = Number(exposure.value, exposure.path);
    if (exposure.path == "hstream_ui.camera_controls.Exposure_x100")
      result.render.exposure /= 100;
    if (result.render.shadow_lift_percent < 0 || result.render.shadow_lift_percent > 100 ||
        result.render.exposure < 0 || result.render.exposure > 1.3f)
      return absl::InvalidArgumentError("Program tone controls exceed their supported range");
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid Program output configuration: ") + error.what());
  }
}
} // namespace hm::program
