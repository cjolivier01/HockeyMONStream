#include "src/apps/hstream-ui/StitchingExperimentDialog.h"

#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace {

constexpr char kStitchedPreviewOptions[] =
    "pipeline.streammux.batch-size=2,pipeline.streammux.sync-inputs=0,"
    "pipeline.streammux.batched-push-timeout=2147483647,pipeline.streammux.frame-num-reset-on-stream-reset=0,"
    "pipeline.streammux.frame-num-reset-on-eos=0,pipeline.hmstitcher.show=0";

class StitchingExperimentVideoTarget : public QWidget {
 public:
  explicit StitchingExperimentVideoTarget(QWidget* parent) : QWidget(parent) {
    if (QGuiApplication::platformName() == "xcb") {
      setAttribute(Qt::WA_NativeWindow);
      setAttribute(Qt::WA_PaintOnScreen);
      setAttribute(Qt::WA_NoSystemBackground);
    }
    setMinimumSize(640, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background:#101820;");
  }
  QPaintEngine* paintEngine() const override {
    return nullptr;
  }
};

QString format_time(const QTime& time) {
  return time.msec() == 0 ? time.toString("HH:mm:ss") : time.toString("HH:mm:ss.zzz");
}

std::optional<QTime> parse_time(const QString& value) {
  QTime parsed = QTime::fromString(value.trimmed(), "HH:mm:ss.zzz");
  if (!parsed.isValid())
    parsed = QTime::fromString(value.trimmed(), "HH:mm:ss");
  return parsed.isValid() ? std::optional<QTime>(parsed) : std::nullopt;
}

std::optional<std::vector<int>> parse_positive_list(
    const QString& text,
    int minimum,
    int maximum,
    const QString& label,
    QString* error) {
  std::vector<int> values;
  std::set<int> unique;
  for (const QString& item : text.split(',', Qt::SkipEmptyParts)) {
    bool ok = false;
    const int value = item.trimmed().toInt(&ok);
    if (!ok || value < minimum || value > maximum) {
      *error = QString("%1 must be comma-separated integers from %2 through %3, not ‘%4’.")
                   .arg(label)
                   .arg(minimum)
                   .arg(maximum)
                   .arg(item.trimmed());
      return std::nullopt;
    }
    if (unique.insert(value).second)
      values.push_back(value);
  }
  if (values.empty()) {
    *error = "Enter at least one value.";
    return std::nullopt;
  }
  return values;
}

std::optional<std::vector<QString>> parse_time_list(const QString& text, QString* error) {
  std::vector<QString> values;
  std::set<QString> unique;
  for (const QString& item : text.split(',', Qt::SkipEmptyParts)) {
    const auto parsed = parse_time(item);
    if (!parsed.has_value()) {
      *error = QString("Invalid start frame time ‘%1’; use HH:MM:SS or HH:MM:SS.mmm.").arg(item.trimmed());
      return std::nullopt;
    }
    const QString normalized = format_time(*parsed);
    if (unique.insert(normalized).second)
      values.push_back(normalized);
  }
  if (values.empty()) {
    *error = "Enter at least one start frame time.";
    return std::nullopt;
  }
  return values;
}

std::optional<std::vector<std::array<double, 3>>> parse_rotations(const QString& text, QString* error) {
  std::vector<std::array<double, 3>> values;
  for (const QString& item : text.split(',', Qt::SkipEmptyParts)) {
    const QStringList pair = item.trimmed().split('/');
    bool pitch_ok = false;
    bool roll_ok = false;
    const double pitch = pair.value(0).trimmed().toDouble(&pitch_ok);
    const double roll = pair.value(1).trimmed().toDouble(&roll_ok);
    if (pair.size() != 2 || !pitch_ok || !roll_ok || !std::isfinite(pitch) || !std::isfinite(roll) || pitch < -180 ||
        pitch > 180 || roll < -180 || roll > 180) {
      *error = QString("Invalid rink rotation ‘%1’; use pitch/roll pairs such as 0/0,-1.5/0.5.").arg(item.trimmed());
      return std::nullopt;
    }
    const std::array<double, 3> rotation{0.0, pitch, roll};
    if (std::find(values.begin(), values.end(), rotation) == values.end())
      values.push_back(rotation);
  }
  if (values.empty()) {
    *error = "Enter at least one pitch/roll pair.";
    return std::nullopt;
  }
  return values;
}

void interrupt_process(QProcess* process) {
  if (!process || process->state() == QProcess::NotRunning)
    return;
#ifdef Q_OS_UNIX
  const qint64 pid = process->processId();
  if (pid > 0 && ::kill(static_cast<pid_t>(-pid), SIGINT) != 0)
    process->terminate();
#else
  process->terminate();
#endif
}

void kill_process(QProcess* process) {
  if (!process || process->state() == QProcess::NotRunning)
    return;
#ifdef Q_OS_UNIX
  const qint64 pid = process->processId();
  if (pid > 0)
    (void)::kill(static_cast<pid_t>(-pid), SIGKILL);
#endif
  process->kill();
}

} // namespace

