#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {

inline constexpr size_t kMaximumEditableMatchesPerPair = 10000;

struct CalibrationMatchFrame {
  std::array<std::filesystem::path, 2> images;
  std::array<cv::Size, 2> sizes;
  std::array<std::string, 2> source_paths;
  std::array<double, 2> source_seconds{};
  // Original camera pixels, including for calibrated AKAZE. Solver conversion
  // uses the immutable lens calibration saved with this match set.
  std::vector<FeatureMatch> matches;
};

struct CalibrationMatchSet {
  std::string fingerprint;
  std::string input_fingerprint;
  std::string automatic_fingerprint;
  std::string selection_fingerprint;
  std::string source_context;
  ControlPointMatcher matcher{ControlPointMatcher::kSuperPointLightGlue};
  AkazeMatchingCalibration calibration;
  bool manual{false};
  std::vector<CalibrationMatchFrame> frames;
};

// Match documents and full-resolution PNG input bundles are immutable and
// content-addressed. Edits share the input bundle rather than copying its PNGs.
absl::StatusOr<CalibrationMatchSet> LoadCalibrationMatches(
    const std::filesystem::path& game,
    const std::string& fingerprint,
    bool verify_images = true);
absl::StatusOr<std::string> PublishCalibrationMatches(
    const std::filesystem::path& game,
    const CalibrationMatchSet& matches);
absl::Status CopyCalibrationMatches(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string& fingerprint);

// Bind overrides to camera media/order/synchronization and the capture anchor.
absl::StatusOr<std::string> CalibrationMatchSourceContext(const YAML::Node& config, const std::filesystem::path& game);
absl::Status ValidateCalibrationMatchInputs(
    const CalibrationMatchSet& matches,
    const YAML::Node& config,
    const std::filesystem::path& game,
    size_t frame_count);
// Check the effective profile already loaded by the runner, including lens
// values as well as the profile identity used when these matches were captured.
absl::Status ValidateCalibrationMatchCalibration(
    const CalibrationMatchSet& matches,
    const AkazeMatchingCalibration& current);
absl::StatusOr<std::vector<FeatureMatch>> ConvertCalibrationMatchCoordinates(
    const std::vector<FeatureMatch>& matches,
    const std::array<cv::Size, 2>& sizes,
    const AkazeMatchingCalibration& calibration,
    bool to_camera_pixels);

} // namespace hm::stitching
