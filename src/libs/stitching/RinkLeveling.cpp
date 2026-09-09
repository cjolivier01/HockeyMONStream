#include "hstream/src/libs/stitching/RinkLeveling.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

#include <opencv2/core.hpp>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadians = kPi / 180.0;
constexpr size_t kMaximumLines = 64;
constexpr double kInlierAngle = 3.0 * kRadians;
using Vector = cv::Vec3d;
using Matrix = cv::Matx33d;

bool valid_rotation(const std::array<double, 3>& angles) {
  return std::all_of(angles.begin(), angles.end(), [](double angle) {
    return std::isfinite(angle) && angle >= -180.0 && angle <= 180.0;
  });
}

// Hugin SetRotationPT: Rz(-yaw) Ry(-pitch) Rx(roll). The PT axes are
// x forward, y left, z up. The shared rotation left-multiplies camera poses.
Matrix rotation_matrix(const std::array<double, 3>& angles) {
  const double y = -angles[0] * kRadians;
  const double p = -angles[1] * kRadians;
  const double r = angles[2] * kRadians;
  const Matrix yaw(std::cos(y), -std::sin(y), 0, std::sin(y), std::cos(y), 0, 0, 0, 1);
  const Matrix pitch(std::cos(p), 0, std::sin(p), 0, 1, 0, -std::sin(p), 0, std::cos(p));
  const Matrix roll(1, 0, 0, 0, std::cos(r), -std::sin(r), 0, std::sin(r), std::cos(r));
  return yaw * pitch * roll;
}

bool read_number(std::string_view text, double* number) {
  std::istringstream input{std::string(text)};
  input.imbue(std::locale::classic());
  if (!(input >> *number) || !std::isfinite(*number))
    return false;
  input >> std::ws;
  return input.eof();
}

// PTO quoted filenames may contain spaces or escaped quotes. Only inspect
// actual fields; a filename containing "TrX1" must not look like translation.
absl::StatusOr<std::vector<std::string>> fields(const std::string& line) {
  std::vector<std::string> result;
  std::string token;
  bool quoted = false;
  bool escaped = false;
  for (char character : line) {
    if (!quoted && (character == ' ' || character == '\t' || character == '\r')) {
      if (!token.empty()) {
        result.push_back(std::move(token));
        token.clear();
      }
      continue;
    }
    token += character;
    if (character == '"' && !escaped)
      quoted = !quoted;
    escaped = character == '\\' && !escaped;
  }
  if (quoted)
    return absl::InvalidArgumentError("The stitching project contains an unterminated quoted field");
  if (!token.empty())
    result.push_back(std::move(token));
  return result;
}

absl::StatusOr<Vector> normalized(const std::array<double, 3>& input) {
  for (double value : input) {
    if (!std::isfinite(value))
      return absl::InvalidArgumentError("A selected point has an invalid viewing ray");
  }
  const Vector value(input[0], input[1], input[2]);
  const double length = cv::norm(value);
  if (!std::isfinite(length) || length < 1e-12)
    return absl::InvalidArgumentError("A selected point has an empty viewing ray");
  return value / length;
}

double residual(const Vector& normal, const Vector& up) {
  return std::asin(std::clamp(std::abs(normal.dot(up)), 0.0, 1.0));
}

std::vector<size_t> inliers(const std::vector<Vector>& normals, const Vector& up) {
  std::vector<size_t> result;
  for (size_t index = 0; index < normals.size(); ++index) {
    if (residual(normals[index], up) <= kInlierAngle)
      result.push_back(index);
  }
  return result;
}

absl::StatusOr<Vector> refine(
    const std::vector<Vector>& normals,
    const std::vector<double>& weights,
    const std::vector<size_t>& selected) {
  Matrix covariance = Matrix::zeros();
  for (size_t index : selected)
    covariance += weights[index] * (normals[index] * normals[index].t());
  cv::Mat values;
  cv::Mat vectors;
  if (!cv::eigen(cv::Mat(covariance), values, vectors) || values.at<double>(1) < 0.0225 * values.at<double>(0)) {
    return absl::InvalidArgumentError("Choose posts farther apart across the rink; these marks cannot determine tilt");
  }
  return Vector(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
}

} // namespace

