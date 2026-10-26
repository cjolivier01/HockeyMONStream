#include "src/apps/hstream-ui/StitchingStillImage.h"

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QImageReader>

#include <sys/stat.h>
#include <filesystem>

QByteArray StitchingStillRevision(const QString& directory) {
  QByteArray result;
  for (const char* name :
       {"config.yaml",
        "stitching_generation_id",
        "stitching_canvas_provenance",
        "s.png",
        "panorama.tif",
        "autooptimiser_out.pto",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif",
        "seam_file.png"}) {
    struct stat info{};
    const QByteArray path = QFile::encodeName(QDir(directory).filePath(name));
    result += name;
    if (::lstat(path.constData(), &info) == 0) {
      for (qint64 value :
           {qint64(info.st_dev),
            qint64(info.st_ino),
            qint64(info.st_size),
            qint64(info.st_mtim.tv_sec),
            qint64(info.st_mtim.tv_nsec),
            qint64(info.st_ctim.tv_sec),
            qint64(info.st_ctim.tv_nsec)})
        result += ':' + QByteArray::number(value);
    }
    result += '\n';
  }
  return result;
}

// Modern calibrations retain an immutable parameter claim. Validate both the
// worker-visible file and the UI's frozen effective choices; merely reloading
// changed settings must not make an old image appear current.
namespace {
void merge(YAML::Node target, const YAML::Node& source) {
  for (const auto& entry : source) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && target[key].IsMap())
      merge(target[key], entry.second);
    else
      target[key] = YAML::Clone(entry.second);
  }
}
bool settings_match(const YAML::Node& config, const QByteArray& expected_settings) {
  const auto calibration = config["hstream_ui"]["stitching_calibration"];
  if (!calibration["backend_generation"])
    return true; // Legacy completed calibrations have no parameter claim.
  const auto owner = calibration["invalidation_id"].as<std::string>("");
  if (owner.empty())
    return false;
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok())
    return false;
  YAML::Node effective = YAML::Clone(baseline->values);
  if (expected_settings.isEmpty() && !qEnvironmentVariableIsEmpty("HOME")) {
    // Without HOME there is no user layer; the explicit game and required
    // baseline remain sufficient for an advisory read. Never create a home.
    // Completion/export has no UI draft. Its supervisor freezes the user layer
    // in the private HOME. Read it without creating legacy preferences or dirs.
    const auto path = hm::user_config::active_file_path();
    if (!path.ok())
      return false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(*path, error);
    if (error && error != std::errc::no_such_file_or_directory)
      return false;
    if (!error && status.type() != std::filesystem::file_type::not_found) {
      const auto bytes =
          hm::stitching::read_bounded_regular_file_no_follow(*path, 4 * 1024 * 1024, "frozen preview defaults");
      if (!bytes.ok())
        return false;
      const auto user = YAML::Load(*bytes);
      if (user && !user.IsNull()) {
        if (!user.IsMap())
          return false;
        merge(effective, user);
        hm::stitching::PreserveLegacyInputLensSelection(effective, user);
      }
    }
  }
  merge(effective, config);
  hm::stitching::PreserveLegacyInputLensSelection(effective, config);
  if (!expected_settings.isEmpty()) {
    const YAML::Node selected = YAML::Load(expected_settings.toStdString());
    for (const auto& entry : selected["stitching"])
      effective["stitching"][entry.first.as<std::string>()] = YAML::Clone(entry.second);
  }
  const auto settings = effective["stitching"];
  hm::stitching::StitchingBackendChoices choices;
  choices.control_point_matcher = settings["control_point_matcher"].as<std::string>();
  choices.mapping_backend = settings["mapping_backend"].as<std::string>();
  choices.projection = settings["projection"].as<std::string>();
  choices.run_autooptimizer = settings["run_autooptimizer"].as<bool>();
  const auto projection = hm::stitching::ParseStitchProjection(choices.projection);
  if (!projection.ok())
    return false;
  const auto parameters = hm::stitching::read_stitch_projection_parameters(effective, *projection);
  const auto framing = hm::stitching::read_stitch_projection_framing(effective);
  const auto camera = hm::stitching::read_stitch_camera_selection(effective);
  const auto resolution = hm::stitching::read_control_point_resolution(effective);
  const auto provider = hm::stitching::read_control_point_execution_provider(effective);
  const auto selection = hm::stitching::player_frame_selection_fingerprint(effective);
  const auto manual = hm::stitching::manual_control_point_fingerprint(effective);
  if (!parameters.ok() || !framing.ok() || !camera.ok() || !resolution.ok() || !provider.ok() || !selection.ok() ||
      !manual.ok())
    return false;
  choices.projection_parameters = *parameters;
  choices.projection_framing = *framing;
  choices.camera = *camera;
  choices.control_point_resolution = *resolution;
  const auto filtering = hm::stitching::read_control_point_filtering(effective);
  if (!filtering.ok())
    return false;
  choices.control_point_filtering = *filtering;
  choices.control_point_execution_provider = *provider;
  choices.calibration_frame_selection_fingerprint = *selection;
  choices.manual_control_point_fingerprint = *manual;
  return hm::stitching::validate_stitching_backend_generation(config, owner, choices).ok();
}

} // namespace

