#include "src/apps/hstream-ui/RinkMapBackend.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

#include "absl/strings/numbers.h"
#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/FieldMaskArtifact.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "hstream/src/libs/stitching/RinkMaskGeometry.h"
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
  auto revision = rink::RinkMapCalibrationRevision(directory.toStdString());
  if (!revision.ok())
    return revision.status();
  return QByteArray::fromStdString(*revision);
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
    // Hugin writes C-locale numbers, regardless of the desktop's numeric locale.
    // Reverse projections can return nan/inf for points outside a camera.
    double px = 0, py = 0;
    if (!absl::SimpleAtod(x, &px) || !absl::SimpleAtod(y, &py) ||
        (!allow_outside && (!std::isfinite(px) || !std::isfinite(py))))
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
absl::StatusOr<std::vector<cv::Point2d>> ProjectRays(
    const RinkMapSnapshot& snapshot,
    const std::vector<cv::Vec3d>& rays,
    int camera,
    const std::function<bool()>& cancelled) {
  std::ostringstream input;
  input.imbue(std::locale::classic());
  input << std::setprecision(17);
  for (auto ray : rays) {
    ray /= cv::norm(ray);
    input << camera << ' ' << 1799.5 - std::atan2(ray[1], ray[0]) * 1800 / kPi << ' '
          << 899.5 - std::asin(std::clamp(ray[2], -1.0, 1.0)) * 1800 / kPi << '\n';
  }
  auto output = Transform(snapshot, QByteArray::fromStdString(input.str()), true, cancelled);
  if (!output.ok())
    return output.status();
  return ReadPairs(*output, rays.size(), true);
}

bool InsideCamera(const RinkMapSnapshot& snapshot, int camera, cv::Point2d p) {
  const auto size = snapshot.original_sizes[camera];
  return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x < size.width() && p.y < size.height();
}

