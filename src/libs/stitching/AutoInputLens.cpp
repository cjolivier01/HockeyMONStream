#include "hstream/src/libs/stitching/AutoInputLens.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "hstream/src/libs/stitching/Orientation.h"

namespace hm::stitching {
namespace {
YAML::Node child(const YAML::Node& node, const char* key) {
  return node && node.IsMap() ? node[key] : YAML::Node();
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
  write_stitch_camera_selection(effective, manual);
}

AutoInputLensResult ReadAutoInputLensForGame(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery,
    bool live_sources) {
  AutoInputLensResult result{selection, ""};
  result.fov_message = "Manual / selected profile";
  result.lens_message = "Manual / selected profile";
  if (!selection.automatic_fov() && !selection.automatic_lens())
    return result;
  try {
    const auto stitching = child(config, "stitching");
    const auto frozen = child(stitching, "auto_input_lens_frozen");
    if (frozen && frozen.IsScalar() && frozen.as<bool>()) {
      result.message = "Auto: using the saved experiment optics snapshot.";
      result.fov_message = "Saved experiment snapshot";
      result.lens_message = "Saved experiment snapshot";
      return result;
    }
    auto fallback = selection;
    const auto profiles = read_stitch_camera_configurations(config);
    if (!profiles.ok())
      throw std::runtime_error(profiles.status().ToString());
    for (const auto& profile : *profiles)
      if (profile.id == selection.configuration) {
        if (selection.automatic_fov()) {
          fallback.horizontal_fov = profile.horizontal_fov;
          fallback.vertical_fov = profile.vertical_fov;
        }
        if (selection.automatic_lens())
          fallback.input_lens = profile.input_lens;
      }
    result.camera = fallback;
    result.fov_message = selection.automatic_fov() ? "Baseline default" : "Manual / selected profile";
    result.lens_message =
        selection.automatic_lens() ? "Baseline default / automatic fitting" : "Manual / selected profile";
    if (live_sources) {
      result.message =
          "Auto unavailable: live cameras have no original-video lens metadata. Using baseline defaults for automatic components.";
      if (selection.automatic_fov())
        result.fov_message = result.message;
      if (selection.automatic_lens())
        result.lens_message = result.message;
      return result;
    }
    const auto roles = child(child(config, "hstream_ui"), "video_roles");
    auto left = paths(child(roles, "left"), game), right = paths(child(roles, "right"), game);
    if (!left.empty() && !right.empty()) {
      left.insert(left.end(), right.begin(), right.end());
      return ResolveAutoInputLens(left, fallback, *profiles);
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
        append(chapters, left);
        ++cameras;
      }
      if (cameras != 2)
        throw std::runtime_error("Auto lens import requires two selected source cameras");
    }
    left.insert(left.end(), right.begin(), right.end());
    return ResolveAutoInputLens(left, fallback, *profiles);
  } catch (const std::exception& error) {
    result.message = "Auto unavailable: " + std::string(error.what()) + ". Using baseline defaults.";
    if (selection.automatic_fov())
      result.fov_message = result.message;
    if (selection.automatic_lens())
      result.lens_message = result.message;
    return result;
  }
}
} // namespace hm::stitching