struct StitchingExperimentDialog::Impl {
  struct Candidate {
    StitchingExperimentWorkspace workspace;
    int row{-1};
    bool complete{false};
    QString failure;
  };

  StitchingExperimentDialog* dialog;
  QString game_directory;
  QString runner;
  QString working_directory;
  QString pipeline_config;
  QProcessEnvironment environment;
  std::function<void()> selection_applied;
  QLineEdit* control_points{nullptr};
  QLineEdit* frame_counts{nullptr};
  QLineEdit* start_frames{nullptr};
  QCheckBox* shared_rotation{nullptr};
  QLineEdit* rotations{nullptr};
  QPushButton* generate{nullptr};
  QPushButton* cancel{nullptr};
  QTableWidget* table{nullptr};
  StitchingExperimentVideoTarget* video{nullptr};
  QTimeEdit* preview_start{nullptr};
  QSpinBox* preview_duration{nullptr};
  QCheckBox* loop{nullptr};
  QPushButton* preview{nullptr};
  QPushButton* stop_preview{nullptr};
  QPushButton* apply{nullptr};
  QLabel* status{nullptr};
  QProgressBar* progress{nullptr};
  QPlainTextEdit* log{nullptr};
  std::unique_ptr<QTemporaryDir> session;
  std::vector<Candidate> candidates;
  std::unique_ptr<QProcess> calibration_process;
  std::unique_ptr<QProcess> preview_process;
  int running_candidate{-1};
  bool cancelling{false};
  bool stopping_preview{false};
  bool closing{false};
  bool candidate_completion_pending{false};
  bool preview_completion_pending{false};
  bool close_completion_scheduled{false};
  int pending_dialog_result{QDialog::Rejected};

  explicit Impl(StitchingExperimentDialog* owner) : dialog(owner) {}

  QStringList base_arguments(const Candidate& candidate) const {
    return {
        "-g",
        QString::fromStdString(candidate.workspace.game_id),
        "--enable-sources=URI-MULTIPLE",
        "--stitching-calibration-only",
        "-c",
        pipeline_config,
        QString("--options=%1").arg(kStitchedPreviewOptions),
    };
  }

  QProcessEnvironment candidate_environment(const Candidate& candidate) const {
    QProcessEnvironment result = environment;
    result.insert("HM_GAME_DIR", QString::fromStdString(candidate.workspace.root.string()));
    result.insert("HM_MAX_CONTROL_POINTS", QString::number(candidate.workspace.settings.control_points));
    result.insert("HM_STITCH_CALIBRATION_FRAME_COUNT", QString::number(candidate.workspace.settings.frame_count));
    result.insert("HSTREAM_RENDER_AUDIO_MUTED", "1");
    result.insert("USE_NEW_NVSTREAMMUX", "yes");
    return result;
  }

  void show_status(const QString& message, bool error = false) {
    status->setText(message);
    status->setStyleSheet(error ? "color:#b42318;" : "color:#475467;");
  }

