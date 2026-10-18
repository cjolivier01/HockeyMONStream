#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "absl/status/statusor.h"

namespace hm::stitching {

// Metres, with centre ice at (0, 0), the left goal at negative x and the
// near boards at negative y. Dimensions are an explicit operator assumption.
struct RinkLayout {
  double length{60.96};
  double width{25.908};
  double blue_line_distance{7.62};
  double goal_inset{3.3528};
  double corner_radius{8.5344};
};

struct RinkMapFeature {
  const char* id;
  const char* name;
  bool line;
};

const std::vector<RinkMapFeature>& RinkMapFeatures();
absl::Status ValidateRinkLayout(const RinkLayout& layout);
absl::StatusOr<cv::Point2d> RinkFeaturePoint(const RinkLayout& layout, const std::string& id);
absl::StatusOr<cv::Vec3d> RinkFeatureLine(const RinkLayout& layout, const std::string& id);

// Original source pixels and their calibrated, common-camera unit rays.
// An empty feature preserves an unassigned detection without using it to fit.
// Lines carry samples along the marking, never assumed board intersections.
struct RinkMapObservation {
  std::string feature;
  std::string label;
  int camera{0};
  bool line{false};
  bool automatic{false};
  bool enabled{true};
  double confidence{1};
  std::vector<cv::Point2d> pixels;
  std::vector<cv::Vec3d> rays;
};

struct RinkMapDocument {
  std::string generation;
  std::string calibration_revision;
  RinkLayout layout;
  std::vector<RinkMapObservation> observations;
};

enum class RinkMapState { kPartial, kInconsistent, kReady };

struct RinkMapFit {
  RinkMapState state{RinkMapState::kPartial};
  std::string message;
  // Projective plane -> calibrated ray, independent of output projection/crop.
  // Valid only in kReady. No fabricated transform is returned for sparse input.
  cv::Matx33d plane_to_ray{cv::Matx33d::zeros()};
  double rms_degrees{0};
  size_t constraints{0};
};

// Fits points and line planes together. Parallel centre/blue/goal lines alone
// remain underconstrained, even when every line is confidently detected.
absl::StatusOr<RinkMapFit> FitRinkMap(const RinkMapDocument& document);
absl::StatusOr<cv::Vec3d> RinkPointToRay(const RinkMapFit& fit, cv::Point2d point);
absl::StatusOr<cv::Point2d> RinkRayToPoint(const RinkMapFit& fit, cv::Vec3d ray);
std::vector<std::vector<cv::Point2d>> RinkLayoutDrawing(const RinkLayout& layout);

// Bounded, versioned evidence. The fitted transform is always recomputed on
// load; a stored matrix can never turn incomplete observations into a map.
absl::StatusOr<std::string> SerializeRinkMap(const RinkMapDocument& document);
absl::StatusOr<RinkMapDocument> ParseRinkMap(const std::string& text);

} // namespace hm::stitching
