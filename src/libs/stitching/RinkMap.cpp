#include "hstream/src/libs/stitching/RinkMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {
constexpr double kDegrees = 180.0 / 3.14159265358979323846;
constexpr size_t kMaximumObservations = 128;
constexpr size_t kMaximumSamples = 64;
bool finite(cv::Vec3d p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

absl::Status ValidateObservation(const RinkMapObservation& observation) {
  if (observation.camera < 0 || observation.camera > 1 || observation.label.size() > 128 ||
      observation.mask_revision.size() > 128 || !std::isfinite(observation.confidence) || observation.confidence < 0 ||
      observation.confidence > 1 || observation.pixels.empty() || observation.pixels.size() > kMaximumSamples ||
      observation.pixels.size() != observation.rays.size() || (!observation.line && observation.pixels.size() != 1) ||
      (observation.line && observation.pixels.size() < 2))
    return absl::InvalidArgumentError("Invalid rink landmark observation");
  for (size_t i = 0; i < observation.pixels.size(); ++i) {
    const auto pixel = observation.pixels[i];
    const auto ray = observation.rays[i];
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) || pixel.x < 0 || pixel.y < 0 || pixel.x > 100000 ||
        pixel.y > 100000 || !finite(ray) || std::abs(cv::norm(ray) - 1) > 1e-5)
      return absl::InvalidArgumentError("Rink landmarks require finite source pixels and unit viewing rays");
  }
  if (!observation.feature.empty()) {
    const auto& features = RinkMapFeatures();
    const auto found = std::find_if(features.begin(), features.end(), [&](const auto& feature) {
      return feature.id == observation.feature && feature.line == observation.line;
    });
    if (found == features.end())
      return absl::InvalidArgumentError("Unknown or mismatched rink feature: " + observation.feature);
  }
  return absl::OkStatus();
}

absl::StatusOr<cv::Vec3d> LineNormal(const std::vector<cv::Vec3d>& rays) {
  cv::Matx33d covariance = cv::Matx33d::zeros();
  for (const auto& ray : rays)
    covariance += ray * ray.t();
  cv::Mat values, vectors;
  cv::eigen(cv::Mat(covariance), values, vectors);
  if (values.at<double>(1) < values.at<double>(0) * 1e-5)
    return absl::InvalidArgumentError("A rink line needs marks farther apart");
  const cv::Vec3d normal(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
  double error = 0;
  for (const auto& ray : rays)
    error = std::max(error, std::abs(normal.dot(ray)));
  if (error > std::sin(1.5 / kDegrees))
    return absl::InvalidArgumentError("A selected marking does not follow a straight line on the ice");
  return normal;
}

// Cross-product constraints, either r x H p = 0 or l x H^T n = 0.
void AddRows(std::vector<std::array<double, 9>>& rows, cv::Vec3d a, cv::Vec3d b, bool transpose, double weight) {
  const cv::Matx33d cross(0, -a[2], a[1], a[2], 0, -a[0], -a[1], a[0], 0);
  for (int i = 0; i < 3; ++i) {
    std::array<double, 9> row{};
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        row[transpose ? 3 * k + j : 3 * j + k] = weight * cross(i, j) * b[k];
    rows.push_back(row);
  }
}
} // namespace

const std::vector<RinkMapFeature>& RinkMapFeatures() {
  static const std::vector<RinkMapFeature> features{
      {"center", "Centre ice", false},
      {"center-near", "Centre line / near boards", false},
      {"center-far", "Centre line / far boards", false},
      {"blue-left-near", "Left blue line / near boards", false},
      {"blue-left-far", "Left blue line / far boards", false},
      {"blue-right-near", "Right blue line / near boards", false},
      {"blue-right-far", "Right blue line / far boards", false},
      {"goal-left", "Left goal-mouth centre (on ice)", false},
      {"goal-right", "Right goal-mouth centre (on ice)", false},
      {"goal-left-near", "Left goal line / near boards", false},
      {"goal-left-far", "Left goal line / far boards", false},
      {"goal-right-near", "Right goal line / near boards", false},
      {"goal-right-far", "Right goal line / far boards", false},
      {"center-line", "Centre line", true},
      {"blue-left-line", "Left blue line", true},
      {"blue-right-line", "Right blue line", true},
      {"goal-left-line", "Left goal line", true},
      {"goal-right-line", "Right goal line", true},
      {"near-boards", "Near straight boards at ice level", true},
      {"far-boards", "Far straight boards at ice level", true}};
  return features;
}

