#include "hstream/src/libs/stitching/CameraSourceStart.h"

#include <cmath>
#include <set>

namespace hm::stitching {
absl::StatusOr<CameraSourceStarts> ReadCameraSourceStarts(const YAML::Node& node) {
  CameraSourceStarts result;
  if (!node || node.IsNull())
    return result;
  if (!node.IsMap() || node.size() > 2)
    return absl::InvalidArgumentError("Camera start times must be a map of at most two source recordings");
  try {
    for (const auto& entry : node) {
      const auto source = entry.first.as<std::string>();
      const auto seconds = entry.second.as<double>();
      if (source.empty() || source.size() > 4096 ||
          source.find_first_of(std::string("\0\r\n", 3)) != std::string::npos || !std::isfinite(seconds) ||
          seconds < 0 || seconds >= 24 * 3600)
        return absl::InvalidArgumentError("Camera start must be finite seconds within one day, bound to a source");
      if (!result.emplace(source, seconds).second)
        return absl::InvalidArgumentError("Duplicate camera start source: " + source);
    }
    // Explicit zero and an omitted trim are the same input timeline.
    for (auto it = result.begin(); it != result.end();)
      it = it->second == 0 ? result.erase(it) : std::next(it);
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid camera start time: " + std::string(error.what()));
  }
}

YAML::Node WriteCameraSourceStarts(const CameraSourceStarts& starts) {
  YAML::Node result(YAML::NodeType::Map);
  for (const auto& [source, seconds] : starts)
    if (seconds != 0)
      result[source] = seconds;
  return result;
}

double CameraSourceStart(
    const CameraSourceStarts& starts,
    const std::filesystem::path& game,
    const std::string& first_chapter) {
  const auto identity = (game / first_chapter).lexically_normal();
  for (const auto& [source, seconds] : starts)
    if ((game / source).lexically_normal() == identity)
      return seconds;
  return 0;
}

absl::Status ValidateCameraSourceStarts(
    const CameraSourceStarts& starts,
    const std::filesystem::path& game,
    const std::vector<std::string>& first_chapters) {
  std::set<std::filesystem::path> selected, used;
  for (const auto& chapter : first_chapters)
    selected.insert((game / chapter).lexically_normal());
  for (const auto& [source, seconds] : starts) {
    const auto identity = (game / source).lexically_normal();
    if (!selected.count(identity) || !used.insert(identity).second)
      return absl::InvalidArgumentError("Camera start must identify one selected camera's first chapter: " + source);
  }
  return absl::OkStatus();
}
} // namespace hm::stitching
