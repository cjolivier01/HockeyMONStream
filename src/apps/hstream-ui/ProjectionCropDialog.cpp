#include "src/apps/hstream-ui/ProjectionCropDialog.h"
#include "src/apps/hstream-ui/ActionIcons.h"

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
#include <memory>
#include <stdexcept>

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/RinkLeveling.h"
#include "src/apps/hstream-ui/RinkLevelingDialog.h"

namespace {
constexpr int kLeft = 1, kRight = 2, kTop = 4, kBottom = 8, kMove = 16;
constexpr int kRendererTimeoutMs = 20 * 60 * 1000;
constexpr int kRendererDiagnosticTailBytes = 64 * 1024;
constexpr int kRendererProgressPartialBytes = 4 * 1024;
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
    QWidget* parent,
    bool in_progress_calibration,
    bool apply_on_accept)
    : QDialog(parent),
      game_directory_(game_directory),
      in_progress_calibration_(in_progress_calibration),
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
  mode_->addItem("Full canvas — no cropping", "full");
  mode_->addItem("Manual", "manual");
  mode_->setCurrentIndex(initial_.auto_crop ? 0 : initial_.crop == kFullCrop ? 1 : 2);
  displayed_mode_ = mode_->currentData().toString();
  layout->addWidget(mode_);
  blend_preview_ = new QCheckBox("Blend preview seams (slower)");
  blend_preview_->setObjectName("projectionCropBlendPreview");
  blend_preview_->setChecked(false);
  blend_preview_->setEnabled(false);
  blend_preview_->setToolTip(
      "Rerender the crop preview with NONA's slower blended seam. The default hard seam renders faster.");
  layout->addWidget(blend_preview_);
  canvas_ = new ProjectionCropCanvas();
  layout->addWidget(canvas_, 1);
  auto* controls = new QGridLayout();
  keep_width_ = new QCheckBox("Keep full width");
  keep_width_->setObjectName("projectionCropKeepWidth");
  keep_width_->setToolTip("Uncheck this in Manual mode to adjust the left and right edges.");
  keep_width_->setChecked(false);
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
      manual_waiting_for_auto_ = false;
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
  auto* note = new QLabel(
      (in_progress_calibration_ || apply_on_accept)
          ? "Use crop saves this choice for the game and continues setup. Full canvas keeps the entire projection."
          : "Use crop returns this selection to the controls. Save Preset applies it to this game.");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Cancel)->setIcon(action_icon(ActionIcon::Cancel));
  if (in_progress_calibration_)
    buttons->button(QDialogButtonBox::Cancel)->setText("Cancel calibration");
  accept_ = buttons->addButton("Use crop", QDialogButtonBox::AcceptRole);
  accept_->setIcon(action_icon(ActionIcon::Apply));
  accept_->setObjectName("acceptProjectionCropButton");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &ProjectionCropDialog::reject);
  connect(accept_, &QPushButton::clicked, this, [this]() { acceptCrop(); });
  connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
    const QString next_mode = mode_->currentData().toString();
    if (next_mode == "manual" && displayed_mode_ != "manual")
      seedManualFromMode(displayed_mode_);
    displayed_mode_ = next_mode;
    syncControls();
  });
  connect(blend_preview_, &QCheckBox::toggled, this, [this]() {
    if (preview_ready_ && !process_)
      renderPreview(false);
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
    manual_waiting_for_auto_ = false;
    syncControls();
  });
  canvas_->cropChanged = [this](const auto& crop) {
    manual_crop_ = crop;
    manual_edited_ = true;
    manual_waiting_for_auto_ = false;
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

void ProjectionCropDialog::seedManualFromMode(const QString& source_mode) {
  if (mode_->currentData() != "manual" || (source_mode != "auto" && source_mode != "full"))
    return;
  manual_crop_ = source_mode == "auto" && auto_ready_ ? auto_crop_ : kFullCrop;
  saved_horizontal_ = {manual_crop_[0], manual_crop_[1]};
  manual_edited_ = false;
  manual_waiting_for_auto_ = source_mode == "auto" && !auto_ready_;
  const QSignalBlocker blocker(keep_width_);
  keep_width_->setChecked(false);
}

void ProjectionCropDialog::seedManualFromAuto() {
  if (mode_->currentData() != "manual" || !manual_waiting_for_auto_ || !auto_ready_ || manual_edited_)
    return;
  manual_crop_ = auto_crop_;
  saved_horizontal_ = {manual_crop_[0], manual_crop_[1]};
  manual_waiting_for_auto_ = false;
  const QSignalBlocker blocker(keep_width_);
  keep_width_->setChecked(false);
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
  if (in_progress_calibration_) {
    source_revision_ = RinkLevelingDialog::inProgressSourceRevision(game_directory_);
    if (!temporary_.isValid() || source_revision_.isEmpty()) {
      previewFailed("The current calibration preview is unavailable.");
      return;
    }
    for (const QString name : {"autooptimiser_out.pto", ".autooptimiser_out.aligned.pto", "left.png", "right.png"}) {
      if (!QFile::copy(QDir(game_directory_).filePath(name), temporary_.filePath(name))) {
        previewFailed("Could not copy the current calibration preview.");
        return;
      }
    }
    if (RinkLevelingDialog::inProgressSourceRevision(temporary_.path()) != source_revision_) {
      previewFailed("Calibration changed while opening the crop editor.");
      return;
    }
  } else {
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
  }
  const QByteArray pto = readFile(temporary_.filePath("autooptimiser_out.pto"));
  const auto prepared = hm::stitching::PrepareRinkLevelingProject(pto.toStdString());
  const auto localized =
      hm::stitching::LocalizeCalibrationPreviewImages(pto.toStdString(), game_directory_.toStdString());
  if (!prepared.ok() || !localized.ok()) {
    previewFailed("The saved project does not reference the calibrated camera images.");
    return;
  }
  QFile preview_project(temporary_.filePath("autooptimiser_out.pto"));
  if (!preview_project.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      preview_project.write(QByteArray::fromStdString(*localized)) != static_cast<qint64>(localized->size()) ||
      !preview_project.flush()) {
    previewFailed("Could not prepare the temporary crop preview project.");
    return;
  }
  preview_project.close();
  geometry_ = hm::stitching::projection_crop_geometry(pto.toStdString(), initial_);
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
      [this]() { renderPreview(true); });
}