absl::Status ValidateRinkLayout(const RinkLayout& layout) {
  if (!std::isfinite(layout.length) || !std::isfinite(layout.width) || !std::isfinite(layout.blue_line_distance) ||
      !std::isfinite(layout.goal_inset) || !std::isfinite(layout.corner_radius) || layout.corner_radius < 0 ||
      layout.corner_radius > layout.width / 2 || layout.length < 30 || layout.length > 80 || layout.width < 12 ||
      layout.width > 40 || layout.width >= layout.length || layout.goal_inset < 1 || layout.goal_inset > 6 ||
      layout.blue_line_distance < 1 || layout.blue_line_distance >= layout.length / 2 - layout.goal_inset)
    return absl::InvalidArgumentError("Check rink length, width, blue-line spacing and goal-line inset");
  return absl::OkStatus();
}

absl::StatusOr<cv::Point2d> RinkFeaturePoint(const RinkLayout& layout, const std::string& id) {
  double x = 0, y = 0;
  if (id == "center")
    return cv::Point2d(0, 0);
  if (id.rfind("blue-left", 0) == 0)
    x = -layout.blue_line_distance;
  else if (id.rfind("blue-right", 0) == 0)
    x = layout.blue_line_distance;
  else if (id.rfind("goal-left", 0) == 0)
    x = -layout.length / 2 + layout.goal_inset;
  else if (id.rfind("goal-right", 0) == 0)
    x = layout.length / 2 - layout.goal_inset;
  else if (id.rfind("center-", 0) != 0)
    return absl::InvalidArgumentError("Unknown rink point");
  if (id.size() >= 5 && id.substr(id.size() - 5) == "-near")
    y = -layout.width / 2;
  else if (id.size() >= 4 && id.substr(id.size() - 4) == "-far")
    y = layout.width / 2;
  else if (id != "goal-left" && id != "goal-right")
    return absl::InvalidArgumentError("Unknown rink point");
  if (y != 0 && layout.corner_radius > 0 && std::abs(x) > layout.length / 2 - layout.corner_radius) {
    const double offset = std::abs(x) - (layout.length / 2 - layout.corner_radius);
    y = std::copysign(
        layout.width / 2 - layout.corner_radius +
            std::sqrt(std::max(0.0, layout.corner_radius * layout.corner_radius - offset * offset)),
        y);
  }
  return cv::Point2d(x, y);
}

absl::StatusOr<cv::Vec3d> RinkFeatureLine(const RinkLayout& layout, const std::string& id) {
  if (id == "near-boards")
    return cv::Vec3d(0, 1, layout.width / 2);
  if (id == "far-boards")
    return cv::Vec3d(0, 1, -layout.width / 2);
  if (id == "center-line")
    return cv::Vec3d(1, 0, 0);
  if (id == "blue-left-line")
    return cv::Vec3d(1, 0, layout.blue_line_distance);
  if (id == "blue-right-line")
    return cv::Vec3d(1, 0, -layout.blue_line_distance);
  if (id == "goal-left-line")
    return cv::Vec3d(1, 0, layout.length / 2 - layout.goal_inset);
  if (id == "goal-right-line")
    return cv::Vec3d(1, 0, -layout.length / 2 + layout.goal_inset);
  return absl::InvalidArgumentError("Unknown rink line");
}

