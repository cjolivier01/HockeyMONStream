#include "src/apps/hstream-ui/RinkLevelingDialog.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

namespace {
const QStringList kSnapshotFiles =
    {"autooptimiser_out.pto", "stitching_canvas_provenance", "left.png", "right.png", "config.yaml"};
const QStringList kInProgressSnapshotFiles = {
    "autooptimiser_out.pto",
    ".autooptimiser_out.aligned.pto",
    "left.png",
    "right.png"};

QByteArray readFile(const QString& path, qint64 limit = 1024 * 1024) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
    return {};
  return file.readAll();
}

bool writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
      file.flush();
}

QByteArray sourceRevisionForFiles(const QString& directory, const QStringList& files) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (const QString& name : files) {
    QFile file(QDir(directory).filePath(name));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 128 * 1024 * 1024 || !hash.addData(&file))
      return {};
  }
  return hash.result();
}
} // namespace

RinkLevelingDialog::RinkLevelingDialog(
    const QString& game_directory,
    const std::array<double, 3>& current_rotation,
    QWidget* parent,
    std::optional<hm::stitching::StitchCameraSelection> expected_camera,
    bool in_progress_calibration,
    std::optional<hm::stitching::StitchProjection> preview_projection,
    std::vector<double> preview_projection_parameters,
    hm::stitching::StitchProjectionFraming preview_projection_framing)
    : QDialog(parent),
      game_directory_(game_directory),
      initial_rotation_(current_rotation),
      expected_camera_(std::move(expected_camera)),
      in_progress_calibration_(in_progress_calibration),
      preview_projection_(preview_projection),
      preview_projection_parameters_(std::move(preview_projection_parameters)),
      preview_projection_framing_(preview_projection_framing) {
  setObjectName("rinkLevelingDialog");
  setWindowTitle("Level the rink from vertical posts");
  resize(1120, 820);
  estimate_timer_.setSingleShot(true);
  estimate_timer_.setInterval(150);
  connect(&estimate_timer_, &QTimer::timeout, this, [this]() { estimate(); });
  auto* layout = new QVBoxLayout(this);
  auto* instructions = new QLabel(
      "Mark the top and bottom of at least three tall, upright wall or glass posts, spread across both cameras. "
      "Each pair of clicks marks one post. Avoid rink corners, sloping beams and short marks. "
      "Scroll to zoom, drag the image to pan, and drag a numbered point to adjust it.");
  instructions->setWordWrap(true);
  layout->addWidget(instructions);
  tabs_ = new QTabWidget();
  for (size_t camera = 0; camera < canvases_.size(); ++camera) {
    auto* page = new QWidget();
    auto* page_layout = new QVBoxLayout(page);
    canvases_[camera] = new ScoreboardSelectionCanvas();
    canvases_[camera]->setObjectName(QString("rinkLevelingCamera%1").arg(camera));
    canvases_[camera]->setLineSelectionMode();
    canvases_[camera]->selectionChanged = [this]() { selectionChanged(); };
    page_layout->addWidget(canvases_[camera], 1);
    auto* actions = new QHBoxLayout();
    for (const auto& name : {"Undo point", "Clear", "Fit image"}) {
      auto* button = new QPushButton(name);
      button->setObjectName(QString("rinkLevelingCamera%1%2").arg(camera).arg(QString(name).remove(' ')));
      actions->addWidget(button);
      connect(button, &QPushButton::clicked, this, [this, camera, name]() {
        if (QString(name) == "Undo point")
          canvases_[camera]->undoLastPoint();
        else if (QString(name) == "Clear")
          canvases_[camera]->clearPoints();
        else
          canvases_[camera]->fitImage();
      });
    }
    page_layout->addLayout(actions);
    tabs_->addTab(page, camera == 0 ? "Left camera" : "Right camera");
  }
  preview_canvas_ = new ScoreboardSelectionCanvas();
  preview_canvas_->setObjectName("rinkLevelingPreview");
  preview_canvas_->setLineSelectionMode(2);
  tabs_->addTab(preview_canvas_, "Preview");
  tabs_->setTabEnabled(2, false);
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
    if (index < 2)
      QTimer::singleShot(0, this, [this, index]() { canvases_[index]->fitImage(); });
  });
  layout->addWidget(tabs_, 1);
  auto* angles = new QHBoxLayout();
  for (size_t index = 0; index < angle_spins_.size(); ++index) {
    angles->addWidget(new QLabel(index == 0 ? "Pitch" : "Roll"));
    auto* spin = new QDoubleSpinBox();
    spin->setObjectName(index == 0 ? "rinkLevelingPitch" : "rinkLevelingRoll");
    spin->setRange(-180, 180);
    spin->setDecimals(3);
    spin->setSingleStep(0.25);
    spin->setSuffix(QString::fromUtf8("°"));
    spin->setValue(initial_rotation_[index + 1]);
    angle_spins_[index] = spin;
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
      previewed_ = false;
      accept_button_->setEnabled(false);
      tabs_->setTabEnabled(2, false);
    });
    angles->addWidget(spin);
  }
  preview_button_ = new QPushButton("Preview angles");
  preview_button_->setObjectName("previewRinkLevelingButton");
  connect(preview_button_, &QPushButton::clicked, this, [this]() { preview(); });
  angles->addWidget(preview_button_);
  layout->addLayout(angles);
  status_ = new QLabel("The current angles are shown. Mark posts to estimate automatically, then preview the result.");
  status_->setObjectName("rinkLevelingStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  auto* note = new QLabel(
      QString("Preview uses the saved projection and crop. The estimate levels the scene; you can fine-tune pitch "
              "and roll. ") +
      (in_progress_calibration_
           ? "Use angles continues calibration with the result. Skip leveling keeps the configured angles."
           : "Use angles returns the result to the controls. Save Preset applies it to this game. Cancel keeps "
             "existing settings."));
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons =
      new QDialogButtonBox(in_progress_calibration_ ? QDialogButtonBox::NoButton : QDialogButtonBox::Cancel);
  QPushButton* reject_button = in_progress_calibration_
      ? buttons->addButton("Skip leveling", QDialogButtonBox::RejectRole)
      : buttons->button(QDialogButtonBox::Cancel);
  reject_button->setObjectName(in_progress_calibration_ ? "skipRinkLevelingButton" : "cancelRinkLevelingButton");
  if (in_progress_calibration_) {
    auto* cancel_calibration = buttons->addButton("Cancel calibration", QDialogButtonBox::DestructiveRole);
    cancel_calibration->setObjectName("cancelRinkCalibrationButton");
    connect(cancel_calibration, &QPushButton::clicked, this, [this]() { cancelCalibration(); });
  }
  accept_button_ = buttons->addButton("Use angles", QDialogButtonBox::AcceptRole);
  accept_button_->setObjectName("acceptRinkLevelingButton");
  accept_button_->setEnabled(false);
  connect(reject_button, &QPushButton::clicked, this, &RinkLevelingDialog::reject);
  connect(accept_button_, &QPushButton::clicked, this, [this]() { acceptAngles(); });
  layout->addWidget(buttons);
  loadSnapshot();
  QTimer::singleShot(0, this, [this]() { canvases_[0]->fitImage(); });
  if (!load_error_.isEmpty())
    fail(load_error_);
  setBusy(false);
}

