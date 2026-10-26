#include "src/apps/hstream-ui/WebRinkEditor.h"

#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

#include <QtCore/QJsonArray>
#include <cmath>
#include <stdexcept>

namespace hm::web {
namespace {
template <typename T>
T take(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}
std::array<double, 3> angles(const QJsonObject& request, const QString& key) {
  const auto values = request.value(key).toArray();
  if (values.size() != 3)
    throw std::runtime_error("Expected three rotation angles");
  std::array<double, 3> result;
  for (int index = 0; index < 3; ++index) {
    if (!values[index].isDouble() || !std::isfinite(values[index].toDouble()) ||
        std::abs(values[index].toDouble()) > 180)
      throw std::runtime_error("Invalid rotation angle");
    result[index] = values[index].toDouble();
  }
  return result;
}
} // namespace

QJsonObject RinkEditor(const QJsonObject& request) {
  const auto localized = take(
      hm::stitching::LocalizeCalibrationPreviewImages(
          RequiredString(request, "pto", 4 * 1024 * 1024).toStdString(),
          RequiredString(request, "sourceDirectory", 4096).toStdString()));
  const auto lens = hm::stitching::HuginProject::ValidateLensCenters(localized);
  if (!lens.ok())
    throw std::runtime_error(lens.ToString());
  const auto project = take(hm::stitching::PrepareRinkLevelingProject(localized));
  if (project.image_sizes.size() != 2)
    throw std::runtime_error("Rink leveling requires two calibrated cameras");
  QJsonArray sizes;
  for (const auto& size : project.image_sizes)
    sizes.append(QJsonArray{double(size[0]), double(size[1])});
  QJsonObject result{{"status", "ok"}, {"imageSizes", sizes}, {"sphere", QString::fromStdString(project.pto)}};
  if (!request.contains("points"))
    return result;
  const auto method_name = RequiredString(request, "method", 16);
  if (method_name != "corners" && method_name != "posts")
    throw std::runtime_error("Unsupported rink leveling method");
  const auto method = method_name == "corners" ? hm::stitching::RinkLevelingMethod::kRinkCorners
                                               : hm::stitching::RinkLevelingMethod::kPosts;
  const auto points = request.value("points").toArray();
  if (points.size() != 2)
    throw std::runtime_error("Select points in both calibrated camera images");
  std::vector<hm::stitching::RinkLevelingLine> lines;
  for (int camera = 0; camera < 2; ++camera) {
    const auto selected = points[camera].toArray();
    if (selected.size() < 2 || selected.size() > 128 || selected.size() % 2 ||
        (method_name == "corners" && selected.size() != 2))
      throw std::runtime_error("Select complete point pairs in each camera image");
    for (int index = 0; index < selected.size(); index += 2) {
      hm::stitching::RinkLevelingLine line;
      line.image_index = camera;
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const auto point = selected[index + endpoint].toArray();
        if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble())
          throw std::runtime_error("Each original-image point requires two coordinates");
        (endpoint ? line.second : line.first) = std::array<double, 2>{{point[0].toDouble(), point[1].toDouble()}};
      }
      lines.push_back(line);
    }
  }
  result.insert(
      "input",
      QString::fromStdString(take(hm::stitching::FormatRinkLevelingPoints(lines, project.image_sizes, method))));
  if (!request.contains("rays"))
    return result;
  const auto rays = take(
      hm::stitching::ParseRinkLevelingRays(
          RequiredString(request, "rays", 128 * 1024).toStdString(), lines.size(), method));
  const auto published = angles(request, "publishedAngles");
  const auto selected = angles(request, "angles");
  std::array<double, 3> rotation;
  if (method_name == "corners") {
    const auto estimate = take(hm::stitching::EstimateRinkLevelingFromCorners(rays, published, selected[0]));
    rotation = estimate.rotation_degrees;
    result.insert("rectangleMismatch", estimate.orthogonality_error_degrees);
  } else {
    const auto estimate = take(hm::stitching::EstimateRinkLeveling(rays, published, selected[0]));
    rotation = estimate.rotation_degrees;
    result.insert("inlierCount", int(estimate.inlier_indices.size()));
    result.insert("postCount", int(lines.size()));
    result.insert("rmsResidual", estimate.rms_residual_degrees);
  }
  result.insert("angles", QJsonArray{rotation[0], rotation[1], rotation[2]});
  result.remove("sphere");
  result.remove("input");
  return result;
}
} // namespace hm::web