absl::StatusOr<RinkMapFit> FitRinkMap(const RinkMapDocument& document) {
  auto valid = ValidateRinkLayout(document.layout);
  if (!valid.ok())
    return valid;
  if (document.observations.size() > kMaximumObservations)
    return absl::InvalidArgumentError("Too many rink observations");
  const double scale = document.layout.length / 2;
  std::vector<std::array<double, 9>> rows;
  std::vector<std::array<double, 9>> ideal_rows;
  std::vector<const RinkMapObservation*> used;
  std::vector<cv::Vec3d> normals;
  RinkMapFit result;
  result.message =
      "Partial map: add marks across and along the rink. Centre, blue and goal lines alone cannot locate players. You can save these marks and continue later.";
  bool has_points = false, has_cross_lines = false, has_board_lines = false;
  size_t assigned = 0;
  for (const auto& observation : document.observations) {
    valid = ValidateObservation(observation);
    if (!valid.ok())
      return valid;
    if (!observation.enabled || observation.feature.empty())
      continue;
    ++assigned;
    if (!observation.line)
      has_points = true;
    else if (observation.feature == "near-boards" || observation.feature == "far-boards")
      has_board_lines = true;
    else
      has_cross_lines = true;
  }
  // Noise cannot supply missing physical constraints. Keep these observations
  // available as partial evidence even on poorly painted rinks.
  if (!has_points && !(has_cross_lines && has_board_lines)) {
    result.constraints = assigned;
    return result;
  }
  for (const auto& observation : document.observations) {
    if (!observation.enabled || observation.feature.empty())
      continue;
    const double weight = observation.automatic ? std::sqrt(std::max(0.05, observation.confidence)) : 1;
    if (observation.line) {
      auto normal = LineNormal(observation.rays);
      if (!normal.ok()) {
        result.state = RinkMapState::kInconsistent;
        result.message = normal.status().message();
        return result;
      }
      auto line = *RinkFeatureLine(document.layout, observation.feature);
      line[2] /= scale;
      line /= cv::norm(line);
      AddRows(rows, line, *normal, true, weight);
      AddRows(ideal_rows, line, line, true, 1);
      normals.push_back(*normal);
    } else {
      const auto point = *RinkFeaturePoint(document.layout, observation.feature);
      AddRows(rows, observation.rays.front(), {point.x / scale, point.y / scale, 1}, false, weight);
      const cv::Vec3d ideal(point.x / scale, point.y / scale, 1);
      AddRows(ideal_rows, ideal / cv::norm(ideal), ideal, false, 1);
      normals.emplace_back();
    }
    used.push_back(&observation);
  }
  result.constraints = used.size();
  if (rows.size() < 9)
    return result;
  // Check the selected physical features independently of measured rays.
  // Noise in repeated/collinear points or parallel lines must not manufacture
  // the missing cross-rink constraint and turn a partial map into a rejection.
  cv::Mat ideal_design(static_cast<int>(ideal_rows.size()), 9, CV_64F);
  for (size_t i = 0; i < ideal_rows.size(); ++i)
    std::copy(ideal_rows[i].begin(), ideal_rows[i].end(), ideal_design.ptr<double>(static_cast<int>(i)));
  cv::Mat ideal_values;
  cv::SVD::compute(ideal_design, ideal_values);
  if (ideal_values.total() < 9 || ideal_values.at<double>(7) < ideal_values.at<double>(0) * 1e-8)
    return result;
  cv::Mat design(static_cast<int>(rows.size()), 9, CV_64F);
  for (size_t i = 0; i < rows.size(); ++i)
    std::copy(rows[i].begin(), rows[i].end(), design.ptr<double>(static_cast<int>(i)));
  cv::SVD svd(design, cv::SVD::FULL_UV);
  // Duplicate detections and parallel markings do not add missing geometry.
  if (svd.w.total() < 9 || svd.w.at<double>(7) < svd.w.at<double>(0) * 1e-3)
    return result;
  cv::Matx33d transform;
  std::copy(svd.vt.ptr<double>(8), svd.vt.ptr<double>(8) + 9, transform.val);
  for (int row = 0; row < 3; ++row) {
    transform(row, 0) /= scale;
    transform(row, 1) /= scale;
  }
  double sign = 0;
  for (const auto* observation : used) {
    if (!observation->line) {
      const auto point = *RinkFeaturePoint(document.layout, observation->feature);
      sign += (transform * cv::Vec3d(point.x, point.y, 1)).dot(observation->rays.front());
    }
  }
  // Lines have no direction. Pick the sign whose plane faces their rays.
  if (sign == 0) {
    const auto inverse = transform.inv();
    for (const auto* observation : used)
      for (const auto& ray : observation->rays)
        sign += (inverse * ray)[2] > 0 ? 1 : -1;
  }
  if (sign < 0)
    transform *= -1;
  const cv::Vec3d x(transform(0, 0), transform(1, 0), transform(2, 0));
  const cv::Vec3d y(transform(0, 1), transform(1, 1), transform(2, 1));
  const cv::Vec3d origin(transform(0, 2), transform(1, 2), transform(2, 2));
  const double nx = cv::norm(x), ny = cv::norm(y);
  const auto normal = x.cross(y);
  const double height = std::abs(normal.dot(origin)) / std::max(1e-20, cv::norm(normal) * (nx + ny) / 2);
  result.state = RinkMapState::kInconsistent;
  result.message =
      "Cannot fit a reliable rink map from these marks yet. You can save them as a partial map. Check the selected landmarks, rink dimensions and camera calibration; player locations remain unavailable.";
  if (nx < 1e-10 || ny < 1e-10 || nx / ny < 0.8 || nx / ny > 1.25 || std::abs(x.dot(y)) / (nx * ny) > 0.15 ||
      height < 0.3 || height > 50)
    return result;
  const auto inverse = transform.inv();
  double squared_error = 0, maximum_error = 0;
  size_t count = 0;
  for (size_t i = 0; i < used.size(); ++i) {
    const auto& observation = *used[i];
    double error = 0;
    if (observation.line) {
      auto predicted = inverse.t() * *RinkFeatureLine(document.layout, observation.feature);
      predicted /= cv::norm(predicted);
      error = std::acos(std::clamp(std::abs(predicted.dot(normals[i])), 0.0, 1.0)) * kDegrees;
    } else {
      const auto point = *RinkFeaturePoint(document.layout, observation.feature);
      auto predicted = transform * cv::Vec3d(point.x, point.y, 1);
      predicted /= cv::norm(predicted);
      error = std::acos(std::clamp(predicted.dot(observation.rays.front()), -1.0, 1.0)) * kDegrees;
    }
    for (const auto& ray : observation.rays) {
      if ((inverse * ray)[2] <= 0)
        return result;
    }
    squared_error += error * error;
    maximum_error = std::max(maximum_error, error);
    ++count;
  }
  result.rms_degrees = std::sqrt(squared_error / count);
  if (result.rms_degrees > 1 || maximum_error > 2)
    return result;
  result.state = RinkMapState::kReady;
  result.plane_to_ray = transform;
  result.message =
      "Rink map fitted. Goal centres and end-board extents are estimates from the selected rink dimensions; review them in both cameras.";
  return result;
}

