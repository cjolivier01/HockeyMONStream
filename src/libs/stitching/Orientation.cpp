#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/stitching/Synchronization.h"

#include "hstream/src/libs/common/StatusMacros.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/OrientationInternal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/videoio.hpp>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace hm {
namespace stitching {

// For convenience.
namespace fs = std::filesystem;

absl::Status orientation_internal::save_orientation_config(
    const fs::path& game_dir,
    const VideoChapter& left,
    const VideoChapter& right,
    const std::string& expected_invalidation_id) {
  if (left.empty() || right.empty()) {
    return absl::InvalidArgumentError("Both camera orientations need at least one chapter");
  }
  std::vector<std::string> left_paths;
  std::vector<std::string> right_paths;
  auto append_relative_paths = [&](const VideoChapter& chapters, std::vector<std::string>& paths) -> absl::Status {
    paths.reserve(chapters.size());
    for (const auto& [_, chapter_path] : chapters) {
      std::error_code error;
      // Keep workspace-local media links named inside the workspace. fs::relative
      // canonicalizes symlinks, escaping to the original game and breaking the
      // frozen player-frame playlist when it is handed to another experiment.
      const fs::path absolute_chapter = fs::absolute(chapter_path, error).lexically_normal();
      if (error)
        return absl::InternalError("Failed to relativize video path: " + error.message());
      const fs::path absolute_game = fs::absolute(game_dir, error).lexically_normal();
      if (error)
        return absl::InternalError("Failed to resolve game directory: " + error.message());
      const fs::path relative = absolute_chapter.lexically_relative(absolute_game);
      if (relative.empty())
        return absl::InvalidArgumentError("Video path cannot be made relative to the game directory");
      paths.push_back(relative.string());
    }
    return absl::OkStatus();
  };
  HM_RETURN_IF_ERROR(append_relative_paths(left, left_paths));
  HM_RETURN_IF_ERROR(append_relative_paths(right, right_paths));

  auto config_lock = GameConfigTransactionLock::Acquire(game_dir);
  if (!config_lock.ok())
    return config_lock.status();
  const fs::path config_path = game_dir / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    HM_RETURN_IF_ERROR(validate_stitching_generation_owner(config, expected_invalidation_id));
    config["game"]["videos"]["left"] = left_paths;
    config["game"]["videos"]["right"] = right_paths;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Failed to update private game config: " + std::string(error.what()));
  }

  std::ostringstream serialized;
  serialized << config << '\n';
  return publish_game_config(game_dir, serialized.str());
}

absl::StatusOr<OrientationScores> rink_orientation_scores(const cv::Mat& binary_mask) {
  if (binary_mask.empty() || binary_mask.channels() != 1) {
    return absl::InvalidArgumentError("Orientation requires a non-empty single-channel rink mask");
  }
  cv::Mat mask;
  cv::compare(binary_mask, 0, mask, cv::CMP_GT);
  const int band_width = mask.cols / 8;
  const int band_height = mask.rows / 8;
  if (band_width <= 0 || band_height <= 0) {
    return absl::InvalidArgumentError("Orientation rink mask is too small");
  }
  return OrientationScores{
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, 0, band_width, mask.rows)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(mask.cols - band_width, 0, band_width, mask.rows)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, 0, mask.cols, band_height)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, mask.rows - band_height, mask.cols, band_height)))),
  };
}

absl::StatusOr<std::string> classify_rink_orientation(const cv::Mat& binary_mask) {
  auto scores = rink_orientation_scores(binary_mask);
  if (!scores.ok())
    return scores.status();
  if (scores->left > scores->right)
    return std::string("right");
  if (scores->right > scores->left)
    return std::string("left");
  return absl::FailedPreconditionError(
      "Ambiguous camera orientation: left edge sum=" + std::to_string(scores->left) +
      ", right edge sum=" + std::to_string(scores->right));
}

