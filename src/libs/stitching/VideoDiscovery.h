#pragma once

#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {

/// Mapping from chapter number to filename.
using VideoChapter = std::map<int, std::string>;
/// Videos dictionary mapping a video key (as string) to its chapters.
using VideosDict = std::map<std::string, VideoChapter>;

// Unnumbered recordings can each form a camera playlist without guessing chapter order.
// Exported to the browser source contract; keep filename admission identical there.
inline constexpr const char* kStandaloneCameraFilePattern = R"(^.*[^0-9]\.(mp4|mkv|m4v|mov|avi)$)";
bool IsStandaloneCameraFile(const std::string& filename);

// Diagnose missing chapters before the end of each ordered recording for cameras using the
// same recognized naming scheme. Extra trailing chapters are allowed. Different schemes do
// not imply matching file boundaries. Camera labels are included in diagnostics.
// Advisory only: the UI offers an override; unattended runners log and continue.
absl::Status ValidateCameraChapters(const std::map<std::string, std::vector<std::string>>& cameras);
absl::Status ValidateDiscoveredCameraChapters(const VideosDict& cameras);

// Scoreboard-only recordings are excluded from automatic game-camera discovery.
// Supply the effective configuration for unsaved/CLI overrides. Otherwise the
// persisted baseline, user snapshot and game configuration own the roles.
absl::StatusOr<VideosDict> get_available_videos(
    const std::string& dir_name, bool prune = false, const YAML::Node* effective_config = nullptr);

} // namespace hm::stitching