void ProjectionCropDialog::renderPreview(bool calculate_auto_crop) {
  if (process_)
    return;
  const bool requested_blend = blend_preview_->isChecked();
  const bool previous_blend = rendered_blend_;
  blend_preview_->setEnabled(false);
  accept_->setEnabled(false);
  status_->setText(requested_blend ? "Rendering blended crop preview…" : "Rendering fast hard-seam crop preview…");
  QFile::remove(temporary_.filePath("full.png"));
  const auto render_failed = [this, previous_blend](const QString& message) {
    const QSignalBlocker blocker(blend_preview_);
    blend_preview_->setChecked(previous_blend);
    blend_preview_->setEnabled(preview_ready_);
    accept_->setEnabled(true);
    if (preview_ready_) {
      status_->setText(message + " Keeping the previous preview.");
    } else {
      previewFailed(message);
    }
  };
  runTool(
      "nona",
      {"-v",
       "-m",
       "PNG",
       "--ignore-exposure",
       requested_blend ? "--seam=blend" : "--seam=hard",
       "-o",
       "full.png",
       "full.pto"},
      [this, calculate_auto_crop, requested_blend, render_failed]() {
        QImageReader reader(temporary_.filePath("full.png"));
        const QImage image = reader.read();
        if (image.isNull()) {
          render_failed("The rendered crop preview could not be loaded: " + reader.errorString());
          return;
        }
        if (image.size() != preview_size_) {
          render_failed("The preview does not cover the full projection canvas.");
          return;
        }
        canvas_->setImage(image);
        preview_ready_ = true;
        rendered_blend_ = requested_blend;
        const auto finish = [this]() {
          blend_preview_->setEnabled(true);
          accept_->setEnabled(true);
          status_->setText("Preview of the full projection. Changes to the rectangle update immediately.");
        };
        if (!calculate_auto_crop) {
          finish();
          return;
        }
        runTool(
            "pano_modify",
            {"--crop=AUTO", "-o", "auto.pto", "full.pto"},
            [this, finish]() {
              const auto crop = QRegularExpression("(?m)^p .*?\\bS(\\d+),(\\d+),(\\d+),(\\d+)")
                                    .match(QString::fromUtf8(readFile(temporary_.filePath("auto.pto"))));
              if (!crop.hasMatch()) {
                blend_preview_->setEnabled(true);
                accept_->setEnabled(true);
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
              finish();
            },
            [this](const QString& message) {
              blend_preview_->setEnabled(true);
              accept_->setEnabled(true);
              status_->setText("Full preview ready; Auto crop could not be previewed. " + message);
            });
      },
      render_failed);
}

void ProjectionCropDialog::runTool(
    const QString& program,
    const QStringList& arguments,
    std::function<void()> completed,
    std::function<void(const QString&)> failed) {
  const QString executable = QStandardPaths::findExecutable(program);
  if (executable.isEmpty()) {
    const QString message = program + " is not installed.";
    if (failed)
      failed(message);
    else
      previewFailed(message);
    return;
  }
  const bool renderer = program == "nona";
  auto* process = new QProcess(this);
  process_ = process;
  process_output_tail_.clear();
  process_progress_partial_.clear();
  process->setWorkingDirectory(temporary_.path());
  if (renderer)
    process->setProcessChannelMode(QProcess::MergedChannels);
  auto* timeout = new QTimer(process);
  auto terminal_handled = std::make_shared<bool>(false);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process]() { process->kill(); });
  connect(process, &QProcess::started, this, [process, timeout, renderer]() {
    process->closeWriteChannel();
    timeout->start(renderer ? kRendererTimeoutMs : 60000);
  });
  auto collect_output = [this, process, renderer]() {
    if (!renderer || process != process_)
      return;
    const QByteArray chunk = process->readAllStandardOutput();
    process_output_tail_ += chunk;
    if (process_output_tail_.size() > kRendererDiagnosticTailBytes)
      process_output_tail_.remove(0, process_output_tail_.size() - kRendererDiagnosticTailBytes);
    QByteArray progress = process_progress_partial_ + chunk;
    progress.replace('\r', '\n');
    const auto lines = progress.split('\n');
    for (auto line = lines.crbegin(); line != lines.crend(); ++line) {
      const QString stage = QString::fromUtf8(*line).trimmed();
      if (!stage.isEmpty()) {
        status_->setText(QString("Rendering crop preview — %1…").arg(stage));
        break;
      }
    }
    const qsizetype newline = progress.lastIndexOf('\n');
    process_progress_partial_ = newline < 0 ? progress : progress.mid(newline + 1);
    if (process_progress_partial_.size() > kRendererProgressPartialBytes)
      process_progress_partial_ = process_progress_partial_.right(kRendererProgressPartialBytes);
  };
  connect(process, &QProcess::readyReadStandardOutput, this, collect_output);
  connect(
      process,
      &QProcess::errorOccurred,
      this,
      [this, process, program, failed, terminal_handled](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
          if (*terminal_handled)
            return;
          *terminal_handled = true;
          process_ = nullptr;
          process->deleteLater();
          const QString message = "Could not start " + program + ".";
          if (failed)
            failed(message);
          else
            previewFailed(message);
        }
      });
  connect(
      process,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this, process, timeout, program, completed, failed, renderer, collect_output, terminal_handled](
          int code, QProcess::ExitStatus status) {
        if (*terminal_handled)
          return;
        *terminal_handled = true;
        timeout->stop();
        collect_output();
        const QString error = renderer ? QString::fromUtf8(process_output_tail_).right(1500)
                                       : QString::fromUtf8(process->readAllStandardError()).right(500);
        process_ = nullptr;
        process->deleteLater();
        if (code || status != QProcess::NormalExit) {
          const QString message = program +
              (renderer ? " failed or exceeded the 20-minute preview limit. " : " failed or timed out. ") + error;
          if (failed)
            failed(message);
          else
            previewFailed(message);
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
  if (in_progress_calibration_) {
    if (!source_revision_.isEmpty() &&
        RinkLevelingDialog::inProgressSourceRevision(game_directory_) != source_revision_) {
      status_->setText("Calibration changed. Cancel calibration and retry.");
      return;
    }
  } else if (preview_ready_) {
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

void ProjectionCropDialog::closeAfterBackendCompletion() {
  backend_completed_ = true;
  reject();
}
