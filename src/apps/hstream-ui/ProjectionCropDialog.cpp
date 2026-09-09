#include "src/apps/hstream-ui/ProjectionCropDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QImageReader>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "src/apps/hstream-ui/RinkLevelingDialog.h"

namespace {
constexpr int kLeft = 1, kRight = 2, kTop = 4, kBottom = 8, kMove = 16;
constexpr double kMinimumSpan = 0.0001;
constexpr std::array<double, 4> kFullCrop{0, 1, 0, 1};

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024 ? file.readAll() : QByteArray();
}

// The crop does not affect the projection's coordinate system.
hm::stitching::StitchProjectionFraming withoutCrop(hm::stitching::StitchProjectionFraming framing) {
  framing.crop = kFullCrop;
  framing.auto_crop = false;
  return framing;
}
} // namespace

ProjectionCropCanvas::ProjectionCropCanvas(QWidget* parent) : QWidget(parent) {
  setObjectName("projectionCropCanvas");
  setMinimumSize(360, 220);
  setMouseTracking(true);
}

void ProjectionCropCanvas::setImage(const QImage& image) {
  image_ = image;
  setAccessibleDescription(image_.isNull() ? placeholder_message_ : QString());
  update();
}

void ProjectionCropCanvas::setPlaceholderMessage(const QString& message) {
  placeholder_message_ = message;
  if (image_.isNull())
    setAccessibleDescription(message);
  update();
}

void ProjectionCropCanvas::setCrop(const std::array<double, 4>& crop, bool editable, bool keep_full_width) {
  crop_ = crop;
  editable_ = editable;
  keep_full_width_ = keep_full_width;
  update();
}

QRectF ProjectionCropCanvas::imageRect() const {
  if (image_.isNull())
    return {};
  const QSizeF available = QSizeF(size()) - QSizeF(32, 32);
  const double scale = std::min(available.width() / image_.width(), available.height() / image_.height());
  const QSizeF shown = QSizeF(image_.size()) * scale;
  return QRectF(QPointF((width() - shown.width()) / 2, (height() - shown.height()) / 2), shown);
}

QRectF ProjectionCropCanvas::cropRect() const {
  const auto image = imageRect();
  return QRectF(
      image.left() + crop_[0] * image.width(),
      image.top() + crop_[2] * image.height(),
      (crop_[1] - crop_[0]) * image.width(),
      (crop_[3] - crop_[2]) * image.height());
}

void ProjectionCropCanvas::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), QColor(18, 23, 29));
  if (image_.isNull()) {
    painter.setPen(QColor(210, 218, 225));
    painter.drawText(rect().adjusted(32, 24, -32, -24), Qt::AlignCenter | Qt::TextWordWrap, placeholder_message_);
    return;
  }
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  const auto image = imageRect(), crop = cropRect();
  painter.drawImage(image, image_);
  QPainterPath dim;
  dim.addRect(image);
  dim.addRect(crop);
  painter.fillPath(dim, QColor(0, 0, 0, 155));
  painter.setPen(QPen(QColor(86, 211, 255), 2));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(crop);
  if (!editable_)
    return;
  painter.setBrush(QColor(86, 211, 255));
  for (const auto point :
       {crop.topLeft(),
        crop.topRight(),
        crop.bottomLeft(),
        crop.bottomRight(),
        QPointF(crop.center().x(), crop.top()),
        QPointF(crop.center().x(), crop.bottom()),
        QPointF(crop.left(), crop.center().y()),
        QPointF(crop.right(), crop.center().y())}) {
    if (keep_full_width_ && (point.x() == crop.left() || point.x() == crop.right()))
      continue;
    painter.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
  }
}

int ProjectionCropCanvas::hitTest(const QPointF& point) const {
  if (!editable_ || image_.isNull())
    return 0;
  const auto crop = cropRect();
  if (!crop.adjusted(-10, -10, 10, 10).contains(point))
    return 0;
  int edges = 0;
  if (!keep_full_width_) {
    if (std::abs(point.x() - crop.left()) <= 10)
      edges |= kLeft;
    else if (std::abs(point.x() - crop.right()) <= 10)
      edges |= kRight;
  }
  if (std::abs(point.y() - crop.top()) <= 10)
    edges |= kTop;
  else if (std::abs(point.y() - crop.bottom()) <= 10)
    edges |= kBottom;
  return edges ? edges : (crop.contains(point) ? kMove : 0);
}