SavedStitchingStill ReadSavedStitchingStill(
    const QString& directory,
    const QString& expected_generation,
    const QByteArray& expected_settings,
    const QByteArray& expected_revision,
    std::mutex& metadata_mutex,
    const std::function<bool()>& cancelled,
    const std::function<void()>& metadata_locked,
    const std::function<void()>& decoding) {
  SavedStitchingStill result;
  result.revision = expected_revision;
  bool png_current = false;
  QFile png(QDir(directory).filePath("s.png"));
  QFile tiff(QDir(directory).filePath("panorama.tif"));
  {
    // Cancellation closes admission before draining this section. Every lock
    // acquired below is nonblocking and is released before metadata_mutex,
    // including early returns. Decoding must stay outside this gate.
    std::lock_guard<std::mutex> metadata_guard(metadata_mutex);
    if (cancelled && cancelled())
      return result;
    const auto path = directory.toStdString();
    auto artifacts = hm::stitching::try_lock_canvas_constraint_snapshot(path);
    if (!artifacts.ok() || !*artifacts) {
      result.retry = true;
      return result;
    }
    if (metadata_locked)
      metadata_locked();
    const auto text = hm::stitching::read_bounded_regular_file_no_follow(
        std::filesystem::path(path) / "config.yaml", 4 * 1024 * 1024, "calibration preview settings");
    if (!text.ok())
      return result;
    try {
      const YAML::Node config = YAML::Load(*text);
      const auto ui = config["hstream_ui"];
      const auto calibration = ui && ui.IsMap() ? ui["stitching_calibration"] : YAML::Node();
      if (!calibration.IsMap() || calibration["status"].as<std::string>("") != "complete" ||
          !calibration["stale_from"].as<std::string>("").empty())
        return result;
      // Reframing can complete the maps while the downstream rink snapshot is
      // still invalidated. Its new panorama is usable; the old PNG is not.
      png_current = !calibration["artifacts_invalidated"].as<bool>(false);
      if (!settings_match(config, expected_settings))
        return result;
    } catch (const YAML::Exception&) {
      return result;
    }
    if (!expected_generation.isEmpty()) {
      const auto generation = hm::stitching::stitch_artifact_snapshot_generation_id_locked(path);
      if (!generation.ok() || *generation != expected_generation.toStdString())
        return result;
    }
    if (StitchingStillRevision(directory) != expected_revision) {
      result.retry = true;
      return result;
    }
    for (QFile* file : {&png, &tiff}) {
      struct stat info{};
      const auto encoded = QFile::encodeName(file->fileName());
      if ((file != &png || png_current) && ::lstat(encoded.constData(), &info) == 0 && S_ISREG(info.st_mode) &&
          info.st_size > 0 && info.st_size <= 2LL * 1024 * 1024 * 1024)
        file->open(QIODevice::ReadOnly);
    }
  }
  // Keep the opened inodes across atomic publication, but release publication
  // locks before decoding so Play and settings saves never wait for an image.
  if (cancelled && cancelled())
    return result;
  if (decoding)
    decoding();
  constexpr int maximum_dimension = 2048;
  if (png.isOpen()) {
    const QString path = QString("/proc/self/fd/%1").arg(png.handle());
    QImageReader reader(path, "png");
    QSize size = reader.size();
    if (size.isValid() && !size.isEmpty()) {
      if (size.width() > maximum_dimension || size.height() > maximum_dimension)
        size.scale(maximum_dimension, maximum_dimension, Qt::KeepAspectRatio);
      result.image = readCalibrationPng(path, size);
      result.name = "s.png";
    }
  }
  if (result.image.isNull() && tiff.isOpen()) {
    result.image = readCalibrationTiff(QString("/proc/self/fd/%1").arg(tiff.handle()), maximum_dimension);
    result.name = "panorama.tif";
  }
  // Both frontends reject a source replaced while the descriptor was decoded.
  // Keep publication locks out of the decode and the UI's cancellation path.
  const bool stopped = cancelled && cancelled();
  if (stopped || StitchingStillRevision(directory) != expected_revision) {
    result.image = {};
    result.name.clear();
    result.retry = !stopped;
  }
  return result;
}