RinkLevelingDialog::~RinkLevelingDialog() {
  if (process_) {
    process_->disconnect(this);
    process_->kill();
    process_->waitForFinished(3000);
  }
}

QByteArray RinkLevelingDialog::sourceRevision(const QString& directory) {
  return sourceRevisionForFiles(directory, kSnapshotFiles);
}

QByteArray RinkLevelingDialog::inProgressSourceRevision(const QString& directory) {
  return sourceRevisionForFiles(directory, kInProgressSnapshotFiles);
}

void RinkLevelingDialog::loadSnapshot() {
  auto copy_snapshot = [&](const QStringList& files, const QByteArray& revision) {
    source_revision_ = revision;
    if (!temporary_.isValid() || source_revision_.isEmpty())
      return false;
    for (const QString& name : files) {
      if (!QFile::copy(QDir(game_directory_).filePath(name), temporary_.filePath(name)))
        return false;
    }
    return (in_progress_calibration_ ? inProgressSourceRevision(temporary_.path())
                                     : sourceRevision(temporary_.path())) == source_revision_;
  };

  if (in_progress_calibration_) {
    if (!preview_projection_.has_value()) {
      load_error_ = "The in-progress projection settings are unavailable. Cancel calibration and retry.";
      return;
    }
    if (!copy_snapshot(kInProgressSnapshotFiles, inProgressSourceRevision(game_directory_))) {
      load_error_ = "The in-progress calibration snapshot is unavailable. Skip leveling and retry calibration.";
      return;
    }
    published_rotation_ = initial_rotation_;
  } else {
    const auto lock = hm::stitching::try_lock_canvas_constraint_artifacts(game_directory_.toStdString());
    if (!lock.ok() || !*lock) {
      load_error_ = "Stitching is being updated. Stop playback and try again after calibration finishes.";
      return;
    }
    const auto config_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(game_directory_.toStdString());
    if (!config_lock.ok()) {
      load_error_ = "Game settings are being updated. Try again after the update finishes.";
      return;
    }
    if (!copy_snapshot(kSnapshotFiles, sourceRevision(game_directory_))) {
      load_error_ = "Could not create a stable temporary calibration snapshot. Reopen the dialog.";
      return;
    }
    try {
      const YAML::Node config = YAML::Load(readFile(temporary_.filePath("config.yaml")).toStdString());
      const auto ui = config["hstream_ui"];
      const auto calibration = ui && ui.IsMap() ? ui["stitching_calibration"] : YAML::Node();
      const auto status = calibration && calibration.IsMap() ? calibration["status"] : YAML::Node();
      if (status && !status.IsNull() && (!status.IsScalar() || status.as<std::string>() != "complete")) {
        load_error_ =
            "Stitching calibration is pending or incomplete. Finish calibration with the current camera and reference-frame settings first.";
        return;
      }
    } catch (const YAML::Exception&) {
      load_error_ = "Could not read the game's calibration settings. Repair the config and recalibrate first.";
      return;
    }
  }
  const auto prepared =
      hm::stitching::PrepareRinkLevelingProject(readFile(temporary_.filePath("autooptimiser_out.pto")).toStdString());
  if (!prepared.ok() || prepared->image_sizes.size() != 2) {
    load_error_ = prepared.ok() ? "This selector requires two calibrated cameras."
                                : QString::fromStdString(prepared.status().ToString());
    return;
  }
  project_ = *prepared;
  // Saved HStream PTOs use these ordered image names. Refuse a project that
  // would make the renderer read different source images from the snapshot.
  QStringList image_records;
  for (const auto& line : QByteArray::fromStdString(project_.pto).split('\n'))
    if (line.trimmed().startsWith("i "))
      image_records.push_back(QString::fromUtf8(line));
  if (image_records.size() != 2 || !image_records[0].contains(" n\"left.png\"") ||
      !image_records[1].contains(" n\"right.png\"")) {
    load_error_ = "The saved project does not reference the expected left and right camera images. Recalibrate first.";
    return;
  }
  if (!in_progress_calibration_) {
    QMap<QString, QString> fields;
    for (const QByteArray& line : readFile(temporary_.filePath("stitching_canvas_provenance")).split('\n')) {
      const int separator = line.indexOf('=');
      if (separator < 0)
        continue;
      const QString key = QString::fromUtf8(line.left(separator));
      if (fields.contains(key)) {
        load_error_ = "The saved calibration metadata has duplicate fields. Recalibrate before leveling.";
        return;
      }
      fields[key] = QString::fromUtf8(line.mid(separator + 1));
    }
    const int version = fields.value("version").toInt();
    if (version < 2 || version > 8 || fields.value("mapping-backend") != "nona") {
      load_error_ = "Leveling requires saved NONA calibration metadata. Run stitching calibration first.";
      return;
    }
    if (expected_camera_ && version < 7) {
      load_error_ =
          "This older calibration has no camera-model metadata. Recalibrate once with the current camera settings before selecting posts.";
      return;
    }
    if (expected_camera_) {
      bool horizontal_ok = false, vertical_ok = false;
      const double horizontal = fields.value("camera-horizontal-fov").toDouble(&horizontal_ok);
      const double vertical = fields.value("camera-vertical-fov").toDouble(&vertical_ok);
      if (!horizontal_ok || !vertical_ok ||
          fields.value("camera-configuration").toStdString() != expected_camera_->configuration ||
          horizontal != expected_camera_->horizontal_fov || vertical != expected_camera_->vertical_fov) {
        load_error_ = "The camera model or FOV differs from the saved calibration. Recalibrate before selecting posts.";
        return;
      }
    }
    // Versions 2-7 predate the shared camera-space rotation and imply zero.
    for (size_t index = 0; index < published_rotation_.size(); ++index) {
      const QString key = QString("projection-rotation-%1").arg(index);
      if (version < 8 && !fields.contains(key))
        continue;
      bool ok = false;
      published_rotation_[index] = fields.value(key).toDouble(&ok);
      if (!ok || !std::isfinite(published_rotation_[index]) || std::abs(published_rotation_[index]) > 180) {
        load_error_ = "The saved calibration rotation is invalid. Recalibrate before leveling.";
        return;
      }
    }
  }
  if (!writeFile(temporary_.filePath("sphere.pto"), QByteArray::fromStdString(project_.pto))) {
    load_error_ = "Could not prepare the calibration project.";
    return;
  }
  for (size_t camera = 0; camera < canvases_.size(); ++camera) {
    if (!canvases_[camera]->setImage(temporary_.filePath(camera == 0 ? "left.png" : "right.png")) ||
        canvases_[camera]->imageSize() != QSize(project_.image_sizes[camera][0], project_.image_sizes[camera][1])) {
      load_error_ = "Saved camera image dimensions do not match the calibration. Run stitching calibration again.";
      return;
    }
  }
}