void ProjectionCropCanvas::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !(dragged_ = hitTest(event->position())))
    return;
  press_ = event->position();
  drag_start_ = crop_;
  event->accept();
}

void ProjectionCropCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (!dragged_) {
    const int hit = hitTest(event->position());
    setCursor(
        hit == kMove                                             ? Qt::SizeAllCursor
            : hit == kLeft || hit == kRight                      ? Qt::SizeHorCursor
            : hit == kTop || hit == kBottom                      ? Qt::SizeVerCursor
            : hit == (kLeft | kTop) || hit == (kRight | kBottom) ? Qt::SizeFDiagCursor
            : hit                                                ? Qt::SizeBDiagCursor
                                                                 : Qt::ArrowCursor);
    return;
  }
  const auto image = imageRect();
  const QPointF delta = event->position() - press_;
  double dx = delta.x() / image.width(), dy = delta.y() / image.height();
  crop_ = drag_start_;
  if (dragged_ == kMove) {
    dx = keep_full_width_ ? 0 : std::clamp(dx, -crop_[0], 1 - crop_[1]);
    dy = std::clamp(dy, -crop_[2], 1 - crop_[3]);
    crop_[0] += dx;
    crop_[1] += dx;
    crop_[2] += dy;
    crop_[3] += dy;
  } else {
    if (dragged_ & kLeft)
      crop_[0] = std::clamp(crop_[0] + dx, 0.0, crop_[1] - kMinimumSpan);
    if (dragged_ & kRight)
      crop_[1] = std::clamp(crop_[1] + dx, crop_[0] + kMinimumSpan, 1.0);
    if (dragged_ & kTop)
      crop_[2] = std::clamp(crop_[2] + dy, 0.0, crop_[3] - kMinimumSpan);
    if (dragged_ & kBottom)
      crop_[3] = std::clamp(crop_[3] + dy, crop_[2] + kMinimumSpan, 1.0);
  }
  if (cropChanged)
    cropChanged(crop_);
  update();
  event->accept();
}

void ProjectionCropCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && dragged_) {
    mouseMoveEvent(event);
    dragged_ = 0;
    event->accept();
  }
}