  void update_controls() {
    const bool generating = calibration_process && calibration_process->state() != QProcess::NotRunning;
    const bool previewing = preview_process && preview_process->state() != QProcess::NotRunning;
    const int row = table->currentRow();
    const bool selected = row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].complete;
    generate->setEnabled(!generating && !previewing && !closing);
    cancel->setEnabled(generating && !closing);
    table->setEnabled(!generating && !previewing && !closing);
    preview->setEnabled(selected && !generating && !previewing && !closing);
    stop_preview->setEnabled(previewing && !closing);
    apply->setEnabled(selected && !generating && !previewing && !closing);
    shared_rotation->setEnabled(!generating && !closing);
    rotations->setEnabled(!generating && !closing && !shared_rotation->isChecked());
    for (QWidget* input : std::array<QWidget*, 3>{control_points, frame_counts, start_frames})
      input->setEnabled(!generating && !closing);
    progress->setVisible(generating);
  }

  void append_output(QProcess* process) {
    const QString output = QString::fromUtf8(process->readAllStandardOutput());
    if (!output.isEmpty()) {
      log->moveCursor(QTextCursor::End);
      log->insertPlainText(output);
      log->moveCursor(QTextCursor::End);
    }
  }

  void stop_preview_process() {
    if (!preview_process || preview_process->state() == QProcess::NotRunning)
      return;
    stopping_preview = true;
    preview_process->write("q");
    if (!preview_process->waitForBytesWritten(250))
      interrupt_process(preview_process.get());
    schedule_forced_stop(preview_process.get());
  }

  void schedule_forced_stop(QProcess* process) {
    if (!process || process->state() == QProcess::NotRunning)
      return;
    QTimer::singleShot(3000, dialog, [this, process]() {
      const bool still_owned = (calibration_process && calibration_process.get() == process) ||
          (preview_process && preview_process.get() == process);
      if (still_owned && process->state() != QProcess::NotRunning)
        kill_process(process);
    });
  }

  void stop_calibration_process() {
    if (!calibration_process || calibration_process->state() == QProcess::NotRunning)
      return;
    interrupt_process(calibration_process.get());
    schedule_forced_stop(calibration_process.get());
  }

  void maybe_finish_close() {
    if (!closing)
      return;
    const bool calibrating = calibration_process && calibration_process->state() != QProcess::NotRunning;
    const bool previewing = preview_process && preview_process->state() != QProcess::NotRunning;
    if (calibrating || previewing || candidate_completion_pending || preview_completion_pending ||
        close_completion_scheduled)
      return;
    close_completion_scheduled = true;
    const int result = pending_dialog_result;
    QTimer::singleShot(0, dialog, [this, result]() { dialog->QDialog::done(result); });
  }

  void finish_candidate(int row, int exit_code, QProcess::ExitStatus exit_status, const QString& startup_error = {}) {
    if (candidate_completion_pending)
      return;
    candidate_completion_pending = true;
    append_output(calibration_process.get());
    Candidate& finished = candidates[row];
    const absl::Status configured = startup_error.isEmpty() ? ValidateStitchingExperimentWorkspace(finished.workspace)
                                                            : absl::UnavailableError(startup_error.toStdString());
    finished.complete = !cancelling && startup_error.isEmpty() && exit_status == QProcess::NormalExit &&
        exit_code == 0 && configured.ok();
    if (finished.complete) {
      table->item(row, 5)->setText("Ready");
    } else {
      finished.failure = cancelling  ? "Cancelled"
          : !startup_error.isEmpty() ? startup_error
          : configured.ok()          ? QString("Runner exited %1").arg(exit_code)
                                     : QString::fromStdString(configured.ToString());
      table->item(row, 5)->setText(finished.failure);
    }
    QTimer::singleShot(0, dialog, [this]() {
      calibration_process.reset();
      candidate_completion_pending = false;
      launch_next_candidate();
      maybe_finish_close();
    });
  }

  void launch_next_candidate() {
    ++running_candidate;
    if (cancelling || running_candidate >= static_cast<int>(candidates.size())) {
      calibration_process.reset();
      progress->setValue(cancelling ? running_candidate : static_cast<int>(candidates.size()));
      show_status(
          cancelling ? "Candidate generation cancelled. Completed candidates remain available."
                     : "Candidate generation complete. Select a successful row and preview the seam in motion.");
      cancelling = false;
      update_controls();
      return;
    }
    Candidate& candidate = candidates[running_candidate];
    table->item(candidate.row, 5)->setText("Running…");
    table->selectRow(candidate.row);
    progress->setValue(running_candidate);
    show_status(QString("Calibrating candidate %1 of %2…").arg(running_candidate + 1).arg(candidates.size()));

    calibration_process = std::make_unique<QProcess>();
    calibration_process->setProcessChannelMode(QProcess::MergedChannels);
    calibration_process->setWorkingDirectory(working_directory);
    QProcessEnvironment env = candidate_environment(candidate);
    env.insert("HSTREAM_CALIBRATION_PENDING", "1");
    env.insert("HSTREAM_CALIBRATION_START_STAGE", "input");
    env.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", QString::fromStdString(candidate.workspace.invalidation_id));
    calibration_process->setProcessEnvironment(env);
    QStringList args = base_arguments(candidate);
    args
        << "--force-reconfigure"
        << QString("--clean-expected-invalidation-id=%1")
               .arg(QString::fromStdString(candidate.workspace.invalidation_id))
        << "--enable-sinks=FAKE"
        << QString("--options=pipeline.hmstitcher.calibration-frame-count=%1")
               .arg(candidate.workspace.settings.frame_count)
        << QString("--stitch-frame-time=%1").arg(QString::fromStdString(candidate.workspace.settings.stitch_frame_time))
        << "-t=1";
#ifdef Q_OS_UNIX
    if (QFileInfo::exists("/usr/bin/setsid")) {
      args.push_front(runner);
      calibration_process->setProgram("/usr/bin/setsid");
    } else {
      calibration_process->setProgram(runner);
    }
#else
    calibration_process->setProgram(runner);
#endif
    calibration_process->setArguments(args);
    QObject::connect(calibration_process.get(), &QProcess::readyReadStandardOutput, dialog, [this]() {
      append_output(calibration_process.get());
    });
    QObject::connect(
        calibration_process.get(),
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        dialog,
        [this, row = candidate.row](int exit_code, QProcess::ExitStatus exit_status) {
          finish_candidate(row, exit_code, exit_status);
        });
    QObject::connect(
        calibration_process.get(),
        &QProcess::errorOccurred,
        dialog,
        [this, row = candidate.row](QProcess::ProcessError error) {
          if (error == QProcess::FailedToStart)
            finish_candidate(row, -1, QProcess::CrashExit, calibration_process->errorString());
        });
    calibration_process->start();
    update_controls();
  }

  void generate_candidates() {
    QString error;
    auto points = parse_positive_list(control_points->text(), 20, 5000, "Control-point counts", &error);
    auto frames = parse_positive_list(frame_counts->text(), 1, 16, "Frame counts", &error);
    auto starts = parse_time_list(start_frames->text(), &error);
    std::optional<std::vector<std::array<double, 3>>> rink_rotations;
    if (!shared_rotation->isChecked())
      rink_rotations = parse_rotations(rotations->text(), &error);
    if (!points || !frames || !starts || (!shared_rotation->isChecked() && !rink_rotations)) {
      show_status(error, true);
      return;
    }
    const size_t rotation_count = shared_rotation->isChecked() ? 1 : rink_rotations->size();
    const size_t count = points->size() * frames->size() * starts->size() * rotation_count;
    if (count > 64) {
      show_status(QString("This creates %1 candidates; limit the matrix to 64 or fewer.").arg(count), true);
      return;
    }
    stop_preview_process();
    session = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hstream-stitch-experiments-XXXXXX");
    if (!session->isValid()) {
      show_status("Could not create a private stitching experiment directory.", true);
      return;
    }
    candidates.clear();
    table->setRowCount(0);
    int sequence = 0;
    for (int point_count : *points) {
      for (int frame_count : *frames) {
        for (const QString& start : *starts) {
          for (size_t rotation_index = 0; rotation_index < rotation_count; ++rotation_index) {
            StitchingExperimentSettings settings{
                .control_points = point_count,
                .frame_count = frame_count,
                .stitch_frame_time = start.toStdString(),
                .rink_rotation_degrees = shared_rotation->isChecked()
                    ? std::nullopt
                    : std::optional<std::array<double, 3>>(rink_rotations->at(rotation_index)),
            };
            auto workspace = CreateStitchingExperimentWorkspace(
                game_directory.toStdString(), session->path().toStdString(), settings, ++sequence);
            if (!workspace.ok()) {
              show_status(QString::fromStdString(workspace.status().ToString()), true);
              candidates.clear();
              table->setRowCount(0);
              session.reset();
              return;
            }
            const int row = table->rowCount();
            table->insertRow(row);
            const QString rotation = settings.rink_rotation_degrees
                ? QString("%1 / %2")
                      .arg((*settings.rink_rotation_degrees)[1], 0, 'g', 6)
                      .arg((*settings.rink_rotation_degrees)[2], 0, 'g', 6)
                : "Saved setting";
            const QStringList columns = {
                QString("Candidate %1").arg(sequence),
                QString::number(point_count),
                QString::number(frame_count),
                start,
                rotation,
                "Queued",
            };
            for (int column = 0; column < columns.size(); ++column) {
              auto* item = new QTableWidgetItem(columns[column]);
              item->setFlags(item->flags() & ~Qt::ItemIsEditable);
              table->setItem(row, column, item);
            }
            candidates.push_back(Candidate{.workspace = std::move(*workspace), .row = row});
          }
        }
      }
    }
    running_candidate = -1;
    cancelling = false;
    progress->setRange(0, static_cast<int>(candidates.size()));
    progress->setValue(0);
    launch_next_candidate();
  }

  void start_preview() {
    if (preview_process && preview_process->state() != QProcess::NotRunning)
      return;
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].complete)
      return;
    stopping_preview = false;
    preview_completion_pending = false;
    Candidate& candidate = candidates[row];
    preview_process = std::make_unique<QProcess>();
    preview_process->setProcessChannelMode(QProcess::MergedChannels);
    preview_process->setWorkingDirectory(working_directory);
    preview_process->setProcessEnvironment(candidate_environment(candidate));
    QStringList args = base_arguments(candidate);
    args << "--enable-sinks=RENDER"
         << "--show" << QString("--start-time=%1").arg(format_time(preview_start->time()))
         << QString("--time-limit=%1").arg(preview_duration->value())
         << QString("--ui-preview-windows=stitched:%1").arg(static_cast<qulonglong>(video->winId()))
         << "--ui-preview-active=stitched";
