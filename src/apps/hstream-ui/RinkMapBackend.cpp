#include "src/apps/hstream-ui/RinkMapBackend.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

#include <opencv2/imgproc.hpp>
#include <yaml-cpp/yaml.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtGui/QImageReader>

#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

namespace {
namespace rink = hm::stitching;
constexpr double kPi = 3.14159265358979323846;
std::string GenerationDigest(const std::string& generation) {
  return QCryptographicHash::hash(QByteArray::fromStdString(generation), QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}

absl::Status CalibrationComplete(const QString& directory) {
  auto text = rink::read_bounded_regular_file_no_follow(
      std::filesystem::path(directory.toStdString()) / "config.yaml", 4 * 1024 * 1024, "rink-map settings");
  if (!text.ok())
    return text.status();
  try {
    const YAML::Node config = YAML::Load(*text);
    const auto calibration = config["hstream_ui"]["stitching_calibration"];
    if (!calibration.IsMap() || calibration["status"].as<std::string>("") != "complete" ||
        !calibration["stale_from"].as<std::string>("").empty())
      return absl::FailedPreconditionError("Complete stitching calibration before mapping the rink");
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
  return absl::OkStatus();
}

absl::StatusOr<QByteArray> CalibrationRevision(const QString& directory) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // Unrelated game preferences do not invalidate this offline geometry.
  for (const char* name : {"autooptimiser_out.pto", "stitching_canvas_provenance", "left.png", "right.png"}) {
    auto text = rink::read_bounded_regular_file_no_follow(
        std::filesystem::path(directory.toStdString()) / name, 128 * 1024 * 1024, "rink-map calibration");
    if (!text.ok())
      return text.status();
    hash.addData(name);
    hash.addData(QByteArray::number(text->size()));
    hash.addData(QByteArrayView(text->data(), text->size()));
  }
  return hash.result().toHex();
}

absl::StatusOr<std::optional<std::string>> ReadSavedMap(const QString& directory) {
  const auto path = std::filesystem::path(directory.toStdString()) / "rink-map.yaml";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (status.type() == std::filesystem::file_type::not_found &&
      (!error || error == std::errc::no_such_file_or_directory))
    return std::nullopt;
  auto text = rink::read_bounded_regular_file_no_follow(path, 512 * 1024, "saved rink map");
  if (!text.ok())
    return text.status();
  return std::optional<std::string>(*text);
}

absl::StatusOr<QByteArray> Transform(
    const RinkMapSnapshot& snapshot,
    const QByteArray& input,
    bool reverse,
    const std::function<bool()>& cancelled) {
  if (cancelled && cancelled())
    return absl::CancelledError("Rink mapping stopped");
  QProcess process;
  QStringList arguments;
  if (reverse)
    arguments << "-r";
  arguments << snapshot.temporary->filePath("sphere.pto");
  process.start("pano_trafo", arguments);
  if (!process.waitForStarted(5000))
    return absl::NotFoundError("Could not start pano_trafo. Install the Hugin tools.");
  process.write(input);
  process.closeWriteChannel();
  QElapsedTimer timer;
  timer.start();
  QByteArray output, errors;
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(100);
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if ((cancelled && cancelled()) || timer.elapsed() > 30000 || output.size() > 2 * 1024 * 1024 ||
        errors.size() > 65536) {
      process.kill();
      process.waitForFinished(3000);
      return cancelled && cancelled() ? absl::CancelledError("Rink mapping stopped")
                                      : absl::ResourceExhaustedError("Rink coordinate transform exceeded its limits");
    }
  }
  output += process.readAllStandardOutput();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return absl::InternalError("Rink coordinate transform failed: " + errors.toStdString());
  return output;
}

absl::StatusOr<std::vector<cv::Point2d>> ReadPairs(const QByteArray& output, size_t count, bool allow_outside) {
  std::istringstream stream(output.toStdString());
  stream.imbue(std::locale::classic());
  std::vector<cv::Point2d> result;
  for (size_t i = 0; i < count; ++i) {
    std::string x, y;
    if (!(stream >> x >> y))
      return absl::DataLossError("Rink coordinate transform returned too few points");
    char* end_x = nullptr;
    char* end_y = nullptr;
    const double px = std::strtod(x.c_str(), &end_x), py = std::strtod(y.c_str(), &end_y);
    if (*end_x || *end_y || (!allow_outside && (!std::isfinite(px) || !std::isfinite(py))))
      return absl::DataLossError("A rink point could not be transformed");
    result.emplace_back(px, py);
  }
  stream >> std::ws;
  if (!stream.eof())
    return absl::DataLossError("Rink coordinate transform returned extra points");
  return result;
}

absl::Status ResolveRays(
    const RinkMapSnapshot& snapshot,
    rink::RinkMapDocument& document,
    const std::function<bool()>& cancelled) {
  std::ostringstream input;
  input.imbue(std::locale::classic());
  input << std::setprecision(17);
  size_t count = 0;
  if (document.observations.size() > 128)
    return absl::ResourceExhaustedError("Keep at most 128 rink observations");
  for (const auto& observation : document.observations) {
    if (observation.camera < 0 || observation.camera > 1 || observation.pixels.size() > 64)
      return absl::InvalidArgumentError("Invalid rink observation");
    const auto size = snapshot.original_sizes[observation.camera];
    for (const auto point : observation.pixels) {
      if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 || point.x >= size.width() ||
          point.y >= size.height())
        return absl::InvalidArgumentError("Keep rink marks inside the camera image");
      input << observation.camera << ' ' << point.x << ' ' << point.y << '\n';
      ++count;
    }
  }
  if (!count)
    return absl::OkStatus();
  auto transformed = Transform(snapshot, QByteArray::fromStdString(input.str()), false, cancelled);
  if (!transformed.ok())
    return transformed.status();
  auto pairs = ReadPairs(*transformed, count, false);
  if (!pairs.ok())
    return pairs.status();
  size_t index = 0;
  for (auto& observation : document.observations) {
    observation.rays.clear();
    for (size_t j = 0; j < observation.pixels.size(); ++j) {
      const auto point = pairs->at(index++);
      if (point.x < -0.50001 || point.x > 3599.50001 || point.y < -0.50001 || point.y > 1799.50001)
        return absl::InvalidArgumentError("A rink mark lies outside the calibrated camera geometry");
      const double longitude = (point.x - 1799.5) * kPi / 1800;
      const double latitude = (899.5 - point.y) * kPi / 1800;
      observation.rays.emplace_back(
          std::cos(latitude) * std::cos(longitude), -std::cos(latitude) * std::sin(longitude), std::sin(latitude));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::array<std::vector<std::vector<cv::Point2d>>, 2>> ProjectLayout(
    const RinkMapSnapshot& snapshot,
    const rink::RinkMapDocument& document,
    const rink::RinkMapFit& fit,
    const std::function<bool()>& cancelled) {
  std::array<std::vector<std::vector<cv::Point2d>>, 2> result;
  if (fit.state != rink::RinkMapState::kReady)
    return result;
  auto layout = rink::RinkLayoutDrawing(document.layout);
  const double goal = document.layout.length / 2 - document.layout.goal_inset;
  // Crosses mark goal-mouth centres on ice; board ends are separate geometry.
  for (double x : {-goal, goal}) {
    layout.push_back({{x - 0.6, 0}, {x + 0.6, 0}});
    layout.push_back({{x, -0.6}, {x, 0.6}});
  }
  std::ostringstream input;
  input.imbue(std::locale::classic());
  input << std::setprecision(17);
  std::vector<size_t> lengths;
  size_t count = 0;
  for (const auto& line : layout) {
    const size_t length = (line.size() - 1) * 12 + 1;
    lengths.push_back(length);
    count += length;
  }
  for (int camera = 0; camera < 2; ++camera) {
    for (const auto& line : layout) {
      for (size_t segment = 0; segment + 1 < line.size(); ++segment) {
        for (int step = 0; step < (segment + 2 == line.size() ? 13 : 12); ++step) {
          const double fraction = step / 12.0;
          const auto ray = *rink::RinkPointToRay(fit, line[segment] * (1 - fraction) + line[segment + 1] * fraction);
          input << camera << ' ' << 1799.5 - std::atan2(ray[1], ray[0]) * 1800 / kPi << ' '
                << 899.5 - std::asin(std::clamp(ray[2], -1.0, 1.0)) * 1800 / kPi << '\n';
        }
      }
    }
  }
  auto transformed = Transform(snapshot, QByteArray::fromStdString(input.str()), true, cancelled);
  if (!transformed.ok())
    return transformed.status();
  auto pairs = ReadPairs(*transformed, count * 2, true);
  if (!pairs.ok())
    return pairs.status();
  size_t offset = 0;
  for (auto& camera : result)
    for (const auto length : lengths) {
      camera.emplace_back(pairs->begin() + offset, pairs->begin() + offset + length);
      offset += length;
    }
  return result;
}
} // namespace

absl::StatusOr<RinkMapSnapshot> LoadRinkMapSnapshot(const QString& game_directory) {
  RinkMapSnapshot result;
  result.game_directory = game_directory;
  result.temporary = std::make_shared<QTemporaryDir>();
  if (!result.temporary->isValid())
    return absl::InternalError("Could not create a rink-map snapshot");
  std::array<std::unique_ptr<QFile>, 2> files;
  {
    auto lock = rink::try_lock_canvas_constraint_artifacts(game_directory.toStdString());
    if (!lock.ok())
      return lock.status();
    if (!*lock)
      return absl::UnavailableError("Calibration is changing; reopen Rink map after it finishes");
    auto config_lock = rink::GameConfigTransactionLock::TryAcquire(game_directory.toStdString());
    if (!config_lock.ok())
      return config_lock.status();
    auto complete = CalibrationComplete(game_directory);
    if (!complete.ok())
      return complete;
    auto provenance = rink::HuginProject::ReadCanvasProvenanceLocked(game_directory.toStdString());
    if (!provenance.ok() || !provenance->has_value() || (**provenance).mapping_backend != rink::MappingBackend::kNona)
      return absl::FailedPreconditionError("Rink mapping currently needs a completed NONA camera calibration");
    auto generation = rink::stitch_artifact_preflight_generation_id_locked(game_directory.toStdString());
    if (!generation.ok())
      return generation.status();
    result.generation = GenerationDigest(*generation);
    auto revision = CalibrationRevision(game_directory);
    if (!revision.ok())
      return revision.status();
    result.revision = *revision;
    auto saved = ReadSavedMap(game_directory);
    if (!saved.ok())
      return saved.status();
    result.saved_map = *saved;
    auto pto = rink::read_bounded_regular_file_no_follow(
        std::filesystem::path(game_directory.toStdString()) / "autooptimiser_out.pto",
        4 * 1024 * 1024,
        "rink-map project");
    if (!pto.ok())
      return pto.status();
    auto project = rink::PrepareRinkLevelingProject(*pto);
    if (!project.ok() || project->image_sizes.size() != 2)
      return absl::FailedPreconditionError("Rink mapping requires two calibrated source cameras");
    QFile sphere(result.temporary->filePath("sphere.pto"));
    if (!sphere.open(QIODevice::WriteOnly) ||
        sphere.write(QByteArray::fromStdString(project->pto)) != qint64(project->pto.size()))
      return absl::InternalError("Could not prepare rink coordinate transforms");
    sphere.close();
    for (int camera = 0; camera < 2; ++camera) {
      files[camera] = std::make_unique<QFile>(QDir(game_directory).filePath(camera ? "right.png" : "left.png"));
      if (!files[camera]->open(QIODevice::ReadOnly))
        return absl::NotFoundError("The saved camera images are unavailable");
      result.original_sizes[camera] = QSize(project->image_sizes[camera][0], project->image_sizes[camera][1]);
    }
  }
  for (int camera = 0; camera < 2; ++camera) {
    QSize proxy = result.original_sizes[camera];
    proxy.scale(2048, 2048, Qt::KeepAspectRatio);
    const QString pinned_path = QString("/proc/self/fd/%1").arg(files[camera]->handle());
    QImageReader reader(pinned_path, "png");
    if (reader.size() != result.original_sizes[camera])
      return absl::DataLossError("Camera image sizes do not match the calibration");
    result.images[camera] = readCalibrationPng(pinned_path, proxy);
    if (result.images[camera].isNull())
      return absl::DataLossError("Could not decode the saved camera image");
  }
  return result;
}

absl::Status SaveRinkMap(RinkMapSnapshot& snapshot, const rink::RinkMapDocument& document) {
  auto text = rink::SerializeRinkMap(document);
  if (!text.ok())
    return text.status();
  auto lock = rink::try_lock_canvas_constraint_artifacts(snapshot.game_directory.toStdString());
  if (!lock.ok())
    return lock.status();
  if (!*lock)
    return absl::UnavailableError("Calibration is changing; wait for it to finish");
  auto config_lock = rink::GameConfigTransactionLock::TryAcquire(snapshot.game_directory.toStdString());
  if (!config_lock.ok())
    return config_lock.status();
  auto complete = CalibrationComplete(snapshot.game_directory);
  if (!complete.ok())
    return complete;
  auto revision = CalibrationRevision(snapshot.game_directory);
  auto generation = rink::stitch_artifact_preflight_generation_id_locked(snapshot.game_directory.toStdString());
  if (!generation.ok() || GenerationDigest(*generation) != snapshot.generation ||
      document.generation != snapshot.generation || document.calibration_revision != snapshot.revision.toStdString() ||
      !revision.ok() || *revision != snapshot.revision)
    return absl::FailedPreconditionError("Calibration changed while the map was open. Reopen Rink map before saving.");
  auto saved = ReadSavedMap(snapshot.game_directory);
  if (!saved.ok())
    return saved.status();
  if (*saved != snapshot.saved_map)
    return absl::FailedPreconditionError("Another editor saved this rink map. Reopen it before saving your changes.");
  auto status =
      rink::publish_named_file(std::filesystem::path(snapshot.game_directory.toStdString()) / "rink-map.yaml", *text);
  if (status.ok())
    snapshot.saved_map = *text;
  return status;
}

absl::StatusOr<RinkMapWorkResult> UpdateRinkMap(
    const RinkMapSnapshot& snapshot,
    rink::RinkMapDocument document,
    bool detect,
    const std::function<bool()>& cancelled) {
  RinkMapWorkResult result;
  if (detect) {
    document.observations.erase(
        std::remove_if(
            document.observations.begin(),
            document.observations.end(),
            [](const auto& observation) { return observation.automatic; }),
        document.observations.end());
    const char* override_path = std::getenv("HM_RINK_LANDMARKS_ONNX_MODEL");
    const QString model_path = override_path && *override_path
        ? QString::fromLocal8Bit(override_path)
        : QDir::home().filePath(".cache/hstream/models/rink-landmarks-mask2former-swin-s-76b96e34a9386267.onnx");
    if (!(override_path && *override_path)) {
      hm::assets::AssetSpec spec;
      spec.name = "Rink landmarks";
      spec.url =
          "https://github.com/cjolivier01/HockeyMONStream/releases/download/pretrained-assets-v1/"
          "rink-landmarks-mask2former-swin-s-76b96e34a9386267.onnx";
      spec.sha256 = "76b96e34a9386267c08e0edc6cb09b2196a94a933b27e2f92fcae5d9f0426a08";
      spec.target = model_path.toStdString();
      spec.on_demand = true;
      auto status = hm::assets::AssetManager::EnsureAsset(spec, {}, cancelled);
      if (!status.ok())
        return status;
    }
    if (!QFile::exists(model_path))
      return absl::NotFoundError("The native rink-landmark model is not installed. Manual marks remain available.");
    auto model = rink::RinkLandmarks::Create(model_path.toStdString());
    if (!model.ok())
      return model.status();
    for (int camera = 0; camera < 2; ++camera) {
      if (cancelled && cancelled())
        return absl::CancelledError("Rink landmark detection stopped");
      const auto image = snapshot.images[camera].convertToFormat(QImage::Format_RGB888);
      cv::Mat rgb(image.height(), image.width(), CV_8UC3, const_cast<uchar*>(image.constBits()), image.bytesPerLine());
      cv::Mat bgr;
      cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
      auto detections = (*model)->Infer(bgr, cancelled);
      if (!detections.ok())
        return detections.status();
      const double sx = double(snapshot.original_sizes[camera].width()) / image.width();
      const double sy = double(snapshot.original_sizes[camera].height()) / image.height();
      for (auto& detection : *detections) {
        for (auto* points : {&detection.outline, &detection.line_samples})
          for (auto& point : *points)
            point = {(point.x + 0.5) * sx - 0.5, (point.y + 0.5) * sy - 0.5};
        if (!detection.line_samples.empty()) {
          rink::RinkMapObservation observation;
          observation.feature = detection.class_index == 2 ? "center-line" : "";
          observation.label = detection.label;
          observation.camera = camera;
          observation.line = true;
          observation.automatic = true;
          observation.confidence = detection.score;
          observation.pixels = detection.line_samples;
          document.observations.push_back(std::move(observation));
        }
      }
      result.detections[camera] = std::move(*detections);
    }
  }
  auto status = ResolveRays(snapshot, document, cancelled);
  if (!status.ok())
    return status;
  auto fit = rink::FitRinkMap(document);
  if (!fit.ok())
    return fit.status();
  auto projected = ProjectLayout(snapshot, document, *fit, cancelled);
  if (!projected.ok())
    return projected.status();
  result.document = std::move(document);
  result.fit = *fit;
  result.projected_layout = std::move(*projected);
  return result;
}