std::array<double, 3> RinkLevelingDialog::rotationDegrees() const {
  return {initial_rotation_[0], angle_spins_[0]->value(), angle_spins_[1]->value()};
}

void RinkLevelingDialog::setBusy(bool busy) {
  busy_ = busy;
  const bool enabled = !busy && load_error_.isEmpty();
  tabs_->setEnabled(enabled);
  for (auto* canvas : canvases_)
    canvas->setEnabled(enabled);
  for (auto* spin : angle_spins_)
    spin->setEnabled(enabled);
  preview_button_->setEnabled(enabled);
  accept_button_->setEnabled(enabled && previewed_);
}

void RinkLevelingDialog::fail(const QString& message) {
  setBusy(false);
  status_->setText(message);
}

void RinkLevelingDialog::selectionChanged() {
  estimated_ = false;
  previewed_ = false;
  if (!accept_button_)
    return;
  accept_button_->setEnabled(false);
  tabs_->setTabEnabled(2, false);
  const auto count = canvases_[0]->points().size() / 2 + canvases_[1]->points().size() / 2;
  const bool complete_pairs = std::all_of(canvases_.begin(), canvases_.end(), [](const auto* canvas) {
    return canvas->points().size() >= 2 && canvas->points().size() % 2 == 0;
  });
  if (count < 3 || !complete_pairs) {
    estimate_timer_.stop();
    preview_button_->setEnabled(!busy_ && load_error_.isEmpty());
    status_->setText(QString("%1 complete posts selected. Use at least three, spread across both cameras.").arg(count));
    return;
  }
  status_->setText("Updating pitch and roll from the selected posts…");
  preview_button_->setEnabled(false);
  estimate_timer_.start();
}