#ifdef Q_OS_UNIX
    if (QFileInfo::exists("/usr/bin/setsid")) {
      args.push_front(runner);
      preview_process->setProgram("/usr/bin/setsid");
    } else {
      preview_process->setProgram(runner);
    }
#else
    preview_process->setProgram(runner);
#endif
    preview_process->setArguments(args);
    QObject::connect(preview_process.get(), &QProcess::readyReadStandardOutput, dialog, [this]() {
      append_output(preview_process.get());
    });
    QObject::connect(
        preview_process.get(),
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        dialog,
        [this](int exit_code, QProcess::ExitStatus exit_status) {
          if (preview_completion_pending)
            return;
          preview_completion_pending = true;
          append_output(preview_process.get());
          const bool replay = !stopping_preview && !closing && loop->isChecked() &&
              exit_status == QProcess::NormalExit && exit_code == 0;
          QTimer::singleShot(0, dialog, [this, replay]() {
            preview_process.reset();
            preview_completion_pending = false;
            update_controls();
            if (replay)
              start_preview();
            else
              maybe_finish_close();
          });
        });
    QObject::connect(preview_process.get(), &QProcess::errorOccurred, dialog, [this](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart || preview_completion_pending)
        return;
      preview_completion_pending = true;
      show_status("Could not start the stitched preview: " + preview_process->errorString(), true);
      QTimer::singleShot(0, dialog, [this]() {
        preview_process.reset();
        preview_completion_pending = false;
        update_controls();
        maybe_finish_close();
      });
    });
    show_status(QString("Previewing Candidate %1 without crop or play tracking.").arg(row + 1));
    preview_process->start();
    update_controls();
  }

  void apply_selected() {
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].complete)
      return;
    show_status(QString("Publishing Candidate %1 to the main Program…").arg(row + 1));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const absl::Status promoted = PromoteStitchingExperiment(candidates[row].workspace, game_directory.toStdString());
    QApplication::restoreOverrideCursor();
    if (!promoted.ok()) {
      show_status(QString::fromStdString(promoted.ToString()), true);
      return;
    }
    show_status(
        QString(
            "Candidate %1 selected. Its maps and seam will be used by the next main Program run without recalibration.")
            .arg(row + 1));
    if (selection_applied)
      selection_applied();
    apply->setEnabled(false);
  }

  void shutdown() {
    closing = true;
    cancelling = true;
    stop_preview_process();
    stop_calibration_process();
    update_controls();
    maybe_finish_close();
  }

  void request_close(int result) {
    pending_dialog_result = result;
    shutdown();
  }

  void abort_for_destruction() {
    closing = true;
    cancelling = true;
    kill_process(preview_process.get());
    kill_process(calibration_process.get());
  }
};

