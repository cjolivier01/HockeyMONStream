#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "opencv2/core.hpp"

namespace hm::stitching {

enum class MappingBackend {
  kNona,
  kOpenCvMagsac,
  kOpenCvAffineRansac,
};

const char* MappingBackendName(MappingBackend backend);
absl::StatusOr<MappingBackend> ParseMappingBackend(const std::string& value);

struct HomographyMapResult {
  int canvas_width{0};
  int canvas_height{0};
  size_t inlier_count{0};
  std::vector<double> right_to_left_homography;
  std::vector<unsigned char> inlier_mask;
};

absl::StatusOr<HomographyMapResult> CreateOpenCvMappingFiles(
    const std::filesystem::path& directory,
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    const std::vector<FeatureMatch>& matches,
    MappingBackend backend,
    const std::optional<size_t>& max_canvas_dimension = std::nullopt,
    const std::optional<size_t>& max_output_width = std::nullopt);

} // namespace hm::stitching