void RinkLevelingDialog::startTool(
    const QString& program,
    const QStringList& arguments,
    const QByteArray& input,
    std::function<void(const QByteArray&)> completed) {
  static const QMap<QString, QString> overrides = {
      {"pano_trafo", "HM_PANO_TRAFO"},
      {"pano_modify", "HM_PANO_MODIFY"},
      {"nona", "HM_NONA"},
  };
  const auto override = overrides.find(program);
  if (override == overrides.end()) {
    fail(QString("Unsupported rink leveling tool: %1").arg(program));
    return;
  }
  const auto resolved =
      hm::stitching::HuginProject::ResolveExecutable(override.value().toStdString(), program.toStdString());
  if (!resolved.ok()) {
    fail(QString::fromStdString(resolved.status().ToString()));
    return;
  }
  const QString executable = QString::fromStdString(*resolved);
  setBusy(true);
  auto* process = new QProcess(this);
  process_ = process;
  process->setWorkingDirectory(temporary_.path());
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert("LC_ALL", "C");
  process->setProcessEnvironment(environment);
  auto* timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process]() { process->kill(); });
  connect(process, &QProcess::started, this, [process, input, timeout]() {
    process->write(input);
    process->closeWriteChannel();
    timeout->start(60000);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process, program](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      process_ = nullptr;
      process->deleteLater();
      fail(QString("Could not start %1: %2").arg(program, process->errorString()));
    }
  });
  connect(
      process,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this, process, program, completed, timeout](int code, QProcess::ExitStatus status) {
        timeout->stop();
        const QByteArray output = process->readAllStandardOutput();
        const QString error = QString::fromUtf8(process->readAllStandardError()).right(1500);
        process_ = nullptr;
        process->deleteLater();
        if (code != 0 || status != QProcess::NormalExit) {
          fail(QString("%1 failed or timed out. %2").arg(program, error));
          return;
        }
        setBusy(false);
        completed(output);
      });
  process->start(executable, arguments);
}