ProjectionCropDialog::ProjectionCropDialog(
    const QString& game_directory,
    const hm::stitching::StitchProjectionFraming& framing,
    const QString& projection,
    const std::vector<double>& projection_parameters,
    const hm::stitching::StitchCameraSelection& camera,
    const QString& preview_unavailable_reason,
    QWidget* parent)
    : QDialog(parent),
      game_directory_(game_directory),
      initial_(framing),
      projection_(projection),
      projection_parameters_(projection_parameters),
      camera_(camera),
      manual_crop_(framing.crop),
      saved_horizontal_{framing.crop[0], framing.crop[1]} {
  setObjectName("projectionCropDialog");
  setWindowTitle("Frame the stitched rink");
  resize(1080, 760);
  auto* layout = new QVBoxLayout(this);
  auto* instructions = new QLabel(
      "Choose Auto, keep the full canvas, or trim each edge yourself. In Manual, drag an edge or corner to resize "
      "the rectangle; drag inside to move it. Dimmed areas will be removed. Black corners may remain.");
  instructions->setWordWrap(true);
  layout->addWidget(instructions);
  mode_ = new QComboBox();
  mode_->setObjectName("projectionCropMode");
  mode_->addItem("Auto — crop valid pixels", "auto");
  mode_->addItem("Full canvas", "full");
  mode_->addItem("Manual", "manual");
  mode_->setCurrentIndex(initial_.auto_crop ? 0 : initial_.crop == kFullCrop ? 1 : 2);
  layout->addWidget(mode_);
  canvas_ = new ProjectionCropCanvas();
  layout->addWidget(canvas_, 1);
  auto* controls = new QGridLayout();
  keep_width_ = new QCheckBox("Keep full width");
  keep_width_->setObjectName("projectionCropKeepWidth");
  keep_width_->setToolTip("Uncheck this in Manual mode to adjust the left and right edges.");
  keep_width_->setChecked(manual_crop_[0] == 0 && manual_crop_[1] == 1);
  controls->addWidget(keep_width_, 0, 0, 1, 4);
  const std::array<QString, 4> names{"Left", "Right", "Top", "Bottom"};
  for (size_t index = 0; index < edges_.size(); ++index) {
    auto* label = new QLabel(names[index] + " trim");
    auto* spin = new QDoubleSpinBox();
    spin->setObjectName("projectionCrop" + names[index]);
    spin->setRange(0, 99.99);
    spin->setDecimals(4);
    spin->setSingleStep(1);
    spin->setSuffix(" %");
    label->setBuddy(spin);
    edges_[index] = spin;
    controls->addWidget(label, 1 + index / 2, (index % 2) * 2);
    controls->addWidget(spin, 1 + index / 2, (index % 2) * 2 + 1);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, index](double trim) {
      manual_crop_[index] = (index == 1 || index == 3) ? 1 - trim / 100 : trim / 100;
      manual_edited_ = true;
      syncControls();
    });
  }
  layout->addLayout(controls);
  coverage_ = new QLabel();
  coverage_->setObjectName("projectionCropCoverage");
  coverage_->setWordWrap(true);
  layout->addWidget(coverage_);
  status_ = new QLabel("Rendering the full projection from the saved calibration…");
  status_->setObjectName("projectionCropStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto* note = new QLabel("Use crop returns this selection to the controls. Save Preset applies it to this game.");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  accept_ = buttons->addButton("Use crop", QDialogButtonBox::AcceptRole);
  accept_->setObjectName("acceptProjectionCropButton");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &ProjectionCropDialog::reject);
  connect(accept_, &QPushButton::clicked, this, [this]() { acceptCrop(); });
  connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
    seedManualFromAuto();
    syncControls();
  });
  connect(keep_width_, &QCheckBox::toggled, this, [this](bool keep) {
    if (keep) {
      saved_horizontal_ = {manual_crop_[0], manual_crop_[1]};
      manual_crop_[0] = 0;
      manual_crop_[1] = 1;
    } else {
      manual_crop_[0] = saved_horizontal_[0];
      manual_crop_[1] = saved_horizontal_[1];
    }
    manual_edited_ = true;
    syncControls();
  });
  canvas_->cropChanged = [this](const auto& crop) {
    manual_crop_ = crop;
    manual_edited_ = true;
    syncControls();
  };
  syncControls();
  if (preview_unavailable_reason.isEmpty())
    QTimer::singleShot(0, this, [this]() { loadPreview(); });
  else
    previewFailed(preview_unavailable_reason);
}

ProjectionCropDialog::~ProjectionCropDialog() {
  stopTool();
}

hm::stitching::StitchProjectionFraming ProjectionCropDialog::framing() const {
  auto result = initial_;
  result.auto_crop = mode_->currentData() == "auto";
  result.crop = mode_->currentData() == "manual" ? manual_crop_ : kFullCrop;
  return result;
}

void ProjectionCropDialog::seedManualFromAuto() {
  if (mode_->currentData() != "manual" || !initial_.auto_crop || !auto_ready_ || manual_edited_)
    return;
  manual_crop_ = auto_crop_;
  saved_horizontal_ = {manual_crop_[0], manual_crop_[1]};
  const QSignalBlocker blocker(keep_width_);
  keep_width_->setChecked(manual_crop_[0] == 0 && manual_crop_[1] == 1);
}

void ProjectionCropDialog::syncControls() {
  const bool manual = mode_->currentData() == "manual";
  const bool automatic = mode_->currentData() == "auto";
  const auto shown = manual ? manual_crop_ : automatic && auto_ready_ ? auto_crop_ : kFullCrop;
  keep_width_->setEnabled(manual);
  for (size_t index = 0; index < edges_.size(); ++index) {
    const QSignalBlocker blocker(edges_[index]);
    const size_t opposite = index ^ 1;
    const double other_trim = opposite == 1 || opposite == 3 ? 1 - shown[opposite] : shown[opposite];
    edges_[index]->setMaximum(std::max(0.0, 100 - other_trim * 100 - kMinimumSpan * 100));
    edges_[index]->setValue((index == 1 || index == 3 ? 1 - shown[index] : shown[index]) * 100);
    edges_[index]->setEnabled(manual && (index >= 2 || !keep_width_->isChecked()));
    edges_[index]->setToolTip(
        !manual ? "Choose Manual to adjust the crop edges."
            : index < 2 && keep_width_->isChecked()
            ? "Uncheck Keep full width to adjust the left and right edges."
            : "Percentage trimmed from this edge of the full projection canvas.");
  }
  canvas_->setCrop(shown, manual, keep_width_->isChecked());
  coverage_->setText(
      automatic && !auto_ready_ ? "Auto crop is calculated during stitching calibration."
                                : QString("Retaining %1% of the width and %2% of the height.")
                                      .arg((shown[1] - shown[0]) * 100, 0, 'f', 1)
                                      .arg((shown[3] - shown[2]) * 100, 0, 'f', 1));
  if (manual && keep_width_->isChecked())
    coverage_->setText(coverage_->text() + " Uncheck Keep full width to adjust the left and right edges.");
}

