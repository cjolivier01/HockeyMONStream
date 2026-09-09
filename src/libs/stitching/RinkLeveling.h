#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::stitching {

// Coordinates are original image pixel centers (the first pixel is 0,0).
struct RinkLevelingLine {
  size_t image_index{0};
  std::array<double, 2> first{};
  std::array<double, 2> second{};
};

struct RinkLevelingProject {
  std::string pto;
  std::vector<std::array<size_t, 2>> image_sizes;
};

struct RinkLevelingRayLine {
  std::array<double, 3> first{};
  std::array<double, 3> second{};
};

struct RinkLevelingEstimate {
  // Absolute shared Hugin yaw, pitch, roll; not a change from a previous fit.
  std::array<double, 3> rotation_degrees{};
  std::vector<size_t> inlier_indices;
  std::vector<double> residual_degrees;
  double rms_residual_degrees{0.0};
};

// Retains source lens calibration and poses, replacing only the output view
// with a full 3600x1800 equirectangular sphere. Run pano_trafo on this private
// project; never publish it over the game's project. Translated cameras are
// unsupported because their mapping is not a rotation of viewing rays.
absl::StatusOr<RinkLevelingProject> PrepareRinkLevelingProject(const std::string& pto);

// Formats pano_trafo's camera-index/x/y stdin triplets after validating marks
// against the dimensions returned by PrepareRinkLevelingProject.
absl::StatusOr<std::string> FormatRinkLevelingPoints(
    const std::vector<RinkLevelingLine>& lines,
    const std::vector<std::array<size_t, 2>>& image_sizes);

// Parses exactly two pano_trafo output pairs per selected line and converts
// the equirectangular pixel centers to unit rays in Hugin's PT coordinates.
absl::StatusOr<std::vector<RinkLevelingRayLine>> ParseRinkLevelingRays(
    const std::string& output,
    size_t expected_line_count);

// Uses the rotation recorded with the published PTO (not today's possibly
// edited config) to recover uncorrected rays before estimating absolute
// pitch/roll. Vertical posts cannot determine yaw, so preserved_yaw_degrees
// is copied to the result. Requires at least three consistent, spread posts.
absl::StatusOr<RinkLevelingEstimate> EstimateRinkLeveling(
    const std::vector<RinkLevelingRayLine>& lines,
    const std::array<double, 3>& published_rotation_degrees,
    double preserved_yaw_degrees);

// For a preview reusing the already-rotated published PTO, returns the Euler
// form of R(desired) * transpose(R(published)). Do not subtract Euler angles.
absl::StatusOr<std::array<double, 3>> RinkLevelingRotationDelta(
    const std::array<double, 3>& published_rotation_degrees,
    const std::array<double, 3>& desired_rotation_degrees);

} // namespace hm::stitching