absl::StatusOr<cv::Vec3d> RinkPointToRay(const RinkMapFit& fit, cv::Point2d point) {
  if (fit.state != RinkMapState::kReady || !std::isfinite(point.x) || !std::isfinite(point.y))
    return absl::FailedPreconditionError("A complete rink map is required");
  auto ray = fit.plane_to_ray * cv::Vec3d(point.x, point.y, 1);
  const double norm = cv::norm(ray);
  if (!finite(ray) || norm < 1e-12)
    return absl::InvalidArgumentError("Rink point has no valid viewing ray");
  return ray / norm;
}

absl::StatusOr<cv::Point2d> RinkRayToPoint(const RinkMapFit& fit, cv::Vec3d ray) {
  if (fit.state != RinkMapState::kReady || !finite(ray) || cv::norm(ray) < 1e-12)
    return absl::FailedPreconditionError("A complete rink map and finite ray are required");
  const auto point = fit.plane_to_ray.inv() * ray;
  if (!finite(point) || point[2] <= 1e-9)
    return absl::OutOfRangeError("Viewing ray does not intersect the rink in front of the camera");
  return cv::Point2d(point[0] / point[2], point[1] / point[2]);
}

std::vector<std::vector<cv::Point2d>> RinkLayoutDrawing(const RinkLayout& layout) {
  std::vector<std::vector<cv::Point2d>> lines;
  const double lx = layout.length / 2, wy = layout.width / 2;
  std::vector<cv::Point2d> outline;
  for (int corner = 0; corner < 4; ++corner) {
    const double cx = (corner == 0 || corner == 3 ? 1 : -1) * (lx - layout.corner_radius);
    const double cy = (corner < 2 ? 1 : -1) * (wy - layout.corner_radius);
    for (int step = 0; step <= 12; ++step) {
      const double angle = (corner * 90.0 + step * 7.5) / kDegrees;
      outline.emplace_back(cx + layout.corner_radius * std::cos(angle), cy + layout.corner_radius * std::sin(angle));
    }
  }
  outline.push_back(outline.front());
  lines.push_back(std::move(outline));
  for (double x :
       {-lx + layout.goal_inset, -layout.blue_line_distance, 0.0, layout.blue_line_distance, lx - layout.goal_inset}) {
    double edge = wy;
    if (layout.corner_radius > 0 && std::abs(x) > lx - layout.corner_radius) {
      const double offset = std::abs(x) - (lx - layout.corner_radius);
      edge = wy - layout.corner_radius +
          std::sqrt(std::max(0.0, layout.corner_radius * layout.corner_radius - offset * offset));
    }
    lines.push_back({{x, -edge}, {x, edge}});
  }
  return lines;
}