void ProjectionCropDialog::previewFailed(const QString& message) {
  canvas_->setPlaceholderMessage("Crop preview unavailable\n\n" + message);
  status_->setText(message + " You can still choose a crop mode or enter manual percentages.");
}

void ProjectionCropDialog::loadPreview() {
  const auto lock = hm::stitching::try_lock_canvas_constraint_artifacts(game_directory_.toStdString());
  if (!lock.ok()) {
    previewFailed("Could not access the saved stitching calibration. Check access to the game folder and try again.");
    return;
  }
  if (!*lock) {
    previewFailed("Stitching calibration is being updated. Wait for it to finish, then reopen Adjust crop.");
    return;
  }
  const auto config_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(game_directory_.toStdString());
  if (!config_lock.ok()) {
    previewFailed("Game settings are being updated. Reopen Adjust crop after the update finishes.");
    return;
  }
  // Explain calibration readiness before attempting to load its images. A
  // partial/failed run may leave old files, or have no images to snapshot yet.
  const QString config_path = QDir(game_directory_).filePath("config.yaml");
  if (!QFileInfo::exists(config_path)) {
    previewFailed(
        "Stitching calibration is not available yet. Save the game settings and run stitching calibration, "
        "then reopen Adjust crop to drag the crop rectangle.");
    return;
  }
  const auto config_contents = readFile(config_path);
  try {
    if (config_contents.isEmpty())
      throw std::invalid_argument("Unreadable game settings");
    const auto config = YAML::Load(config_contents.toStdString());
    const auto ui = config["hstream_ui"];
    if (ui && !ui.IsNull() && !ui.IsMap())
      throw std::invalid_argument("Invalid UI settings");
    const auto calibration = ui && ui.IsMap() ? ui["stitching_calibration"] : YAML::Node();
    if (calibration && !calibration.IsNull() && !calibration.IsMap())
      throw std::invalid_argument("Invalid calibration settings");
    const auto status = calibration && calibration.IsMap() ? calibration["status"] : YAML::Node();
    if (status && !status.IsNull() && status.as<std::string>() != "complete") {
      previewFailed(
          status.as<std::string>() == "failed"
              ? "Stitching calibration failed, so its crop preview is not ready. Resolve the calibration "
                "error and run calibration again, then reopen Adjust crop."
              : "Stitching calibration is incomplete. Finish calibration with the current camera and "
                "projection settings, then reopen Adjust crop to drag the crop rectangle.");
      return;
    }
  } catch (const std::exception&) {
    previewFailed("Could not read the game's calibration status. Check the game settings and reopen Adjust crop.");
    return;
  }
  for (const QString name : {"autooptimiser_out.pto", "stitching_canvas_provenance", "left.png", "right.png"}) {
    const QFileInfo file(QDir(game_directory_).filePath(name));
    if (!file.exists() || file.size() == 0) {
      previewFailed(
          "Stitching calibration has not produced all the saved camera images and projection needed for "
          "the crop preview. Complete stitching calibration, then reopen Adjust crop.");
      return;
    }
  }
  if (!temporary_.isValid()) {
    previewFailed(
        "Could not create the temporary crop preview folder. Check temporary-folder access and free disk space.");
    return;
  }
  source_revision_ = RinkLevelingDialog::sourceRevision(game_directory_);
  if (source_revision_.isEmpty()) {
    previewFailed(
        "The saved calibration files could not be read for the crop preview. Check the game files and try again.");
    return;
  }
  for (const QString name :
       {"autooptimiser_out.pto", "stitching_canvas_provenance", "left.png", "right.png", "config.yaml"}) {
    const QString source = QDir(game_directory_).filePath(name);
    if (QFileInfo(source).isSymLink() || !QFile::copy(source, temporary_.filePath(name))) {
      previewFailed(
          "Could not copy the saved calibration into the crop preview folder. Check file access and free disk space.");
      return;
    }
  }
  if (RinkLevelingDialog::sourceRevision(temporary_.path()) != source_revision_ ||
      readFile(temporary_.filePath("config.yaml")) != config_contents) {
    previewFailed("The saved calibration changed. Reopen the crop editor.");
    return;
  }
  QMap<QString, QString> fields;
  for (const auto& line : readFile(temporary_.filePath("stitching_canvas_provenance")).split('\n')) {
    const int separator = line.indexOf('=');
    if (separator < 0)
      continue;
    const QString key = QString::fromUtf8(line.left(separator));
    if (fields.contains(key)) {
      previewFailed("Calibration metadata contains duplicate fields.");
      return;
    }
    fields[key] = QString::fromUtf8(line.mid(separator + 1));
  }
  try {
    YAML::Node wrapper, saved = wrapper["stitching"]["projection_framing"];
    for (const auto& key : {"auto-fov", "auto-canvas", "horizontal-fov"}) {
      QString yaml_key(key);
      yaml_key.replace('-', '_');
      const QString value = fields.value("projection-" + QString(key));
      if (value.isEmpty())
        throw std::invalid_argument("Missing framing metadata");
      if (QString(key) == "horizontal-fov") {
        saved[yaml_key.toStdString()] = YAML::Load(value.toStdString());
      } else {
        if (value != "0" && value != "1")
          throw std::invalid_argument("Invalid framing metadata");
        saved[yaml_key.toStdString()] = value == "1";
      }
    }
    for (int index = 0; index < 3; ++index) {
      bool ok = false;
      const double angle = fields.value(QString("projection-rotation-%1").arg(index)).toDouble(&ok);
      if (!ok)
        throw std::invalid_argument("Missing rotation metadata");
      saved["rotation_degrees"].push_back(angle);
    }
    const auto parsed = hm::stitching::read_stitch_projection_framing(wrapper);
    bool h_ok = false, v_ok = false;
    const double horizontal = fields.value("camera-horizontal-fov").toDouble(&h_ok);
    const double vertical = fields.value("camera-vertical-fov").toDouble(&v_ok);
    const QString parameters = projection_parameters_.empty()
        ? "none"
        : QString::fromStdString(hm::stitching::FormatStitchProjectionParameters(projection_parameters_));
    if (!parsed.ok() || withoutCrop(*parsed) != withoutCrop(initial_) || fields.value("mapping-backend") != "nona" ||
        fields.value("projection") != projection_ || fields.value("projection-parameters") != parameters ||
        fields.value("camera-configuration").toStdString() != camera_.configuration || !h_ok || !v_ok ||
        horizontal != camera_.horizontal_fov || vertical != camera_.vertical_fov) {
      previewFailed("Calibrate the current projection and camera settings before previewing the crop.");
      return;
    }
  } catch (const std::exception&) {
    previewFailed("Saved calibration metadata is incomplete. Recalibrate to enable the preview.");
    return;
  }
  const QByteArray pto = readFile(temporary_.filePath("autooptimiser_out.pto"));
  const auto prepared = hm::stitching::PrepareRinkLevelingProject(pto.toStdString());
  QStringList images;
  for (const auto& line : pto.split('\n'))
    if (line.trimmed().startsWith("i "))
      images.push_back(QString::fromUtf8(line));
  if (!prepared.ok() || images.size() != 2 || !images[0].contains(" n\"left.png\"") ||
      !images[1].contains(" n\"right.png\"")) {
    previewFailed("The saved project does not reference the calibrated camera images.");
    return;
  }
  const auto dimensions = QRegularExpression("(?m)^p .*?\\bw(\\d+) .*?\\bh(\\d+)").match(QString::fromUtf8(pto));
  const int width = dimensions.captured(1).toInt(), height = dimensions.captured(2).toInt();
  if (width <= 0 || height <= 0) {
    previewFailed("Saved canvas dimensions are invalid.");
    return;
  }
  // Offline still rendering only, at most 1600 pixels on either axis. It
  // restores the full PTO canvas instead of using an already-cropped snapshot.
  const double scale = std::min(1.0, 1600.0 / std::max(width, height));
  preview_size_ = QSize(std::max(1, int(std::round(width * scale))), std::max(1, int(std::round(height * scale))));
  runTool(
      "pano_modify",
      {QString("--canvas=%1x%2").arg(preview_size_.width()).arg(preview_size_.height()),
       "--crop=0,100,0,100%",
       "-o",
       "full.pto",
       "autooptimiser_out.pto"},
      [this]() {
        runTool("nona", {"-m", "PNG", "--ignore-exposure", "--seam=blend", "-o", "full.png", "full.pto"}, [this]() {
          QImageReader reader(temporary_.filePath("full.png"));
          const QImage image = reader.read();
          if (image.isNull()) {
            previewFailed("The rendered crop preview could not be loaded: " + reader.errorString());
            return;
          }
          if (image.size() != preview_size_) {
            previewFailed("The preview does not cover the full projection canvas.");
            return;
          }
          canvas_->setImage(image);
          preview_ready_ = true;
          status_->setText("Preview of the full projection. Changes to the rectangle update immediately.");
          runTool("pano_modify", {"--crop=AUTO", "-o", "auto.pto", "full.pto"}, [this]() {
            const auto crop = QRegularExpression("(?m)^p .*?\\bS(\\d+),(\\d+),(\\d+),(\\d+)")
                                  .match(QString::fromUtf8(readFile(temporary_.filePath("auto.pto"))));
            if (!crop.hasMatch()) {
              status_->setText("Full preview ready; Auto crop will be calculated during calibration.");
              return;
            }
            for (int index = 0; index < 4; ++index)
              auto_crop_[index] =
                  crop.captured(index + 1).toDouble() / (index < 2 ? preview_size_.width() : preview_size_.height());
            auto_ready_ = auto_crop_[0] >= 0 && auto_crop_[1] <= 1 && auto_crop_[2] >= 0 && auto_crop_[3] <= 1 &&
                auto_crop_[0] < auto_crop_[1] && auto_crop_[2] < auto_crop_[3];
            seedManualFromAuto();
            syncControls();
          });
        });
      });
}