StitchingExperimentDialog::StitchingExperimentDialog(
    const QString& game_directory,
    const QString& runner,
    const QString& working_directory,
    const QString& pipeline_config,
    const QProcessEnvironment& environment,
    int control_points,
    int frame_count,
    const QString& stitch_frame_time,
    QWidget* parent,
    std::function<void()> selection_applied)
    : QDialog(parent), impl_(std::make_unique<Impl>(this)) {
  setObjectName("stitchingExperimentDialog");
  setWindowTitle("Stitching Experiments");
  resize(1280, 820);
  auto& s = *impl_;
  s.game_directory = game_directory;
  s.runner = runner;
  s.working_directory = working_directory;
  s.pipeline_config = pipeline_config;
  s.environment = environment;
  s.selection_applied = std::move(selection_applied);

  auto* root = new QVBoxLayout(this);
  auto* intro = new QLabel(
      "Generate independent calibration candidates, replay the same moving passage across each seam, then publish "
      "the selected maps directly to the main Program. Experiments do not run crop, inference, or play tracking.");
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* matrix_group = new QGroupBox("Candidate matrix");
  auto* matrix_layout = new QFormLayout(matrix_group);
  s.control_points = new QLineEdit(QString::number(control_points));
  s.control_points->setObjectName("stitchExperimentControlPoints");
  s.control_points->setPlaceholderText("600,900,1500");
  s.frame_counts = new QLineEdit(QString::number(frame_count));
  s.frame_counts->setObjectName("stitchExperimentFrameCounts");
  s.frame_counts->setPlaceholderText("1,4,8");
  s.start_frames = new QLineEdit(stitch_frame_time);
  s.start_frames->setObjectName("stitchExperimentStartFrames");
  s.start_frames->setPlaceholderText("00:05:00,00:10:00.500");
  s.shared_rotation = new QCheckBox("Use the game’s saved rink leveling for every candidate");
  s.shared_rotation->setObjectName("stitchExperimentSharedRotation");
  s.shared_rotation->setChecked(true);
  s.rotations = new QLineEdit("0/0");
  s.rotations->setObjectName("stitchExperimentRotations");
  s.rotations->setPlaceholderText("pitch/roll pairs: 0/0,-1.5/0.5");
  s.rotations->setEnabled(false);
  connect(s.shared_rotation, &QCheckBox::toggled, this, [&s](bool checked) { s.rotations->setEnabled(!checked); });
  matrix_layout->addRow("Control-point counts", s.control_points);
  matrix_layout->addRow("Frame counts", s.frame_counts);
  matrix_layout->addRow("First calibration frames", s.start_frames);
  matrix_layout->addRow(s.shared_rotation);
  matrix_layout->addRow("Rink pitch/roll variants", s.rotations);
  auto* matrix_actions = new QHBoxLayout();
  s.generate = new QPushButton("Generate candidates");
  s.generate->setObjectName("generateStitchExperimentsButton");
  s.cancel = new QPushButton("Cancel generation");
  s.cancel->setObjectName("cancelStitchExperimentsButton");
  s.cancel->setEnabled(false);
  matrix_actions->addWidget(s.generate);
  matrix_actions->addWidget(s.cancel);
  matrix_actions->addStretch(1);
  matrix_layout->addRow(matrix_actions);
  root->addWidget(matrix_group);

  auto* splitter = new QSplitter(Qt::Horizontal);
  s.table = new QTableWidget(0, 6);
  s.table->setObjectName("stitchExperimentCandidates");
  s.table->setHorizontalHeaderLabels({"Candidate", "CP", "Frames", "First frame", "Rink pitch / roll", "Status"});
  s.table->setSelectionBehavior(QAbstractItemView::SelectRows);
  s.table->setSelectionMode(QAbstractItemView::SingleSelection);
  s.table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  s.table->horizontalHeader()->setStretchLastSection(true);
  splitter->addWidget(s.table);

  auto* preview_panel = new QWidget();
  auto* preview_layout = new QVBoxLayout(preview_panel);
  s.video = new StitchingExperimentVideoTarget(preview_panel);
  s.video->setObjectName("stitchExperimentVideo");
  preview_layout->addWidget(s.video, 1);
  auto* preview_controls = new QHBoxLayout();
  s.preview_start = new QTimeEdit();
  s.preview_start->setObjectName("stitchExperimentPreviewStart");
  s.preview_start->setDisplayFormat("HH:mm:ss.zzz");
  if (const auto parsed = parse_time(stitch_frame_time))
    s.preview_start->setTime(*parsed);
  s.preview_duration = new QSpinBox();
  s.preview_duration->setObjectName("stitchExperimentPreviewDuration");
  s.preview_duration->setRange(2, 120);
  s.preview_duration->setValue(15);
  s.preview_duration->setSuffix(" s");
  s.loop = new QCheckBox("Loop");
  s.loop->setObjectName("stitchExperimentLoop");
  s.loop->setChecked(true);
  s.preview = new QPushButton("Play selected");
  s.preview->setObjectName("previewStitchExperimentButton");
  s.stop_preview = new QPushButton("Stop");
  s.stop_preview->setObjectName("stopStitchExperimentPreviewButton");
  auto* maximize = new QPushButton("Maximize for seam inspection");
  maximize->setObjectName("maximizeStitchExperimentButton");
  preview_controls->addWidget(new QLabel("Passage start"));
  preview_controls->addWidget(s.preview_start);
  preview_controls->addWidget(new QLabel("Duration"));
  preview_controls->addWidget(s.preview_duration);
  preview_controls->addWidget(s.loop);
  preview_controls->addWidget(s.preview);
  preview_controls->addWidget(s.stop_preview);
  preview_controls->addWidget(maximize);
  preview_layout->addLayout(preview_controls);
  splitter->addWidget(preview_panel);
  splitter->setStretchFactor(0, 2);
  splitter->setStretchFactor(1, 3);
  root->addWidget(splitter, 1);

  s.progress = new QProgressBar();
  s.progress->setVisible(false);
  s.status = new QLabel("Choose the candidate combinations to generate.");
  s.status->setObjectName("stitchExperimentStatus");
  s.status->setWordWrap(true);
  s.log = new QPlainTextEdit();
  s.log->setObjectName("stitchExperimentLog");
  s.log->setReadOnly(true);
  s.log->setMaximumBlockCount(4000);
  s.log->setMaximumHeight(130);
  root->addWidget(s.progress);
  root->addWidget(s.status);
  root->addWidget(s.log);
  auto* bottom = new QHBoxLayout();
  s.apply = new QPushButton("Use selected in main Program");
  s.apply->setObjectName("applyStitchExperimentButton");
  auto* close = new QPushButton("Close");
  close->setObjectName("closeStitchExperimentButton");
  bottom->addWidget(s.apply);
  bottom->addStretch(1);
  bottom->addWidget(close);
  root->addLayout(bottom);

  connect(s.generate, &QPushButton::clicked, this, [&s]() { s.generate_candidates(); });
  connect(s.cancel, &QPushButton::clicked, this, [&s]() {
    s.cancelling = true;
    s.show_status("Cancelling candidate generation…");
    s.stop_calibration_process();
  });
  connect(s.table, &QTableWidget::itemSelectionChanged, this, [&s]() { s.update_controls(); });
  connect(s.table, &QTableWidget::cellDoubleClicked, this, [&s](int, int) { s.start_preview(); });
  connect(s.preview, &QPushButton::clicked, this, [&s]() { s.start_preview(); });
  connect(s.stop_preview, &QPushButton::clicked, this, [&s]() {
    s.stop_preview_process();
    s.show_status("Stopping preview…");
  });
  connect(maximize, &QPushButton::clicked, this, [this]() { isMaximized() ? showNormal() : showMaximized(); });
  connect(s.apply, &QPushButton::clicked, this, [&s]() { s.apply_selected(); });
  connect(close, &QPushButton::clicked, this, [this]() { this->close(); });
  s.update_controls();
}

StitchingExperimentDialog::~StitchingExperimentDialog() {
  impl_->abort_for_destruction();
}

void StitchingExperimentDialog::done(int result) {
  impl_->request_close(result);
}

void StitchingExperimentDialog::closeEvent(QCloseEvent* event) {
  event->ignore();
  impl_->request_close(QDialog::Rejected);
}
