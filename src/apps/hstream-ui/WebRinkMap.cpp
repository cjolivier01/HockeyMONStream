#include "src/apps/hstream-ui/WebRinkMap.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "src/apps/hstream-ui/RinkMapBackend.h"

#include <QtCore/QCryptographicHash>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace hm::web {
namespace {
namespace rink = hm::stitching;
template <typename T>
T take(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void fields(const QJsonObject& object, const std::set<QString>& allowed) {
  for (auto item = object.begin(); item != object.end(); ++item)
    require(allowed.count(item.key()), "Unsupported rink-map field");
}
QString string(const QJsonObject& object, const QString& key, int maximum = 128) {
  const auto value = object.value(key);
  require(
      value.isString() && value.toString().size() <= maximum && !value.toString().contains(QChar(0)),
      "Invalid rink-map text");
  return value.toString();
}
bool boolean(const QJsonObject& object, const QString& key) {
  require(object.value(key).isBool(), "Expected rink-map boolean");
  return object.value(key).toBool();
}
QJsonArray points(const std::vector<cv::Point2d>& values) {
  QJsonArray result;
  for (const auto& point : values)
    result.append(QJsonArray{point.x, point.y});
  return result;
}
QJsonObject observation(const rink::RinkMapObservation& value) {
  return {
      {"feature", text(value.feature)},
      {"label", text(value.label)},
      {"camera", value.camera},
      {"line", value.line},
      {"arc", value.arc},
      {"automatic", value.automatic},
      {"enabled", value.enabled},
      {"confidence", value.confidence},
      {"mask_revision", text(value.mask_revision)},
      {"pixels", points(value.pixels)}};
}
QJsonObject document(const rink::RinkMapDocument& value) {
  QJsonArray observations;
  for (const auto& item : value.observations)
    observations.append(observation(item));
  const auto& d = value.layout;
  return {
      {"generation", text(value.generation)},
      {"calibration_revision", text(value.calibration_revision)},
      {"dimensions_m", QJsonArray{d.length, d.width, d.blue_line_distance, d.goal_inset, d.corner_radius}},
      {"observations", observations}};
}
QJsonObject identity(const RinkMapSnapshot& snapshot) {
  return {
      {"generation", text(snapshot.generation)},
      {"revision", QString::fromLatin1(snapshot.revision)},
      {"saved_map_revision",
       QString::fromLatin1(
           QCryptographicHash::hash(
               QByteArray::fromStdString(snapshot.saved_map.value_or("")), QCryptographicHash::Sha256)
               .toHex())}};
}
rink::RinkMapDocument draft(const QJsonObject& object, const RinkMapSnapshot& snapshot) {
  fields(object, {"generation", "calibration_revision", "dimensions_m", "observations"});
  rink::RinkMapDocument result;
  result.generation = string(object, "generation").toStdString();
  result.calibration_revision = string(object, "calibration_revision").toStdString();
  require(
      result.generation == snapshot.generation && result.calibration_revision == snapshot.revision.toStdString(),
      "Rink-map draft belongs to another calibration");
  const auto dims = object.value("dimensions_m").toArray();
  require(dims.size() == 5, "Expected five rink dimensions");
  for (const auto& value : dims)
    require(value.isDouble() && std::isfinite(value.toDouble()), "Invalid rink dimension");
  result.layout = {dims[0].toDouble(), dims[1].toDouble(), dims[2].toDouble(), dims[3].toDouble(), dims[4].toDouble()};
  const auto status = rink::ValidateRinkLayout(result.layout);
  if (!status.ok())
    throw std::runtime_error(status.ToString());
  require(object.value("observations").isArray(), "Expected rink-map observations");
  const auto observations = object.value("observations").toArray();
  require(observations.size() <= 128, "Keep at most 128 rink observations");
  for (const auto& entry : observations) {
    require(entry.isObject(), "Expected rink-map observation");
    const auto item = entry.toObject();
    fields(
        item,
        {"feature", "label", "camera", "line", "arc", "automatic", "enabled", "confidence", "mask_revision", "pixels"});
    rink::RinkMapObservation mark;
    mark.feature = string(item, "feature").toStdString();
    mark.label = string(item, "label").toStdString();
    mark.mask_revision = string(item, "mask_revision").toStdString();
    const double camera = Number(item, "camera", 0, 1, -1);
    require(camera == 0 || camera == 1, "Invalid rink-map camera");
    mark.camera = int(camera);
    mark.line = boolean(item, "line");
    mark.arc = boolean(item, "arc");
    mark.automatic = boolean(item, "automatic");
    mark.enabled = boolean(item, "enabled");
    mark.confidence = Number(item, "confidence", 0, 1, -1);
    require(mark.confidence >= 0, "Missing observation confidence");
    require(item.value("pixels").isArray(), "Expected original-image coordinates");
    const auto pixels = item.value("pixels").toArray();
    require(pixels.size() > 0 && pixels.size() <= 64, "Invalid rink observation sample count");
    for (const auto& pixel : pixels) {
      const auto point = pixel.toArray();
      require(point.size() == 2 && point[0].isDouble() && point[1].isDouble(), "Invalid original-image point");
      mark.pixels.emplace_back(point[0].toDouble(), point[1].toDouble());
    }
    result.observations.push_back(std::move(mark));
  }
  // UpdateRinkMap resolves every ray from the current PTO, then calls the shared
  // observation/feature/fit validators. Browser-supplied rays are never accepted.
  return result;
}
QJsonArray drawing(const std::vector<std::vector<cv::Point2d>>& values) {
  QJsonArray result;
  for (const auto& line : values)
    result.append(points(line));
  return result;
}
} // namespace

QJsonObject RunRinkMap(const Request& request, QJsonArray& artifacts, const std::function<bool()>& cancelled) {
  auto snapshot = take(LoadRinkMapSnapshot(text(request.game.string())));
  rink::RinkMapDocument selected;
  selected.generation = snapshot.generation;
  selected.calibration_revision = snapshot.revision.toStdString();
  selected.layout = take(ReadRinkMapDefaults());
  QString notice;
  if (request.operation == "rink-map-read") {
    if (snapshot.saved_map) {
      const auto saved = rink::ParseRinkMap(*snapshot.saved_map);
      if (!saved.ok())
        notice = text(saved.status().ToString());
      else if (saved->generation != selected.generation || saved->calibration_revision != selected.calibration_revision)
        notice = "The saved map belongs to an older calibration. New marks use this calibration.";
      else
        selected = *saved;
    }
  } else {
    require(
        request.payload.value("snapshot").toObject() == identity(snapshot),
        "Rink-map snapshot changed; reopen before editing");
    require(request.payload.value("document").isObject(), "Missing rink-map draft");
    selected = draft(request.payload.value("document").toObject(), snapshot);
  }
  const bool detect = request.payload.contains("detect") && boolean(request.payload, "detect");
  const bool use_mask = request.payload.contains("use_mask") && boolean(request.payload, "use_mask");
  require(request.operation != "rink-map-save" || (!detect && !use_mask), "Review assistance before saving the map");
  RinkMaskPreparation prepare_mask;
  if (use_mask) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("HM_GAME_DIR", text(request.game.parent_path().string()));
    prepare_mask = RinkMaskPreparer(
        RequiredString(request.runtime, "cli", 4096),
        RequiredString(request.runtime, "root", 4096),
        RequiredString(request.runtime, "config", 4096),
        text(request.game.string()),
        environment);
  }
  auto updated = take(UpdateRinkMap(
      snapshot, std::move(selected), detect, cancelled, use_mask, prepare_mask, [](const QString& message) {
        std::cout << "Rink mask: " << message.toStdString() << std::endl;
      }));
  const bool saved = request.operation == "rink-map-save";
  if (saved) {
    require(!cancelled || !cancelled(), "Rink map save cancelled");
    const auto status = SaveRinkMap(snapshot, updated.document);
    if (!status.ok())
      throw std::runtime_error(status.ToString());
  }
  QJsonArray features, images, sizes, projected, detections, intersections, review;
  for (const auto& feature : rink::RinkMapFeatures())
    features.append(QJsonObject{{"id", feature.id}, {"name", feature.name}, {"line", feature.line}});
  for (int camera = 0; camera < 2; ++camera) {
    const auto path = request.game / (camera ? "right.png" : "left.png");
    artifacts.append(Artifact(request, path, "image"));
    images.append(text(Relative(request, path)));
    sizes.append(QJsonArray{snapshot.original_sizes[camera].width(), snapshot.original_sizes[camera].height()});
    projected.append(drawing(updated.projected_layout[camera]));
    QJsonArray found;
    for (const auto& item : updated.detections[camera])
      found.append(
          QJsonObject{
              {"class_index", item.class_index},
              {"label", text(item.label)},
              {"score", item.score},
              {"outline", points(item.outline)},
              {"line_samples", points(item.line_samples)}});
    detections.append(found);
  }
  for (const auto& item : updated.mask_intersections)
    intersections.append(observation(item));
  for (const auto index : updated.fit.review_observations)
    review.append(int(index));
  const char* state = updated.fit.state == rink::RinkMapState::kReady ? "ready"
      : updated.fit.state == rink::RinkMapState::kInconsistent        ? "inconsistent"
                                                                      : "partial";
  bool playback_verified = false;
  QString playback_error;
  if (snapshot.saved_map && *snapshot.saved_map == take(rink::SerializeRinkMap(updated.document)) &&
      updated.fit.state == rink::RinkMapState::kReady) {
    const auto playback = rink::LoadPlaybackRinkMap(request.game, cancelled);
    playback_verified = playback.ok();
    if (!playback.ok())
      playback_error = text(playback.status().ToString());
  }
  return {
      {"rink_map_version", 1},
      {"snapshot", identity(snapshot)},
      {"document", document(updated.document)},
      {"fit",
       QJsonObject{
           {"state", state},
           {"message", text(updated.fit.message)},
           {"rms_degrees", updated.fit.rms_degrees},
           {"constraints", int(updated.fit.constraints)},
           {"review_observations", review}}},
      {"images", images},
      {"image_sizes", sizes},
      {"features", features},
      {"projected_layout", projected},
      {"detections", detections},
      {"mask_intersections", intersections},
      {"assistance_message", text(updated.assistance_message)},
      {"layout_drawing", drawing(rink::RinkLayoutDrawing(updated.document.layout))},
      {"load_notice", notice},
      {"limits", QJsonObject{{"observations", 128}, {"samples", 64}, {"undo", 32}}},
      {"saved", saved},
      {"calibration_complete", true},
      {"playback_verified", playback_verified},
      {"playback_error", playback_error}};
}
} // namespace hm::web