absl::StatusOr<RinkLevelingProject> PrepareRinkLevelingProject(const std::string& pto) {
  if (pto.empty() || pto.size() > 16 * 1024 * 1024)
    return absl::InvalidArgumentError("The stitching project is empty or too large");
  RinkLevelingProject result;
  size_t panoramas = 0;
  std::vector<size_t> translation_links;
  std::istringstream input(pto);
  std::string line;
  while (std::getline(input, line)) {
    const size_t start = line.find_first_not_of(" \t\r");
    const bool record =
        start != std::string::npos && start + 1 < line.size() && (line[start + 1] == ' ' || line[start + 1] == '\t');
    if (record && line[start] == 'p') {
      ++panoramas;
      result.pto += "p f2 w3600 h1800 v360 n\"TIFF_m c:LZW r:CROP\"\n";
      continue;
    }
    if (record && line[start] == 'i') {
      auto tokens = fields(line.substr(start));
      if (!tokens.ok())
        return tokens.status();
      std::array<size_t, 2> dimensions{};
      for (const std::string& token : *tokens) {
        if (token.size() > 1 && (token[0] == 'w' || token[0] == 'h')) {
          double value = 0;
          const size_t index = token[0] == 'w' ? 0 : 1;
          if (dimensions[index] || !read_number(std::string_view(token).substr(1), &value) || value < 1 ||
              value > 262144 || std::floor(value) != value) {
            return absl::InvalidArgumentError("The stitching project has invalid source image dimensions");
          }
          dimensions[index] = static_cast<size_t>(value);
        }
        if (token.rfind("TrX", 0) == 0 || token.rfind("TrY", 0) == 0 || token.rfind("TrZ", 0) == 0) {
          double value = 0;
          std::string_view number = std::string_view(token).substr(3);
          const bool linked = !number.empty() && number.front() == '=';
          if (linked)
            number.remove_prefix(1);
          if (!read_number(number, &value) || (!linked && value != 0.0) ||
              (linked && (value < 0 || value >= 64 || std::floor(value) != value))) {
            return absl::InvalidArgumentError("Rink leveling requires a stitching project without camera translation");
          }
          if (linked)
            translation_links.push_back(static_cast<size_t>(value));
        }
      }
      if (!dimensions[0] || !dimensions[1])
        return absl::InvalidArgumentError("The stitching project has missing source image dimensions");
      result.image_sizes.push_back(dimensions);
    }
    result.pto += line + '\n';
  }
  if (panoramas != 1 || result.image_sizes.empty() || result.image_sizes.size() > 64)
    return absl::InvalidArgumentError("The stitching project must contain one panorama and valid source images");
  for (size_t linked : translation_links) {
    if (linked >= result.image_sizes.size())
      return absl::InvalidArgumentError("The stitching project links a missing source image");
  }
  return result;
}

absl::StatusOr<std::string> FormatRinkLevelingPoints(
    const std::vector<RinkLevelingLine>& lines,
    const std::vector<std::array<size_t, 2>>& image_sizes) {
  if (lines.size() < 3 || lines.size() > kMaximumLines)
    return absl::InvalidArgumentError("Select between 3 and 64 vertical posts");
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10);
  for (const auto& line : lines) {
    if (line.image_index >= image_sizes.size())
      return absl::InvalidArgumentError("A selected post refers to a missing camera image");
    const auto& size = image_sizes[line.image_index];
    for (const auto& point : {line.first, line.second}) {
      if (!size[0] || !size[1] || !std::isfinite(point[0]) || !std::isfinite(point[1]) || point[0] < 0 ||
          point[1] < 0 || point[0] > size[0] - 1 || point[1] > size[1] - 1) {
        return absl::InvalidArgumentError("A selected post extends outside its camera image");
      }
      output << line.image_index << ' ' << point[0] << ' ' << point[1] << '\n';
    }
    if (std::hypot(line.first[0] - line.second[0], line.first[1] - line.second[1]) < 4.0)
      return absl::InvalidArgumentError("Mark a taller section of each post");
  }
  return output.str();
}

absl::StatusOr<std::vector<RinkLevelingRayLine>> ParseRinkLevelingRays(
    const std::string& output,
    size_t expected_line_count) {
  if (expected_line_count < 3 || expected_line_count > kMaximumLines || output.size() > 32768)
    return absl::InvalidArgumentError("Unexpected number of transformed rink marks");
  std::istringstream input(output);
  input.imbue(std::locale::classic());
  std::vector<RinkLevelingRayLine> result(expected_line_count);
  for (auto& line : result) {
    for (auto* point : {&line.first, &line.second}) {
      double x = 0;
      double y = 0;
      if (!(input >> x >> y) || !std::isfinite(x) || !std::isfinite(y) || x < -0.500001 || x > 3599.500001 ||
          y < -0.500001 || y > 1799.500001) {
        return absl::InvalidArgumentError("A selected point could not be transformed by the stitching calibration");
      }
      const double longitude = (x - 1799.5) * kRadians / 10.0;
      const double latitude = (899.5 - y) * kRadians / 10.0;
      *point = {
          std::cos(latitude) * std::cos(longitude), -std::cos(latitude) * std::sin(longitude), std::sin(latitude)};
    }
  }
  input >> std::ws;
  if (!input.eof())
    return absl::InvalidArgumentError("The stitching transform returned unexpected extra output");
  return result;
}

