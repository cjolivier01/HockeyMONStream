#include "src/apps/hstream-ui/WebSavedCalibration.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtGui/QImageReader>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "hstream/src/libs/stitching/StitchingReframe.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/CalibrationImage.h"
#include "src/apps/hstream-ui/ProjectionCropDialog.h"
#include "src/apps/hstream-ui/RinkMapBackend.h"
#include "src/apps/hstream-ui/WebRinkEditor.h"

namespace hm::web {
namespace {
namespace rink = hm::stitching;
namespace fs = std::filesystem;
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void check(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <typename T>
T take(absl::StatusOr<T> value) {
  check(value.status());
  return std::move(*value);
}
void merge(YAML::Node target, const YAML::Node& source) {
  for (const auto& entry : source) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && target[key].IsMap())
      merge(target[key], entry.second);
    else
      target[key] = YAML::Clone(entry.second);
  }
}
QJsonArray values(const std::array<double, 3>& array) {
  return {array[0], array[1], array[2]};
}
QJsonArray values(const std::array<double, 4>& array) {
  return {array[0], array[1], array[2], array[3]};
}
template <size_t N>
std::array<double, N> array(const QJsonObject& object, const char* key, double low, double high) {
  const auto input = object.value(key).toArray();
  require(input.size() == N, "Incomplete calibration view coordinates");
  std::array<double, N> result;
  for (size_t index = 0; index < N; ++index) {
    const auto value = input[int(index)];
    require(
        value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low && value.toDouble() <= high,
        "Calibration view coordinate is outside its range");
    result[index] = value.toDouble();
  }
  return result;
}
QByteArray tool(
    const QString& name,
    const QStringList& arguments,
    const fs::path& directory,
    const std::function<bool()>& cancelled,
    const QByteArray& input = {}) {
  QProcess process;
  process.setWorkingDirectory(text(directory.string()));
  process.start(name, arguments);
  require(process.waitForStarted(5000), "Could not start native calibration tool");
  process.write(input);
  process.closeWriteChannel();
  QElapsedTimer timer;
  timer.start();
  QByteArray output, errors;
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(50);
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if ((cancelled && cancelled()) || timer.elapsed() > 120000 || output.size() + errors.size() > 4 * 1024 * 1024) {
      process.kill();
      process.waitForFinished(3000);
      throw std::runtime_error("Calibration tool cancelled or exceeded its limits");
    }
  }
  output += process.readAllStandardOutput();
  errors += process.readAllStandardError();
  require(
      process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
      "Native calibration tool failed: " + errors.right(2000).toStdString());
  return output;
}
struct Snapshot {
  YAML::Node config, effective;
  rink::StitchProjectionFraming framing;
  rink::StitchProjection projection;
  std::vector<double> parameters;
  std::array<double, 3> published;
  std::string project;
  QJsonObject identity;
};
Snapshot snapshot_locked(const Request& request) {
  Snapshot result;
  result.config = YAML::Load(take(
      rink::read_bounded_regular_file_no_follow(request.game / "config.yaml", 4 * 1024 * 1024, "calibration config")));
  const auto calibration = result.config["hstream_ui"]["stitching_calibration"];
  require(
      calibration.IsMap() && calibration["status"].as<std::string>("") == "complete" &&
          !rink::HasStitchingReframeIntent(result.config),
      "Finish calibration before editing its saved view");
  result.effective = YAML::Clone(take(hm::baseline_config::load()).values);
  merge(result.effective, result.config);
  rink::PreserveLegacyInputLensSelection(result.effective, result.config);
  result.framing = take(rink::read_stitch_projection_framing(result.effective));
  const auto projection_name = result.effective["stitching"]["projection"].as<std::string>();
  result.projection = take(rink::ParseStitchProjection(projection_name));
  result.parameters = take(rink::read_stitch_projection_parameters(result.effective, result.projection));
  const auto camera = take(rink::read_stitch_camera_selection(result.effective));
  const auto error = ProjectionCropDialog::savedCalibrationPreviewError(
      text(request.game.string()), result.framing, text(projection_name), result.parameters, camera);
  require(error.isEmpty(), error.toStdString());
  const auto provenance = take(rink::HuginProject::ReadCanvasProvenanceLocked(request.game));
  require(provenance && provenance->projection_framing, "Saved view has no published rotation");
  result.published = provenance->projection_framing->rotation_degrees;
  result.project = take(rink::HuginProject::ReadProject(request.game / "autooptimiser_out.pto"));
  const auto generation = take(rink::stitch_artifact_preflight_generation_id_locked(request.game));
  result.identity = {
      {"generation",
       QString::fromLatin1(
           QCryptographicHash::hash(QByteArray::fromStdString(generation), QCryptographicHash::Sha256).toHex())},
      {"revision", text(take(rink::RinkMapCalibrationRevision(request.game)))}};
  return result;
}
Snapshot snapshot(const Request& request) {
  auto lock = take(rink::try_lock_canvas_constraint_artifacts(request.game));
  require(bool(lock), "Calibration is changing; reopen the saved view");
  auto config_lock = take(rink::GameConfigTransactionLock::Acquire(request.game));
  return snapshot_locked(request);
}
QJsonObject render(
    const Request& request,
    const Snapshot& source,
    const rink::StitchProjectionFraming& selected,
    const QString& kind,
    const fs::path& work,
    bool blend,
    QJsonArray& artifacts,
    const std::function<bool()>& cancelled) {
  const auto localized = take(rink::LocalizeCalibrationPreviewImages(source.project, request.game));
  std::ofstream(work / "source.pto") << localized;
  for (const char* name : {"left.png", "right.png"})
    fs::copy_file(request.game / name, work / name);
  QStringList framing;
  if (kind == "leveling") {
    const auto delta = take(rink::RinkLevelingRotationDelta(source.published, selected.rotation_degrees));
    framing
        << QString("--rotate=%1,%2,%3").arg(delta[0], 0, 'g', 17).arg(delta[1], 0, 'g', 17).arg(delta[2], 0, 'g', 17);
  } else
    framing << "--crop=0,100,0,100%";
  framing << "--output=framed.pto" << "source.pto";
  tool("pano_modify", framing, work, cancelled);
  const auto size =
      take(rink::HuginProject::ParseCanvasSize(take(rink::HuginProject::ReadProject(work / "framed.pto"))));
  require(
      size.first && size.second && size.first <= 65536 && size.second <= size.first * 4ULL,
      "Preview canvas exceeds its limit");
  const double scale = std::min(1.0, 1600.0 / (kind == "crop" ? std::max(size.first, size.second) : size.first));
  const int width = std::max(1, int(std::lround(size.first * scale))),
            height = std::max(1, int(std::lround(size.second * scale)));
  tool(
      "pano_modify",
      {QString("--canvas=%1x%2").arg(width).arg(height), "--output=preview.pto", "framed.pto"},
      work,
      cancelled);
  tool(
      "nona",
      {"-v",
       "-m",
       "PNG",
       "--ignore-exposure",
       blend ? "--seam=blend" : "--seam=hard",
       "-o",
       "preview.png",
       "preview.pto"},
      work,
      cancelled);
  QImageReader reader(text((work / "preview.png").string()), "png");
  require(reader.size().width() > 0 && reader.size().height() > 0, "Native preview image is unavailable");
  const auto path = request.root / "view-preview.png";
  fs::copy_file(work / "preview.png", path, fs::copy_options::overwrite_existing);
  artifacts.append(Artifact(request, path, "image"));
  QJsonObject result{{"preview", text(Relative(request, path))}, {"blend", blend}};
  if (kind == "crop") {
    try {
      tool("pano_modify", {"--crop=AUTO", "--output=auto.pto", "preview.pto"}, work, cancelled);
      const auto automatic = text(take(rink::HuginProject::ReadProject(work / "auto.pto")));
      const auto found = QRegularExpression("(?m)^p .*?\\bS(\\d+),(\\d+),(\\d+),(\\d+)").match(automatic);
      if (found.hasMatch()) {
        QJsonArray crop;
        for (int index = 0; index < 4; ++index)
          crop.append(found.captured(index + 1).toDouble() / (index < 2 ? width : height));
        result.insert("auto_crop", crop);
      }
    } catch (const std::exception&) {
      require(!cancelled || !cancelled(), "Crop preview cancelled");
    }
  }
  return result;
}
} // namespace

