#include "src/apps/hstream-ui/StitchingStillPreview.h"

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>

#include <sys/stat.h>
#include <memory>

namespace {

QByteArray revision(const QString& directory) {
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
bool settings_match(const YAML::Node& config, const QByteArray& expected_settings) {
  const auto calibration = config["hstream_ui"]["stitching_calibration"];
  if (!calibration["backend_generation"])
    return true; // Legacy completed calibrations have no parameter claim.
  const auto owner = calibration["invalidation_id"].as<std::string>("");
  if (owner.empty())
    return false;
  YAML::Node effective = YAML::Clone(config);
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
  choices.control_point_execution_provider = *provider;
  choices.calibration_frame_selection_fingerprint = *selection;
  choices.manual_control_point_fingerprint = *manual;
  return hm::stitching::validate_stitching_backend_generation(config, owner, choices).ok();
}

struct Still {
  QImage image;
  QString name;
  bool retry{false};
};

Still load(
    const QString& directory,
    const QString& expected_generation,
    const QByteArray& expected_settings,
    const QByteArray& expected_revision) {
  Still result;
  bool png_current = false;
  QFile png(QDir(directory).filePath("s.png"));
  QFile tiff(QDir(directory).filePath("panorama.tif"));
  {
    const auto path = directory.toStdString();
    auto artifacts = hm::stitching::try_lock_canvas_constraint_artifacts(path);
    if (!artifacts.ok() || !*artifacts) {
      result.retry = true;
      return result;
    }
    auto config_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(path);
    if (!config_lock.ok()) {
      result.retry = true;
      return result;
    }
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
      const auto generation = hm::stitching::stitch_artifact_preflight_generation_id_locked(path);
      if (!generation.ok() || *generation != expected_generation.toStdString())
        return result;
    }
    if (revision(directory) != expected_revision) {
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
  return result;
}

} // namespace

StitchingStillPreview::StitchingStillPreview(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setGeometry(parent->rect());
  parent->installEventFilter(this);
  hide();
  auto* timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, [this] { refresh(); });
  timer->start();
}

void StitchingStillPreview::setSource(
    const QString& directory,
    const QString& expected_generation,
    const QByteArray& expected_settings) {
  if (directory_ == directory && expected_generation_ == expected_generation && expected_settings_ == expected_settings)
    return;
  directory_ = directory;
  expected_generation_ = expected_generation;
  expected_settings_ = expected_settings;
  ++request_;
  revision_.clear();
  image_ = {};
  setProperty("calibrationImage", QVariant());
  hide();
  refresh();
}

void StitchingStillPreview::refresh() {
  if (directory_.isEmpty())
    return;
  const auto current = revision(directory_);
  if (revision_ == current)
    return;
  image_ = {};
  setProperty("calibrationImage", QVariant());
  hide();
  if (worker_)
    return;
  const auto request = request_;
  const auto directory = directory_;
  const auto expected = expected_generation_;
  const auto settings = expected_settings_;
  auto result = std::make_shared<Still>();
  worker_ = QThread::create([directory, expected, settings, current, result] {
    try {
      *result = load(directory, expected, settings, current);
    } catch (const std::exception&) {
      // A failed optional image must not terminate playback or the UI.
      *result = {};
    }
  });
  connect(worker_, &QThread::finished, this, [this, result, request, current] {
    worker_ = nullptr;
    if (request != request_ || directory_.isEmpty() || revision(directory_) != current)
      return;
    if (!result->retry)
      revision_ = current;
    image_ = std::move(result->image);
    if (!image_.isNull()) {
      setProperty("calibrationImage", result->name);
      setAccessibleName("Saved stitching calibration image");
      setToolTip("Saved calibration image; Play starts the live stitched preview.");
      show();
      raise();
      update();
    }
  });
  connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
  worker_->start();
}

bool StitchingStillPreview::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && event->type() == QEvent::Resize)
    setGeometry(parentWidget()->rect());
  return QWidget::eventFilter(watched, event);
}

void StitchingStillPreview::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  if (image_.isNull())
    return;
  const QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
  const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(target, image_);
}