absl::StatusOr<RinkLevelingEstimate> EstimateRinkLeveling(
    const std::vector<RinkLevelingRayLine>& lines,
    const std::array<double, 3>& published_rotation_degrees,
    double preserved_yaw_degrees) {
  if (lines.size() < 3 || lines.size() > kMaximumLines)
    return absl::InvalidArgumentError("Select between 3 and 64 vertical posts");
  if (!valid_rotation(published_rotation_degrees) || !valid_rotation({preserved_yaw_degrees, 0, 0}))
    return absl::InvalidArgumentError("The saved rink rotation contains an invalid angle");
  const Matrix undo = rotation_matrix(published_rotation_degrees).t();
  std::vector<Vector> normals;
  std::vector<double> weights;
  for (const auto& line : lines) {
    auto first = normalized(line.first);
    auto second = normalized(line.second);
    if (!first.ok())
      return first.status();
    if (!second.ok())
      return second.status();
    const Vector a = undo * *first;
    const Vector b = undo * *second;
    const Vector cross = a.cross(b);
    const double length = cv::norm(cross);
    const double angle = std::atan2(length, a.dot(b));
    if (angle < 0.5 * kRadians || angle > 90.0 * kRadians)
      return absl::InvalidArgumentError("Mark a taller, clearly visible section of each vertical post");
    normals.push_back(cross / length);
    weights.push_back(std::pow(std::min(length, std::sin(20.0 * kRadians)), 2));
  }
  std::vector<size_t> selected;
  double best_cost = std::numeric_limits<double>::infinity();
  for (size_t first = 0; first < normals.size(); ++first) {
    for (size_t second = 0; second < first; ++second) {
      Vector candidate = normals[first].cross(normals[second]);
      const double length = cv::norm(candidate);
      if (length < 0.15)
        continue;
      candidate /= length;
      const auto candidate_inliers = inliers(normals, candidate);
      double cost = 0;
      for (const auto& normal : normals)
        cost += std::pow(std::min(residual(normal, candidate), kInlierAngle), 2);
      if (candidate_inliers.size() > selected.size() ||
          (candidate_inliers.size() == selected.size() && cost < best_cost)) {
        selected = candidate_inliers;
        best_cost = cost;
      }
    }
  }
  const size_t required = std::max<size_t>(3, (7 * lines.size() + 9) / 10);
  Vector up;
  bool stable = false;
  for (size_t iteration = 0; iteration < 8; ++iteration) {
    if (selected.size() < required)
      return absl::InvalidArgumentError("The marks disagree; select at least three well-spaced vertical posts");
    auto fitted = refine(normals, weights, selected);
    if (!fitted.ok())
      return fitted.status();
    up = *fitted;
    auto next = inliers(normals, up);
    if (next == selected) {
      stable = true;
      break;
    }
    selected = std::move(next);
  }
  if (!stable)
    return absl::InvalidArgumentError("These marks do not give a stable tilt; adjust the post endpoints");
  if (up.dot(undo * Vector(0, 0, 1)) < 0)
    up = -up;
  RinkLevelingEstimate result;
  result.rotation_degrees = {
      preserved_yaw_degrees,
      std::atan2(up[0], std::hypot(up[1], up[2])) / kRadians,
      std::atan2(up[1], up[2]) / kRadians};
  result.inlier_indices = selected;
  for (const auto& normal : normals)
    result.residual_degrees.push_back(residual(normal, up) / kRadians);
  for (size_t index : selected)
    result.rms_residual_degrees += std::pow(result.residual_degrees[index], 2);
  result.rms_residual_degrees = std::sqrt(result.rms_residual_degrees / selected.size());
  return result;
}

absl::StatusOr<std::array<double, 3>> RinkLevelingRotationDelta(
    const std::array<double, 3>& published_rotation_degrees,
    const std::array<double, 3>& desired_rotation_degrees) {
  if (!valid_rotation(published_rotation_degrees) || !valid_rotation(desired_rotation_degrees))
    return absl::InvalidArgumentError("The rink rotation contains an invalid angle");
  const Matrix delta = rotation_matrix(desired_rotation_degrees) * rotation_matrix(published_rotation_degrees).t();
  const double pitch = std::asin(std::clamp(delta(2, 0), -1.0, 1.0));
  if (std::hypot(delta(0, 0), delta(1, 0)) < 1e-10) {
    return std::array<double, 3>{std::atan2(delta(0, 1), delta(1, 1)) / kRadians, pitch / kRadians, 0};
  }
  return std::array<double, 3>{
      std::atan2(-delta(1, 0), delta(0, 0)) / kRadians,
      pitch / kRadians,
      std::atan2(delta(2, 1), delta(2, 2)) / kRadians};
}

} // namespace hm::stitching