absl::StatusOr<rink::RinkMapObservation> MaskBoard(
    const RinkMapSnapshot& snapshot,
    const std::function<bool()>& cancelled) {
  QFile mask_file;
  QSize mask_size;
  QString pinned_path;
  std::string pto, output_generation, hugin_generation;
  rink::MappingCanvasPlacement placement;
  struct stat identity{};
  const auto status =
      rink::try_visit_current_field_mask(snapshot.game_directory.toStdString(), [&](const std::string& path) {
        // The visitor holds both publication locks. Never acquire them recursively.
        const auto root = std::filesystem::path(snapshot.game_directory.toStdString());
        const auto generation = rink::stitch_artifact_preflight_generation_id_locked(root);
        const auto revision = CalibrationRevision(snapshot.game_directory);
        if (!generation.ok() || !revision.ok() || GenerationDigest(*generation) != snapshot.generation ||
            *revision != snapshot.revision)
          return absl::FailedPreconditionError("Calibration changed. Reopen Rink map before using the mask.");
        hugin_generation = *generation;
        const auto config_text =
            rink::read_bounded_regular_file_no_follow(root / "config.yaml", 4 * 1024 * 1024, "rink settings");
        if (!config_text.ok())
          return config_text.status();
        try {
          output_generation = YAML::Load(*config_text)["rink"]["stitched_output_generation"].as<std::string>();
        } catch (const YAML::Exception& error) {
          return absl::InvalidArgumentError(error.what());
        }
        const auto canvas = rink::read_mapping_canvas_placement_locked(root);
        const auto project =
            rink::read_bounded_regular_file_no_follow(root / "autooptimiser_out.pto", 4 * 1024 * 1024, "rink project");
        if (!canvas.ok())
          return canvas.status();
        if (!project.ok())
          return project.status();
        placement = *canvas;
        pto = *project;
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0)
          return absl::NotFoundError("The saved rink mask is unavailable");
        if (!mask_file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
          ::close(descriptor);
          return absl::InternalError("Could not pin the rink mask");
        }
        if (::fstat(descriptor, &identity) != 0 || !S_ISREG(identity.st_mode) || identity.st_size <= 0 ||
            identity.st_size > 64 * 1024 * 1024)
          return absl::ResourceExhaustedError("The rink mask is not a bounded regular PNG");
        pinned_path = QString("/proc/self/fd/%1").arg(descriptor);
        QImageReader reader(pinned_path, "png");
        mask_size = reader.size();
        if (mask_size.width() <= 0 || mask_size.height() <= 0 || mask_size.width() > 65535 ||
            mask_size.height() > 65535 || uint64_t(mask_size.width()) * mask_size.height() > 256ULL * 1024 * 1024)
          return absl::ResourceExhaustedError("The rink mask has invalid dimensions");
        return rink::validate_stitched_output_generation_dimensions(
            output_generation, mask_size.width(), mask_size.height());
      });
  if (!status.ok())
    return status;
  if (cancelled && cancelled())
    return absl::CancelledError("Rink mask assistance stopped");
  QSize proxy_size = mask_size;
  if (std::max(proxy_size.width(), proxy_size.height()) > 2048)
    proxy_size.scale(2048, 2048, Qt::KeepAspectRatio);
  const auto decoded = readCalibrationPng(pinned_path, proxy_size).convertToFormat(QImage::Format_Grayscale8);
  if (decoded.isNull())
    return absl::DataLossError("Could not decode the saved rink mask");
  const auto digest = hm::assets::AssetManager::Sha256(pinned_path.toStdString(), cancelled);
  if (!digest.ok())
    return digest.status();
  rink::PlaybackRinkMap projection;
  // OutputRotation validates both the full dimensions and the Hugin identity.
  projection.hugin_generation = hugin_generation;
  const auto rotation = projection.OutputRotation(output_generation, mask_size.width(), mask_size.height());
  if (!rotation)
    return absl::InvalidArgumentError("The rink mask has unsupported output geometry");
  const auto rays = rink::RinkProjectionRays(pto, placement, projection.columns, projection.rows, cancelled);
  if (!rays.ok())
    return rays.status();
  projection.rays = *rays;
  cv::Mat mask(
      decoded.height(), decoded.width(), CV_8UC1, const_cast<uchar*>(decoded.constBits()), decoded.bytesPerLine());
  cv::Mat unrotated;
  const double c = std::cos(*rotation * kPi / 180), s = std::sin(*rotation * kPi / 180);
  const double sx = double(mask.cols) / mask_size.width(), sy = double(mask.rows) / mask_size.height();
  const double cx = (mask.cols - 1) / 2.0, cy = (mask.rows - 1) / 2.0;
  const cv::Matx23d inverse(
      c, s * sx / sy, cx - c * cx - s * sx / sy * cy, -s * sy / sx, c, cy + s * sy / sx * cx - c * cy);
  cv::warpAffine(mask, unrotated, inverse, mask.size(), cv::INTER_NEAREST | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT);
  auto board = rink::SuggestFarRinkBoard(unrotated, [&](double x, double y) -> std::optional<cv::Vec3d> {
    const double dx = (x - 0.5) * mask_size.width(), dy = (y - 0.5) * mask_size.height();
    const double ox = 0.5 + (c * dx + s * dy) / mask_size.width();
    const double oy = 0.5 + (-s * dx + c * dy) / mask_size.height();
    // Do not fit newly exposed rotation padding or the original clipped frame.
    if (ox < 0.02 || ox > 0.98 || oy < 0.02 || oy > 0.98)
      return std::nullopt;
    return projection.ViewingRay(x, y, 0, mask_size.width(), mask_size.height());
  });
  if (!board.ok())
    return board.status();
  rink::RinkMapObservation result;
  result.feature = "far-boards";
  result.label = "Ice mask far boards";
  result.line = result.automatic = true;
  result.enabled = false; // Silhouette agreement does not establish ice level.
  result.confidence = 0.5;
  result.mask_revision = *digest;
  double best_span = 0;
  for (int camera = 0; camera < 2; ++camera) {
    const auto points = ProjectRays(snapshot, board->rays, camera, cancelled);
    if (!points.ok())
      return points.status();
    std::vector<cv::Point2d> pixels;
    std::vector<cv::Vec3d> visible;
    for (size_t i = 0; i < points->size(); ++i)
      if (InsideCamera(snapshot, camera, points->at(i))) {
        pixels.push_back(points->at(i));
        visible.push_back(board->rays[i]);
      }
    if (visible.size() < 8)
      continue;
    const double span = std::acos(std::clamp(visible.front().dot(visible.back()), -1.0, 1.0));
    if (span > best_span) {
      best_span = span;
      result.camera = camera;
      result.pixels = std::move(pixels);
      result.rays = std::move(visible);
    }
  }
  if (best_span < 12 * kPi / 180)
    return absl::NotFoundError("Too little of the far boards is visible in either saved camera");
  struct stat current{}, pinned{};
  const auto mask_path = QDir(snapshot.game_directory).filePath("rink_mask_0.png").toStdString();
  if (::lstat(mask_path.c_str(), &current) != 0 || ::fstat(mask_file.handle(), &pinned) != 0 ||
      current.st_dev != identity.st_dev || current.st_ino != identity.st_ino || pinned.st_size != identity.st_size ||
      pinned.st_mtim.tv_sec != identity.st_mtim.tv_sec || pinned.st_mtim.tv_nsec != identity.st_mtim.tv_nsec ||
      pinned.st_ctim.tv_sec != identity.st_ctim.tv_sec || pinned.st_ctim.tv_nsec != identity.st_ctim.tv_nsec)
    return absl::AbortedError("The rink mask changed during analysis. Try the mask again.");
  return result;
}
absl::StatusOr<std::vector<rink::RinkMapObservation>> MaskIntersections(
    const RinkMapSnapshot& snapshot,
    const rink::RinkMapDocument& document,
    const std::function<bool()>& cancelled) {
  std::vector<rink::RinkMapObservation> result;
  const auto board = std::find_if(document.observations.begin(), document.observations.end(), [](const auto& o) {
    return !o.mask_revision.empty() && o.feature == "far-boards" && o.line && o.rays.size() >= 2;
  });
  if (board == document.observations.end())
    return result;
  const auto normal = [](const auto& rays) {
    cv::Matx33d covariance = cv::Matx33d::zeros();
    for (const auto& ray : rays)
      covariance += ray * ray.t();
    cv::Mat values, vectors;
    cv::eigen(cv::Mat(covariance), values, vectors);
    return cv::Vec3d(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
  };
  const auto board_normal = normal(board->rays);
  const auto angle = [](cv::Vec3d a, cv::Vec3d b) { return std::acos(std::clamp(a.dot(b), -1.0, 1.0)); };
  const auto first = board->rays.front(), last = board->rays.back();
  std::vector<cv::Vec3d> rays;
  std::vector<std::string> identities;
  for (const auto& o : document.observations) {
    if (!o.enabled || !o.line || o.rays.size() < 2 ||
        (o.feature != "center-line" && o.feature != "blue-left-line" && o.feature != "blue-right-line" &&
         o.feature != "goal-left-line" && o.feature != "goal-right-line"))
      continue;
    const auto identity = o.feature.substr(0, o.feature.size() - 5) + "-far";
    if (std::find(identities.begin(), identities.end(), identity) != identities.end())
      continue;
    auto intersection = board_normal.cross(normal(o.rays));
    if (cv::norm(intersection) < 0.1)
      continue;
    intersection /= cv::norm(intersection);
    if (intersection.dot(first + last) < 0)
      intersection = -intersection;
    // Do not extend a short visible mask section into hidden corners or ends.
    if (angle(first, intersection) + angle(intersection, last) > angle(first, last) + 0.5 * kPi / 180)
      continue;
    identities.push_back(identity);
    rays.push_back(intersection);
  }
  if (rays.empty())
    return result;
  for (int camera = 0; camera < 2; ++camera) {
    auto points = ProjectRays(snapshot, rays, camera, cancelled);
    if (!points.ok())
      return points.status();
    for (size_t i = 0; i < points->size(); ++i) {
      if (!InsideCamera(snapshot, camera, points->at(i)))
        continue;
      rink::RinkMapObservation guide;
      guide.camera = camera;
      guide.feature = identities[i];
      guide.pixels = {points->at(i)};
      guide.rays = {rays[i]};
      result.push_back(std::move(guide));
    }
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

absl::StatusOr<rink::RinkCornerLevelingEstimate> RinkLandmarkLeveling(
    const RinkMapWorkResult& landmarks,
    const std::array<double, 3>& published_rotation,
    double yaw) {
  const auto vector = [](cv::Vec3d ray) { return std::array<double, 3>{ray[0], ray[1], ray[2]}; };
  if (landmarks.fit.state == rink::RinkMapState::kReady) {
    // A validated plane uses every enabled observation, including automatic
    // line samples. The synthetic rectangle only extracts that plane's normal.
    std::vector<rink::RinkLevelingRayLine> edges;
    for (double x : {-landmarks.document.layout.blue_line_distance, landmarks.document.layout.blue_line_distance}) {
      auto near = rink::RinkPointToRay(landmarks.fit, {x, -landmarks.document.layout.width / 2});
      auto far = rink::RinkPointToRay(landmarks.fit, {x, landmarks.document.layout.width / 2});
      if (!near.ok() || !far.ok())
        return absl::FailedPreconditionError("The fitted map cannot provide leveling rays");
      edges.push_back({vector(*near), vector(*far)});
    }
    return rink::EstimateRinkLevelingFromCorners(edges, published_rotation, yaw);
  }
  // Never treat arbitrary line-segment endpoints as board intersections.
  // A goal line can be cut off by players, boards or the camera crop.
  for (const auto& pair :
       {std::pair{"goal-left", "goal-right"},
        std::pair{"blue-left", "blue-right"},
        std::pair{"center", "blue-left"},
        std::pair{"center", "blue-right"}}) {
    std::vector<rink::RinkLevelingRayLine> edges;
    for (const char* end : {pair.first, pair.second}) {
      std::array<cv::Vec3d, 2> rays{};
      for (int side = 0; side < 2; ++side) {
        const std::string identity = std::string(end) + (side ? "-far" : "-near");
        size_t count = 0;
        for (const auto& mark : landmarks.document.observations) {
          if (!mark.enabled || mark.line || mark.feature != identity || mark.rays.size() != 1)
            continue;
          if (count && cv::norm(rays[side] / cv::norm(rays[side]) - mark.rays.front()) > 0.035)
            return absl::FailedPreconditionError(
                "Repeated " + identity + " points disagree between camera views; review or disable a suspect point");
          rays[side] += mark.rays.front();
          ++count;
        }
        if (count)
          rays[side] /= cv::norm(rays[side]);
      }
      if (cv::norm(rays[0]) > 0.5 && cv::norm(rays[1]) > 0.5)
        edges.push_back({vector(rays[0]), vector(rays[1])});
    }
    if (edges.size() == 2)
      return rink::EstimateRinkLevelingFromCorners(edges, published_rotation, yaw);
  }
  return absl::FailedPreconditionError(
      "Saved landmarks cannot determine leveling yet. Mark the near/far board intersections of both goal lines "
      "at ice level, or complete the rink map with independent landmarks. Detected goal-line segments alone "
      "have arbitrary endpoints and do not define the four corners. Your partial marks remain saved.");
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
    const std::function<bool()>& cancelled,
    bool use_mask) {
  RinkMapWorkResult result;
  if (use_mask) {
    auto board = MaskBoard(snapshot, cancelled);
    if (!board.ok())
      return board.status();
    document.observations.erase(
        std::remove_if(
            document.observations.begin(),
            document.observations.end(),
            [](const auto& observation) { return observation.automatic && !observation.mask_revision.empty(); }),
        document.observations.end());
    document.observations.push_back(*board);
    result.assistance_message =
        "Far-board suggestion added unchecked. Review it at ice level, then check Use. The near mask edge is excluded because boards, glass and posts can hide the ice.";
  }
  if (detect) {
    document.observations.erase(
        std::remove_if(
            document.observations.begin(),
            document.observations.end(),
            [](const auto& observation) { return observation.automatic && observation.mask_revision.empty(); }),
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
  auto guides = MaskIntersections(snapshot, result.document, cancelled);
  if (!guides.ok())
    return guides.status();
  result.mask_intersections = std::move(*guides);
  return result;
}