void RinkLevelingDialog::estimate() {
  if (busy_ || !load_error_.isEmpty())
    return;
  std::vector<hm::stitching::RinkLevelingLine> lines;
  for (size_t camera = 0; camera < canvases_.size(); ++camera) {
    const auto& points = canvases_[camera]->points();
    if (points.size() < 2 || points.size() % 2) {
      fail("Mark complete top/bottom pairs in both camera images.");
      return;
    }
    for (qsizetype index = 0; index < points.size(); index += 2)
      lines.push_back(
          {camera,
           {double(points[index].x()), double(points[index].y())},
           {double(points[index + 1].x()), double(points[index + 1].y())}});
  }
  const auto input = hm::stitching::FormatRinkLevelingPoints(lines, project_.image_sizes);
  if (!input.ok()) {
    fail(QString::fromStdString(input.status().ToString()));
    return;
  }
  status_->setText("Estimating pitch and roll from the calibrated viewing rays…");
  startTool(
      "pano_trafo",
      {"sphere.pto"},
      QByteArray::fromStdString(*input),
      [this, count = lines.size()](const QByteArray& output) {
        const auto rays = hm::stitching::ParseRinkLevelingRays(output.toStdString(), count);
        if (!rays.ok()) {
          fail(QString::fromStdString(rays.status().ToString()));
          return;
        }
        const auto result = hm::stitching::EstimateRinkLeveling(*rays, published_rotation_, initial_rotation_[0]);
        if (!result.ok()) {
          fail(QString::fromStdString(result.status().ToString()));
          return;
        }
        for (size_t index = 0; index < angle_spins_.size(); ++index)
          angle_spins_[index]->setValue(result->rotation_degrees[index + 1]);
        estimated_ = true;
        status_->setText(
            QString("Used %1 of %2 posts; RMS angular residual %3°. Preview the result, then fine-tune if needed.")
                .arg(result->inlier_indices.size())
                .arg(count)
                .arg(result->rms_residual_degrees, 0, 'f', 2));
      });
}