absl::StatusOr<std::string> SerializeRinkMap(const RinkMapDocument& document) {
  auto fit = FitRinkMap(document);
  if (!fit.ok())
    return fit.status();
  if (document.generation.empty() || document.generation.size() > 256 || document.calibration_revision.empty() ||
      document.calibration_revision.size() > 256)
    return absl::InvalidArgumentError("Rink map needs its calibration identity");
  YAML::Node root;
  root["schema"] = "hstream-rink-map-v1";
  root["generation"] = document.generation;
  root["calibration_revision"] = document.calibration_revision;
  root["dimensions_m"] = std::vector<double>{
      document.layout.length,
      document.layout.width,
      document.layout.blue_line_distance,
      document.layout.goal_inset,
      document.layout.corner_radius};
  root["status"] = fit->state == RinkMapState::kReady ? "estimated" : "partial";
  root["observations"] = YAML::Node(YAML::NodeType::Sequence);
  for (const auto& observation : document.observations) {
    YAML::Node entry;
    entry["feature"] = observation.feature;
    entry["label"] = observation.label;
    entry["camera"] = observation.camera;
    entry["line"] = observation.line;
    entry["automatic"] = observation.automatic;
    if (!observation.mask_revision.empty())
      entry["mask_revision"] = observation.mask_revision;
    entry["enabled"] = observation.enabled;
    entry["confidence"] = observation.confidence;
    for (size_t i = 0; i < observation.pixels.size(); ++i) {
      const auto p = observation.pixels[i];
      const auto r = observation.rays[i];
      entry["samples"].push_back(std::vector<double>{p.x, p.y, r[0], r[1], r[2]});
    }
    root["observations"].push_back(entry);
  }
  YAML::Emitter output;
  output.SetDoublePrecision(17);
  output << root;
  return std::string(output.c_str());
}

absl::StatusOr<RinkMapDocument> ParseRinkMap(const std::string& text) {
  if (text.size() > 512 * 1024)
    return absl::ResourceExhaustedError("Rink map exceeds the size limit");
  try {
    const auto root = YAML::Load(text);
    if (root["schema"].as<std::string>() != "hstream-rink-map-v1")
      return absl::InvalidArgumentError("Unsupported rink map schema");
    RinkMapDocument result;
    result.generation = root["generation"].as<std::string>();
    result.calibration_revision = root["calibration_revision"].as<std::string>();
    const auto dimensions = root["dimensions_m"].as<std::vector<double>>();
    if (dimensions.size() != 5)
      return absl::InvalidArgumentError("Rink map needs five layout dimensions");
    result.layout = {dimensions[0], dimensions[1], dimensions[2], dimensions[3], dimensions[4]};
    const auto observations = root["observations"];
    if (!observations.IsSequence() || observations.size() > kMaximumObservations)
      return absl::InvalidArgumentError("Invalid rink map observations");
    for (const auto& entry : observations) {
      RinkMapObservation observation;
      observation.feature = entry["feature"].as<std::string>();
      observation.label = entry["label"].as<std::string>();
      observation.camera = entry["camera"].as<int>();
      observation.line = entry["line"].as<bool>();
      observation.automatic = entry["automatic"].as<bool>();
      observation.mask_revision = entry["mask_revision"].as<std::string>("");
      observation.enabled = entry["enabled"].as<bool>();
      observation.confidence = entry["confidence"].as<double>();
      const auto samples = entry["samples"];
      if (!samples.IsSequence() || samples.size() > kMaximumSamples)
        return absl::InvalidArgumentError("Invalid rink map samples");
      for (const auto& sample : samples) {
        const auto values = sample.as<std::vector<double>>();
        if (values.size() != 5)
          return absl::InvalidArgumentError("Invalid rink map sample");
        observation.pixels.emplace_back(values[0], values[1]);
        observation.rays.emplace_back(values[2], values[3], values[4]);
      }
      result.observations.push_back(std::move(observation));
    }
    auto serialized = SerializeRinkMap(result);
    if (!serialized.ok())
      return serialized.status();
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid rink map: ") + error.what());
  }
}
} // namespace hm::stitching
