#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {
// Seconds from the beginning of a camera's complete chapter playlist, keyed
// by its first selected chapter. An absent entry means no source trim.
using CameraSourceStarts = std::map<std::string, double>;
absl::StatusOr<CameraSourceStarts> ReadCameraSourceStarts(const YAML::Node& node);
YAML::Node WriteCameraSourceStarts(const CameraSourceStarts& starts);
absl::Status ValidateCameraSourceStarts(
    const CameraSourceStarts& starts,
    const std::filesystem::path& game,
    const std::vector<std::string>& first_chapters);
double CameraSourceStart(
    const CameraSourceStarts& starts,
    const std::filesystem::path& game,
    const std::string& first_chapter);
} // namespace hm::stitching
