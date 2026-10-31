#include "hstream/src/libs/stitching/AutoInputLens.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/stitching/Orientation.h"

namespace hm::stitching {
namespace {
YAML::Node child(const YAML::Node& node, const char* key) {
  if (!node || !node.IsMap())
    return YAML::Node();
  const auto value = node[key];
  return value ? value : YAML::Node();
}
std::vector<std::filesystem::path> paths(const YAML::Node& node, const std::filesystem::path& game) {
  std::vector<std::filesystem::path> result;
  if (!node || node.IsNull())
    return result;
  if (!node.IsSequence())
    throw std::runtime_error("Source playlist must be a sequence");
  for (const auto& item : node) {
    if (!item.IsScalar())
      throw std::runtime_error("Source playlist must contain paths");
    std::filesystem::path path(item.as<std::string>());
    result.push_back(path.is_absolute() ? path : game / path);
  }
  return result;
}
} // namespace
bool HasManualInputLensOverrides(const YAML::Node& config) {
  const auto stitching = child(config, "stitching");
  const auto fov = child(stitching, "camera_fov");
  const auto lens = child(stitching, "input_lens");
  for (const char* key : {"horizontal_fov", "vertical_fov"}) {
    const auto value = child(fov, key);
    if (value && !value.IsNull())
      return true;
  }
  for (const char* key : {"projection", "optimization", "distortion", "center_offset"}) {
    const auto value = child(lens, key);
    if (value && !value.IsNull())
      return true;
  }
  return false;
}

void PreserveLegacyInputLensSelection(YAML::Node& effective, const YAML::Node& layer) {
  const auto stitching = child(layer, "stitching");
  const auto selected = child(stitching, "camera_config");
  const auto automation = child(stitching, "camera_automation");
  if (selected && selected.IsScalar()) {
    if (!child(stitching, "input_cameras").IsMap())
      effective["stitching"]["input_cameras"] = YAML::Node(YAML::NodeType::Map);
    // A higher-layer selector replaces inherited policies, just as it does in
    // the CLI. Explicit independent choices in this layer still take priority.
    for (const char* key : {"fov", "lens"})
      if (!child(automation, key).IsScalar())
        effective["stitching"]["camera_automation"][key] = YAML::Node(YAML::NodeType::Null);
    return;
  }
  if (!HasManualInputLensOverrides(layer))
    return;
  const auto camera = read_stitch_camera_selection(effective);
  if (!camera.ok())
    return;
  auto manual = *camera;
  const bool independent = camera->auto_fov.has_value() || camera->auto_lens.has_value();
  if (!independent && camera->configuration == "auto") {
    manual.configuration = "manual"; // Existing configs retain their historical meaning.
  } else {
    const auto fov = child(stitching, "camera_fov"), lens = child(stitching, "input_lens");
    if (!child(automation, "fov").IsScalar() &&
        (child(fov, "horizontal_fov").IsScalar() || child(fov, "vertical_fov").IsScalar()))
      manual.auto_fov = false;
    if (!child(automation, "lens").IsScalar())
      for (const char* key : {"projection", "optimization", "distortion", "center_offset"}) {
        const auto value = child(lens, key);
        if (value && !value.IsNull())
          manual.auto_lens = false;
      }
  }
  if (!child(stitching, "input_cameras").IsMap()) {
    const auto fov = child(stitching, "camera_fov"), lens = child(stitching, "input_lens");
    for (auto& [source, value] : manual.input_cameras) {
      if (child(fov, "horizontal_fov").IsScalar())
        value.horizontal_fov = manual.horizontal_fov;
      if (child(fov, "vertical_fov").IsScalar())
        value.vertical_fov = manual.vertical_fov;
      const auto updated = ReadStitchInputLens(lens, value.input_lens);
      if (updated.ok())
        value.input_lens = *updated;
    }
  }
  write_stitch_camera_selection(effective, manual);
}

static AutoInputLensResult read_auto_input_lens_for_game(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery,
    bool live_sources) {
  AutoInputLensResult result{selection, ""};
  result.fov_message = "Manual / selected profile (MP4 import off)";
  result.lens_message = "Manual / selected profile (MP4 import off)";
  result.recording_message = "Not checked: automatic camera settings are off.";
  if (!selection.automatic_fov() && !selection.automatic_lens())
    return result;
  try {
    const auto stitching = child(config, "stitching");
    const auto frozen = child(stitching, "auto_input_lens_frozen");
    if (frozen && frozen.IsScalar() && frozen.as<bool>()) {
      result.message = "Auto: using the saved experiment optics snapshot.";
      result.fov_message = "Saved experiment snapshot";
      result.lens_message = "Saved experiment snapshot";
      result.recording_message = "Not checked: using saved experiment values, without rereading the MP4 files.";
      return result;
    }
    auto fallback = selection;
    std::string profile_name = selection.configuration;
    const auto profiles = read_stitch_camera_configurations(config);
    if (!profiles.ok())
      throw std::runtime_error(profiles.status().ToString());
    for (const auto& profile : *profiles)
      if (profile.id == selection.configuration) {
        profile_name = profile.display_name;
        if (selection.automatic_fov()) {
          fallback.horizontal_fov = profile.horizontal_fov;
          fallback.vertical_fov = profile.vertical_fov;
        }
        if (selection.automatic_lens())
          fallback.input_lens = profile.input_lens;
      }
    result.camera = fallback;
    const std::string baseline_source = "Fallback defaults: " + profile_name + " (not from MP4)";
    if (selection.automatic_fov())
      result.fov_message = baseline_source;
    if (selection.automatic_lens())
      result.lens_message = baseline_source;
    if (live_sources) {
      result.message =
          "Auto unavailable: live cameras have no original-video lens metadata. Using baseline defaults for automatic components.";
      result.recording_message = "Unavailable: live cameras have no original MP4 recording metadata.";
      if (selection.automatic_fov())
        result.fov_message = result.message;
      if (selection.automatic_lens())
        result.lens_message = result.message;
      return result;
    }
    const auto roles = child(child(config, "hstream_ui"), "video_roles");
    auto left = paths(child(roles, "left"), game), right = paths(child(roles, "right"), game);
    if (!left.empty() && !right.empty()) {
      return ResolveAutoInputLensForCameras(left, right, fallback, *profiles, true, game);
    }
    auto discovered = get_available_videos(game.string());
    if (!discovered.ok())
      throw std::runtime_error(discovered.status().ToString());
    const bool explicit_roles = !left.empty() || !right.empty();
    const bool cam_dirs = std::any_of(discovered->begin(), discovered->end(), [](const auto& v) {
      std::string key = v.first;
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
      return key.rfind("cam", 0) == 0;
    });
    if (!explicit_roles && !cam_dirs && !force_video_discovery) {
      const auto videos = child(child(config, "game"), "videos");
      left = paths(child(videos, "left"), game);
      right = paths(child(videos, "right"), game);
    }
    auto append = [&](const VideoChapter& chapters, std::vector<std::filesystem::path>& target) {
      for (const auto& [_, path] : chapters)
        target.emplace_back(path);
    };
    if (!left.empty() || !right.empty() || discovered->count("left") || discovered->count("right")) {
      if (left.empty() && discovered->count("left"))
        append(discovered->at("left"), left);
      if (right.empty() && discovered->count("right"))
        append(discovered->at("right"), right);
      if (left.empty() || right.empty())
        throw std::runtime_error("Select both Left and Right camera sources");
    } else {
      size_t cameras = 0;
      for (const auto& [key, chapters] : *discovered) {
        if (key == "stitched" || chapters.empty())
          continue;
        append(chapters, cameras == 0 ? left : right);
        ++cameras;
      }
      if (cameras != 2)
        throw std::runtime_error("Auto lens import requires two selected source cameras");
      // Discovery groups chapters before orientation is known. Preserve those
      // groups and identify choices by filename instead of guessing Left/Right.
      return ResolveAutoInputLensForCameras(left, right, fallback, *profiles, false, game);
    }
    return ResolveAutoInputLensForCameras(left, right, fallback, *profiles, true, game);
  } catch (const std::exception& error) {
    result.message = "Auto unavailable: " + std::string(error.what()) + ". Using baseline defaults.";
    result.recording_message = "Not read: " + std::string(error.what());
    if (selection.automatic_fov())
      result.fov_message = result.message;
    if (selection.automatic_lens())
      result.lens_message = result.message;
    return result;
  }
}
AutoInputLensResult ReadAutoInputLensForGame(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery,
    bool live_sources) {
  auto result = read_auto_input_lens_for_game(game, config, selection, force_video_discovery, live_sources);
  if (!result.camera.input_cameras.empty()) {
    std::ostringstream description;
    description << "Each source camera uses its own optics.\n" << std::setprecision(10);
    size_t index = 0;
    for (const auto& [source, values] : result.camera.input_cameras) {
      description << "Camera " << ++index << " (" << source << "): horizontal FOV " << values.horizontal_fov
                  << "°, vertical FOV " << values.vertical_fov << "°.\n"
                  << values.input_lens.projection << "; " << values.input_lens.optimization << " correction. "
                  << "Distortion a/b/c: " << values.input_lens.distortion[0] << ", " << values.input_lens.distortion[1]
                  << ", " << values.input_lens.distortion[2]
                  << ". Center X/Y: " << values.input_lens.center_offset[0] * 100 << "%, "
                  << values.input_lens.center_offset[1] * 100 << "%.\n";
    }
    description << "Use one of the shared choices to apply the same numbers to both cameras.";
    result.shared_choices_message = description.str();
    if (result.shared_choices.empty()) {
      index = 0;
      for (const auto& [source, values] : result.camera.input_cameras) {
        StitchCameraSelection shared{"manual", values.horizontal_fov, values.vertical_fov, values.input_lens};
        shared.auto_fov = shared.auto_lens = false;
        result.shared_choices["camera" + std::to_string(++index)] = std::move(shared);
      }
    }
    if (!result.shared_choices.count("average"))
      if (const auto average = AverageStitchInputCameras(result.camera.input_cameras))
        result.shared_choices["average"] = *average;
    result.mismatch = true;
  }
  return result;
}
} // namespace hm::stitching