QString ExportSavedCalibrationPreview(const Request& request, QJsonArray& artifacts) {
  const auto source = Contained(request.root, text(Relative(request, request.game / "panorama.tif")));
  const auto preview = readCalibrationTiff(text(source.string()), 1920);
  require(!preview.isNull(), "Saved calibration did not produce a readable panorama");
  const auto path = Contained(
      request.root,
      text((request.game / "setup-preview.png").lexically_relative(request.root).generic_string()),
      false);
  require(
      !fs::is_symlink(fs::symlink_status(request.game / "setup-preview.png")),
      "Saved preview must not be a symbolic link");
  QSaveFile output(text(path.string()));
  require(
      output.open(QIODevice::WriteOnly) && preview.save(&output, "PNG") && output.commit(),
      "Cannot publish the saved panorama preview");
  artifacts.append(Artifact(request, path, "image"));
  return text(Relative(request, path));
}

QJsonObject RunSavedCalibration(const Request& request, QJsonArray& artifacts, const std::function<bool()>& cancelled) {
  if (request.operation == "calibration-view-rotation-stage") {
    require(
        request.payload.size() == 2 && request.payload.contains("source_generation") &&
            request.payload.contains("post_stitch_rotate_degrees"),
        "Saved rotation staging requires an exact source and rotation");
    const auto source = RequiredString(request.payload, "source_generation", 1024 * 1024);
    const auto rotation = request.payload.value("post_stitch_rotate_degrees");
    require(rotation.isDouble() && std::isfinite(rotation.toDouble()), "Saved rotation must be finite");
    require(!cancelled || !cancelled(), "Saved rotation staging cancelled");
    const auto staged = take(
        rink::stage_saved_stitched_output_rotation(request.game.string(), source.toStdString(), rotation.toDouble()));
    return {
        {"source_generation", text(staged.source_generation)},
        {"desired_generation", text(staged.desired_generation)},
        {"hugin_generation", text(staged.hugin_generation)},
        {"changed", staged.changed}};
  }
  auto source = snapshot(request);
  const auto kind = RequiredString(request.payload, "kind", 16);
  require(kind == "leveling" || kind == "crop", "Unknown saved calibration editor");
  const bool reading = request.operation == "calibration-view-read";
  if (!reading)
    require(
        request.payload.value("snapshot").toObject() == source.identity,
        "Saved calibration changed; reopen the editor");
  auto selected = source.framing;
  if (!reading && kind == "leveling") {
    selected.rotation_degrees = array<3>(request.payload, "angles", -180, 180);
    selected.rotation_inherited = false;
  }
  if (!reading && kind == "crop") {
    selected.crop = array<4>(request.payload, "crop", 0, 1);
    require(request.payload.value("automatic").isBool(), "Expected automatic crop selection");
    selected.auto_crop = request.payload.value("automatic").toBool();
    require(
        selected.crop[0] < selected.crop[1] && selected.crop[2] < selected.crop[3] &&
            (!selected.auto_crop || selected.crop == std::array<double, 4>{{0, 1, 0, 1}}),
        "Invalid saved crop selection");
  }
  check(rink::ValidateStitchProjectionFraming(source.projection, source.parameters, selected));
  fs::create_directories(request.root / "work");
  QTemporaryDir temporary(text((request.root / "work" / "view-XXXXXX").string()));
  require(temporary.isValid(), "Cannot create private saved-view snapshot");
  const fs::path work(temporary.path().toStdString());
  auto prepared = RinkEditor({{"pto", text(source.project)}, {"sourceDirectory", text(request.game.string())}});
  QJsonArray images;
  for (int camera = 0; camera < 2; ++camera) {
    const auto path = request.game / (camera ? "right.png" : "left.png");
    QImageReader reader(text(path.string()), "png");
    const auto size = prepared.value("imageSizes").toArray()[camera].toArray();
    require(
        reader.size() == QSize(size[0].toInt(), size[1].toInt()),
        "Original image dimensions differ from saved camera geometry");
    images.append(text(Relative(request, path)));
  }
  QJsonObject result{
      {"calibration_view_version", 1},
      {"kind", kind},
      {"snapshot", source.identity},
      {"images", images},
      {"image_sizes", prepared.value("imageSizes")},
      {"angles", values(selected.rotation_degrees)},
      {"crop", values(selected.crop)},
      {"automatic", selected.auto_crop},
      {"saved_landmarks_available", fs::is_regular_file(request.game / "rink-map.yaml")}};
  if (request.operation == "calibration-view-estimate") {
    require(kind == "leveling", "Only leveling estimates original-image points");
    auto value = request.payload;
    value.insert("pto", text(source.project));
    value.insert("sourceDirectory", text(request.game.string()));
    value.insert("publishedAngles", values(source.published));
    prepared = RinkEditor(value);
    std::ofstream(work / "sphere.pto") << prepared.value("sphere").toString().toStdString();
    value.insert(
        "rays",
        QString::fromUtf8(
            tool("pano_trafo", {"sphere.pto"}, work, cancelled, prepared.value("input").toString().toUtf8())));
    const auto estimate = RinkEditor(value);
    for (auto item = estimate.begin(); item != estimate.end(); ++item)
      result.insert(item.key(), item.value());
    if (estimate.contains("rectangleMismatch"))
      result.insert("rectangle_mismatch", estimate.value("rectangleMismatch"));
  } else if (request.operation == "calibration-view-landmarks") {
    require(kind == "leveling", "Saved landmarks estimate leveling only");
    const auto map = take(LoadRinkMapSnapshot(text(request.game.string())));
    require(map.saved_map.has_value(), "Save a partial or reviewed rink map first");
    const auto marks = take(rink::ParseRinkMap(*map.saved_map));
    require(
        marks.generation == map.generation && marks.calibration_revision == map.revision.toStdString(),
        "Saved landmarks belong to another calibration");
    const auto updated = take(UpdateRinkMap(map, marks, false, cancelled));
    QJsonArray reference_marks;
    for (int camera = 0; camera < 2; ++camera) {
      QJsonArray marks;
      for (const auto& observation : updated.document.observations) {
        if (!observation.enabled || observation.camera != camera)
          continue;
        QJsonArray points;
        for (const auto& point : observation.pixels)
          points.append(QJsonArray{point.x, point.y});
        marks.append(QJsonObject{{"pixels", points}, {"arc", observation.arc}});
      }
      reference_marks.append(marks);
    }
    result.insert("reference_marks", reference_marks);
    const auto estimate = take(RinkLandmarkLeveling(updated, source.published, selected.rotation_degrees[0]));
    result.insert("angles", values(estimate.rotation_degrees));
    result.insert("rectangle_mismatch", estimate.orthogonality_error_degrees);
    result.insert("message", "Reused saved rink landmarks. Inspect the preview before applying angles.");
  } else if (request.operation == "calibration-view-save") {
    if (kind == "leveling")
      require(request.payload.value("preview_verified").toBool(false), "Preview the selected angles before saving");
    bool reframe = false;
    const auto owner = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    size_t width = 0;
    {
      auto lock = take(rink::lock_canvas_constraint_artifacts(request.game));
      auto config_lock = take(rink::GameConfigTransactionLock::Acquire(request.game));
      const auto current = snapshot_locked(request);
      require(current.identity == source.identity, "Saved calibration changed while selecting its view");
      auto desired = YAML::Clone(current.config);
      rink::write_stitch_projection_framing(desired, selected);
      auto effective = YAML::Clone(current.effective);
      rink::write_stitch_projection_framing(effective, selected);
      reframe =
          take(rink::PrepareStitchingReframeIntentLocked(request.game, current.effective, effective, owner, desired));
      if (reframe) {
        auto calibration = desired["hstream_ui"]["stitching_calibration"];
        calibration["status"] = "pending";
        calibration["rink_mask_status"] = "pending";
        calibration["stale_from"] = "canvas";
        calibration["artifacts_invalidated"] = true;
        calibration["invalidation_id"] = owner;
        calibration.remove("backend_generation");
        width = take(rink::ValidateStitchingReframeIntentLocked(request.game, desired)).desired_max_output_width;
      }
      check(rink::publish_game_config(request.game, YAML::Dump(desired)));
    }
    if (reframe)
      check(rink::reframe_stitching(request.game.string(), owner, cancelled, width));
    source = snapshot(request);
    result.insert("snapshot", source.identity);
    result.insert("saved", true);
    result.insert("calibration_complete", true);
    result.insert("alignment_reused", reframe);
    result.insert("setup_preview", ExportSavedCalibrationPreview(request, artifacts));
  }
  if (request.operation == "calibration-view-preview" || request.operation == "calibration-view-save" ||
      (reading && kind == "crop")) {
    const bool blend = request.payload.value("blend").toBool(false);
    const auto preview = render(request, source, selected, kind, work, blend, artifacts, cancelled);
    for (auto item = preview.begin(); item != preview.end(); ++item)
      result.insert(item.key(), item.value());
  }
  require(!cancelled || !cancelled(), "Saved view cancelled");
  require(snapshot(request).identity == source.identity, "Saved calibration changed during editing");
  // Reframing republishes even unchanged original stills; capture their final file identities.
  for (const char* name : {"left.png", "right.png"})
    artifacts.append(Artifact(request, request.game / name, "image"));
  return result;
}
} // namespace hm::web