void RinkLevelingDialog::preview() {
  if (busy_ || !load_error_.isEmpty())
    return;
  previewed_ = false;
  accept_button_->setEnabled(false);
  status_->setText("Rendering a temporary still preview…");
  auto render_preview = [this](const QString& framed_project) {
    const auto canvas =
        hm::stitching::HuginProject::ParseCanvasSize(readFile(temporary_.filePath(framed_project)).toStdString());
    if (!canvas.ok() || canvas->first == 0 || canvas->second == 0 || canvas->second > canvas->first * 4ULL) {
      fail(
          canvas.ok() ? "Saved preview canvas dimensions are invalid."
                      : QString::fromStdString(canvas.status().ToString()));
      return;
    }
    const size_t preview_width = std::min<size_t>(1600, canvas->first);
    const size_t preview_height =
        std::max<size_t>(1, size_t(std::llround(double(canvas->second) * preview_width / canvas->first)));
    startTool(
        "pano_modify",
        {QString("--canvas=%1x%2").arg(preview_width).arg(preview_height), "--output=preview.pto", framed_project},
        {},
        [this](const QByteArray&) {
          QFile::remove(temporary_.filePath("preview.png"));
          startTool(
              "nona",
              {"-m", "PNG", "--ignore-exposure", "--seam=blend", "-o", "preview.png", "preview.pto"},
              {},
              [this](const QByteArray&) {
                if (!preview_canvas_->setImage(temporary_.filePath("preview.png"))) {
                  fail("Hugin did not produce a readable preview.");
                  return;
                }
                preview_canvas_->fitImage();
                previewed_ = true;
                tabs_->setTabEnabled(2, true);
                tabs_->setCurrentIndex(2);
                accept_button_->setEnabled(true);
                status_->setText(
                    estimated_ ? "Inspect the walls and both ends of the rink. Use angles when satisfied."
                               : "Preview of the displayed angles. Add or adjust posts to estimate automatically.");
              });
        });
  };

  if (in_progress_calibration_) {
    auto framing = preview_projection_framing_;
    framing.rotation_degrees = rotationDegrees();
    const auto arguments = hm::stitching::HuginProject::ProjectionPanoModifyArguments(
        *preview_projection_,
        preview_projection_parameters_,
        framing,
        "preview-framed.pto",
        ".autooptimiser_out.aligned.pto");
    if (!arguments.ok()) {
      fail(QString::fromStdString(arguments.status().ToString()));
      return;
    }
    QStringList qt_arguments;
    for (const std::string& argument : *arguments)
      qt_arguments.push_back(QString::fromStdString(argument));
    startTool(
        "pano_modify", qt_arguments, {}, [render_preview](const QByteArray&) { render_preview("preview-framed.pto"); });
    return;
  }

  const auto delta = hm::stitching::RinkLevelingRotationDelta(published_rotation_, rotationDegrees());
  if (!delta.ok()) {
    fail(QString::fromStdString(delta.status().ToString()));
    return;
  }
  const QString rotation = QString("--rotate=%1,%2,%3")
                               .arg((*delta)[0], 0, 'g', 16)
                               .arg((*delta)[1], 0, 'g', 16)
                               .arg((*delta)[2], 0, 'g', 16);
  startTool(
      "pano_modify",
      {rotation, "--output=preview-framed.pto", "autooptimiser_out.pto"},
      {},
      [render_preview](const QByteArray&) { render_preview("preview-framed.pto"); });
}

void RinkLevelingDialog::acceptAngles() {
  if (busy_ || !previewed_)
    return;
  if (in_progress_calibration_) {
    if (inProgressSourceRevision(game_directory_) != source_revision_) {
      fail("The in-progress calibration changed. Skip leveling and retry calibration.");
      accept_button_->setEnabled(false);
      return;
    }
    accept();
    return;
  }
  const auto lock = hm::stitching::try_lock_canvas_constraint_artifacts(game_directory_.toStdString());
  if (!lock.ok() || !*lock) {
    fail("The stitching calibration is being updated. Cancel and reopen after it finishes.");
    return;
  }
  const auto config_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(game_directory_.toStdString());
  if (!config_lock.ok() || sourceRevision(game_directory_) != source_revision_) {
    fail("The stitching calibration changed. Cancel and reopen the dialog before applying angles.");
    accept_button_->setEnabled(false);
    return;
  }
  accept();
}

void RinkLevelingDialog::reject() {
  if (process_) {
    process_->disconnect(this);
    process_->kill();
    process_->waitForFinished(3000);
    process_->deleteLater();
    process_ = nullptr;
  }
  QDialog::reject();
}

void RinkLevelingDialog::cancelCalibration() {
  calibration_cancellation_requested_ = true;
  reject();
}

void RinkLevelingDialog::closeAfterBackendCompletion() {
  closed_after_backend_completion_ = true;
  reject();
}