absl::Status configure_game_orientation(
    const std::string& game_dir_string,
    const RinkSegmentation& rink_model,
    const std::string& expected_invalidation_id,
    const std::function<bool()>& is_cancelled,
    const CameraSourceStarts& source_starts) {
  const fs::path game_dir(game_dir_string);
  auto videos = get_available_videos(game_dir.string());
  if (!videos.ok())
    return videos.status();
  if (videos->count("left") && videos->count("right")) {
    return orientation_internal::save_orientation_config(
        game_dir, videos->at("left"), videos->at("right"), expected_invalidation_id);
  }

  std::vector<std::string> first_chapters;
  for (const auto& [camera, chapters] : *videos)
    if (camera != "stitched" && !chapters.empty())
      first_chapters.push_back(chapters.begin()->second);
  const auto starts_status = ValidateCameraSourceStarts(source_starts, game_dir, first_chapters);
  if (!starts_status.ok())
    return starts_status;
  std::map<std::string, VideoChapter> oriented;
  for (const auto& [camera, chapters] : *videos) {
    if (is_cancelled && is_cancelled())
      return absl::CancelledError("Camera orientation cancelled");
    if (camera == "stitched" || chapters.empty())
      continue;
    std::vector<std::string> playlist;
    for (const auto& [number, file] : chapters)
      playlist.push_back((game_dir / file).string());
    const double start = CameraSourceStart(source_starts, game_dir, playlist.front());
    const auto window = CameraRecordingWindowAt(playlist, start, 1);
    if (!window.ok())
      return window.status();
    const auto& selected = window->chapters.front();
    cv::VideoCapture capture(selected.file);
    if (!capture.isOpened())
      return absl::NotFoundError("Failed to open orientation video: " + selected.file);
    if (selected.start_seconds > 0 && !capture.set(cv::CAP_PROP_POS_MSEC, selected.start_seconds * 1000))
      return absl::FailedPreconditionError("Failed to seek orientation video to the camera start: " + selected.file);
    cv::Mat first_frame;
    if (!capture.read(first_frame) || first_frame.empty()) {
      return absl::InternalError("Failed to decode the orientation frame at the camera start: " + selected.file);
    }
    auto rink = rink_model.Infer(first_frame, RinkSegmentation::kHockeyMomInferenceScale, is_cancelled);
    if (!rink.ok()) {
      return absl::Status(
          rink.status().code(),
          "Rink inference failed for camera " + camera + ": " + std::string(rink.status().message()));
    }
    auto scores = rink_orientation_scores(rink->combined_mask);
    if (!scores.ok())
      return scores.status();
    auto orientation = classify_rink_orientation(rink->combined_mask);
    if (!orientation.ok()) {
      return absl::Status(
          orientation.status().code(),
          "Camera " + camera + " orientation failed (left=" + std::to_string(scores->left) +
              ", right=" + std::to_string(scores->right) + "): " + std::string(orientation.status().message()));
    }
    std::cout << "Camera " << camera << " orientation=" << *orientation << " left_edge=" << scores->left
              << " right_edge=" << scores->right << std::endl;
    if (oriented.count(*orientation)) {
      return absl::FailedPreconditionError("Multiple cameras classified as " + *orientation);
    }
    oriented[*orientation] = chapters;
  }
  if (!oriented.count("left") || !oriented.count("right") || oriented.size() != 2) {
    return absl::FailedPreconditionError("Native orientation did not identify exactly one left and one right camera");
  }
  return orientation_internal::save_orientation_config(
      game_dir, oriented.at("left"), oriented.at("right"), expected_invalidation_id);
}

/**
 * @brief Example main function demonstrating the usage of get_available_videos.
 *
 * This function retrieves available videos from a given directory and prints the results.
 *
 * @return int Exit code.
 */
#if 0
int main() {
  // Example directory (modify as needed)
  std::string directory = "path/to/your/video/directory";

  // Retrieve available videos without pruning.
  VideosDict videos = get_available_videos(directory, false);

  // Print out the videos dictionary.
  for (const auto& [key, chapters] : videos) {
    std::cout << "Video key: " << key << std::endl;
    for (const auto& [chapter, filename] : chapters) {
      std::cout << "  Chapter " << chapter << ": " << filename << std::endl;
    }
  }

  return 0;
}
#endif
} // namespace stitching
} // namespace hm
