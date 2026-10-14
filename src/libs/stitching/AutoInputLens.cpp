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
AutoInputLensResult ReadAutoInputLensForGame(
    const std::filesystem::path& game,
    const YAML::Node& config,
    const StitchCameraSelection& selection,
    bool force_video_discovery) {
  AutoInputLensResult result{selection, ""};
  if (selection.configuration != "auto")
    return result;
  try {
    const auto stitching = child(config, "stitching");
    const auto frozen = child(stitching, "auto_input_lens_frozen");
    if (frozen && frozen.IsScalar() && frozen.as<bool>()) {
      result.message = "Auto: using the saved experiment lens snapshot.";
      return result;
    }
    auto fallback = selection;
    const auto profiles = read_stitch_camera_configurations(config);
    if (!profiles.ok())
      throw std::runtime_error(profiles.status().ToString());
    for (const auto& profile : *profiles)
      if (profile.id == "auto")
        fallback = {profile.id, profile.horizontal_fov, profile.vertical_fov, profile.input_lens};
    result.camera = fallback;
    const auto roles = child(child(config, "hstream_ui"), "video_roles");
    auto left = paths(child(roles, "left"), game), right = paths(child(roles, "right"), game);
    if (!left.empty() && !right.empty()) {
      left.insert(left.end(), right.begin(), right.end());
      return ResolveAutoInputLens(left, fallback);
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
    return ResolveAutoInputLens(left, fallback);
  } catch (const std::exception& error) {
    result.message = "Auto unavailable: " + std::string(error.what()) + ". Using automatic lens fitting.";
    return result;
  }
}
} // namespace hm::stitching