void ProjectionCropDialog::runTool(
    const QString& program,
    const QStringList& arguments,
    std::function<void()> completed) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    previewFailed(program + " is not installed.");
    return;
  }
  auto* process = new QProcess(this);
  process_ = process;
  process->setWorkingDirectory(temporary_.path());
  auto* timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process]() { process->kill(); });
  connect(process, &QProcess::started, this, [process, timeout]() {
    process->closeWriteChannel();
    timeout->start(60000);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process, program](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      process_ = nullptr;
      process->deleteLater();
      previewFailed("Could not start " + program + ".");
    }
  });
  connect(
      process,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this, process, timeout, program, completed](int code, QProcess::ExitStatus status) {
        timeout->stop();
        const QString error = QString::fromUtf8(process->readAllStandardError()).right(500);
        process_ = nullptr;
        process->deleteLater();
        if (code || status != QProcess::NormalExit) {
          previewFailed(program + " failed or timed out. " + error);
          return;
        }
        completed();
      });
  process->start(executable, arguments);
}

void ProjectionCropDialog::stopTool() {
  if (!process_)
    return;
  process_->disconnect(this);
  process_->kill();
  process_->waitForFinished(3000);
  delete process_;
  process_ = nullptr;
}

void ProjectionCropDialog::acceptCrop() {
  if (preview_ready_) {
    const auto lock = hm::stitching::try_lock_canvas_constraint_artifacts(game_directory_.toStdString());
    if (!lock.ok() || !*lock) {
      status_->setText("Calibration is being updated. Cancel and reopen afterward.");
      return;
    }
    const auto config_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(game_directory_.toStdString());
    if (!config_lock.ok() || RinkLevelingDialog::sourceRevision(game_directory_) != source_revision_) {
      status_->setText("Calibration or game settings changed. Cancel and reopen the crop editor.");
      return;
    }
  }
  stopTool();
  accept();
}

void ProjectionCropDialog::reject() {
  stopTool();
  QDialog::reject();
}
