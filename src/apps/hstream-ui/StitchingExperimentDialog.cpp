#include "src/apps/hstream-ui/StitchingExperimentDialog.h"

#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QImageReader>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
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
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/syscall.h>
#include <unistd.h>
#endif

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
    setMinimumSize(320, 180);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    setAutoFillBackground(false);
  }
  QPaintEngine* paintEngine() const override {
    return nullptr;
  }

  std::function<void()> toggle_focus;

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && toggle_focus) {
      toggle_focus();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }
};

class SelectedFrameImage : public QLabel {
 public:
  explicit SelectedFrameImage(QWidget* parent) : QLabel(parent) {
    setAlignment(Qt::AlignCenter);
    setMinimumSize(180, 160);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setWordWrap(true);
  }

  void load(const std::filesystem::path& path) {
    original_ = {};
    clear();
    const QString file = QString::fromStdString(path.string());
    const QFileInfo info(file);
    if (!info.isFile()) {
      setText("Thumbnail becomes available when this pair is extracted for calibration.");
      return;
    }
    QImageReader reader(file);
    const QSize size = reader.size();
    if (info.isSymLink() || info.size() > 5 * 1024 * 1024 || !size.isValid() || size.width() > 1024 ||
        size.height() > 1024) {
      setText("Thumbnail exceeds the inspection limits.");
      return;
    }
    original_ = QPixmap::fromImage(reader.read());
    if (original_.isNull())
      setText("Thumbnail could not be read.");
    else
      update_image();
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QLabel::resizeEvent(event);
    update_image();
  }

 private:
  void update_image() {
    if (!original_.isNull())
      setPixmap(original_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
  QPixmap original_;
};

class SelectedFrameCoverage : public QWidget {
 public:
  explicit SelectedFrameCoverage(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 135);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  void set_coverage(const std::vector<uint16_t>& cells) {
    cells_ = cells;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    constexpr std::array<Qt::GlobalColor, 3> colors{Qt::blue, Qt::darkGreen, Qt::darkYellow};
    const double cell_width = static_cast<double>(width()) / 16;
    const double cell_height = static_cast<double>(height()) / 9;
    painter.fillRect(rect(), QColor("#f3f4f6"));
    for (const uint16_t value : cells_) {
      if (value >= 3 * 16 * 9)
        continue;
      const unsigned band = value / (16 * 9);
      const unsigned cell = value % (16 * 9);
      painter.fillRect(
          QRectF(
              (cell % 16 + static_cast<double>(band) / 3) * cell_width,
              cell / 16 * cell_height,
              cell_width / 3,
              cell_height),
          colors[band]);
    }
    painter.setPen(QColor("#d1d5db"));
    for (int column = 0; column <= 16; ++column)
      painter.drawLine(QPointF(column * cell_width, 0), QPointF(column * cell_width, height()));
    for (int row = 0; row <= 9; ++row)
      painter.drawLine(QPointF(0, row * cell_height), QPointF(width(), row * cell_height));
  }

 private:
  std::vector<uint16_t> cells_;
};

QString exact_frame_time(uint64_t nanoseconds) {
  const uint64_t seconds = nanoseconds / 1000000000ULL;
  return QString("%1:%2:%3.%4")
      .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
      .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
      .arg(seconds % 60, 2, 10, QLatin1Char('0'))
      .arg(nanoseconds % 1000000000ULL, 9, 10, QLatin1Char('0'));
}

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

struct ExperimentSessionInspection {
  std::vector<int> pidfds;
  bool unverified{false};

  ~ExperimentSessionInspection() {
#ifdef Q_OS_UNIX
    for (int descriptor : pidfds)
      ::close(descriptor);
#endif
  }
  ExperimentSessionInspection() = default;
  ExperimentSessionInspection(ExperimentSessionInspection&& other) noexcept : unverified(other.unverified) {
    pidfds.swap(other.pidfds);
  }
  ExperimentSessionInspection& operator=(ExperimentSessionInspection&&) = delete;
  ExperimentSessionInspection(const ExperimentSessionInspection&) = delete;
  ExperimentSessionInspection& operator=(const ExperimentSessionInspection&) = delete;
};

ExperimentSessionInspection inspect_experiment_session(qint64 session_id, const QString& token) {
  ExperimentSessionInspection inspection;
  if (session_id == 0 && token.isEmpty())
    return inspection;
#if defined(Q_OS_UNIX) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  if (session_id <= 0 || token.isEmpty()) {
    inspection.unverified = true;
    return inspection;
  }
  QDir proc("/proc");
  if (!proc.exists() || !proc.isReadable()) {
    inspection.unverified = true;
    return inspection;
  }
  const QByteArray expected = "HSTREAM_EXPERIMENT_PROCESS_TOKEN=" + token.toUtf8();
  const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
  // Our own PID must be visible even when there are no experiment processes.
  if (!entries.contains(QString::number(::getpid())))
    inspection.unverified = true;
  for (const QString& entry : entries) {
    bool numeric = false;
    const qint64 parsed = entry.toLongLong(&numeric);
    if (!numeric || parsed <= 0)
      continue;
    const pid_t pid = static_cast<pid_t>(parsed);
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pidfd < 0) {
      if (errno != ESRCH)
        inspection.unverified = true;
      continue;
    }
    const pid_t observed_session = ::getsid(pid);
    if (observed_session != static_cast<pid_t>(session_id)) {
      if (observed_session < 0 && errno != ESRCH)
        inspection.unverified = true;
      ::close(pidfd);
      continue;
    }
    QFile environment(QString("/proc/%1/environ").arg(entry));
    if (!environment.open(QIODevice::ReadOnly)) {
      if (::syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) == 0 || errno != ESRCH)
        inspection.unverified = true;
      ::close(pidfd);
      continue;
    }
    const QList<QByteArray> variables = environment.readAll().split('\0');
    if (::syscall(SYS_pidfd_send_signal, pidfd, 0, nullptr, 0) != 0) {
      if (errno != ESRCH)
        inspection.unverified = true;
      ::close(pidfd);
      continue;
    }
    if (environment.error() != QFileDevice::NoError) {
      inspection.unverified = true;
      ::close(pidfd);
      continue;
    }
    if (!variables.contains(expected)) {
      ::close(pidfd);
      continue;
    }
    inspection.pidfds.push_back(pidfd);
  }
#else
  (void)session_id;
  (void)token;
  inspection.unverified = session_id > 0 || !token.isEmpty();
#endif
  return inspection;
}

bool experiment_session_alive(qint64 session_id, const QString& token) {
  ExperimentSessionInspection inspection = inspect_experiment_session(session_id, token);
  return inspection.unverified || !inspection.pidfds.empty();
}

bool signal_experiment_session(qint64 session_id, const QString& token, int signal) {
  ExperimentSessionInspection inspection = inspect_experiment_session(session_id, token);
  bool signalled = false;
  for (int pidfd : inspection.pidfds) {
#if defined(Q_OS_UNIX) && defined(SYS_pidfd_send_signal)
    signalled = ::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0) == 0 || signalled;
#else
    (void)pidfd;
    (void)signal;
#endif
  }
  return signalled;
}

void interrupt_process(QProcess* process, qint64 session_id, const QString& token) {
  const bool signalled = signal_experiment_session(session_id, token, SIGINT);
  if (!signalled && process && process->state() != QProcess::NotRunning)
    process->terminate();
}

void kill_process(QProcess* process, qint64 session_id, const QString& token) {
  (void)signal_experiment_session(session_id, token, SIGKILL);
  if (process && process->state() != QProcess::NotRunning)
    process->kill();
}

} // namespace

struct StitchingExperimentDialog::Impl {
  enum class CandidatePhase { kCalibration, kScan };

  struct Candidate {
    StitchingExperimentSettings settings;
    std::optional<StitchingExperimentWorkspace> workspace;
    int sequence{0};
    int row{-1};
    int baseline_sequence{0};
    int scan_duration_seconds{60};
    bool requires_ice_mask{false};
    CandidatePhase phase{CandidatePhase::kCalibration};
    bool has_selected_frames{false};
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
  QCheckBox* prefer_player_frames{nullptr};
  QSpinBox* scan_duration{nullptr};
  QPushButton* add_to_batch{nullptr};
  QPushButton* remove_from_batch{nullptr};
  QPushButton* clear_batch{nullptr};
  QPushButton* start_batch{nullptr};
  QPushButton* cancel{nullptr};
  QTableWidget* table{nullptr};
  StitchingExperimentVideoTarget* video{nullptr};
  QTimeEdit* preview_start{nullptr};
  QSpinBox* preview_duration{nullptr};
  QCheckBox* loop{nullptr};
  QPushButton* preview{nullptr};
  QPushButton* stop_preview{nullptr};
  QPushButton* expand_preview{nullptr};
  QSplitter* splitter{nullptr};
  std::vector<QWidget*> preview_focus_siblings;
  QList<int> normal_splitter_sizes;
  bool preview_focused{false};
  QPushButton* apply{nullptr};
  QPushButton* inspect_frames{nullptr};
  QLabel* status{nullptr};
  QProgressBar* progress{nullptr};
  QPlainTextEdit* log{nullptr};
  std::unique_ptr<QTemporaryDir> session;
  std::vector<Candidate> candidates;
  std::unique_ptr<QProcess> calibration_process;
  std::unique_ptr<QProcess> preview_process;
  QThread* promotion_worker{nullptr};
  qint64 calibration_process_group{0};
  qint64 preview_process_group{0};
  QString calibration_process_token;
  QString preview_process_token;
  std::optional<uint64_t> calibration_shutdown_ticket;
  std::optional<uint64_t> preview_shutdown_ticket;
  uint64_t next_shutdown_ticket{0};
  int pending_group_shutdowns{0};
  int next_candidate_sequence{0};
  int running_candidate{-1};
  bool batch_started{false};
  bool batch_active{false};
  bool cancelling{false};
  bool stopping_preview{false};
  bool closing{false};
  bool candidate_completion_pending{false};
  bool preview_completion_pending{false};
  bool preview_replay_pending{false};
  bool current_candidate_workspace_unsafe{false};
  bool session_retained{false};
  int pending_candidate_row{-1};
  uint64_t process_generation{0};
  int pending_candidate_exit_code{-1};
  QProcess::ExitStatus pending_candidate_exit_status{QProcess::CrashExit};
  QString pending_candidate_startup_error;
  int preview_candidate_row{-1};
  bool close_completion_scheduled{false};
  int pending_dialog_result{QDialog::Rejected};

  explicit Impl(StitchingExperimentDialog* owner) : dialog(owner) {}

  void set_preview_focus(bool focused) {
    if (focused == preview_focused)
      return;
    // Keep the native target and its XID intact. Unmap it only while the
    // surrounding layout settles, just as the main UI's preview focus does.
    const bool remap_video = video->isVisible();
    if (remap_video)
      video->hide();
    if (focused)
      normal_splitter_sizes = splitter->sizes();
    preview_focused = focused;
    for (QWidget* sibling : preview_focus_siblings)
      sibling->setVisible(!focused);
    expand_preview->setText(focused ? "Restore layout" : "Expand preview");
    expand_preview->setToolTip(
        focused ? "Restore the candidate controls (Escape or double-click the preview)."
                : "Expand the preview within this dialog (or double-click the preview).");
    dialog->layout()->activate();
    if (!focused)
      splitter->setSizes(normal_splitter_sizes);
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (remap_video) {
      video->show();
      video->raise();
    }
  }

  QStringList base_arguments(const Candidate& candidate) const {
    return {
        "-g",
        QString::fromStdString(candidate.workspace->game_id),
        "--enable-sources=URI-MULTIPLE",
        "--stitching-calibration-only",
        "-c",
        pipeline_config,
        QString("--options=%1").arg(kStitchedPreviewOptions),
    };
  }

  QProcessEnvironment candidate_environment(const Candidate& candidate) const {
    QProcessEnvironment result = environment;
    // These belong only to a particular calibration process. Inheriting them
    // into a scan or preview can trigger cleanup or a calibration rewind.
    for (const char* name :
         {"HSTREAM_CALIBRATION_PENDING", "HSTREAM_CALIBRATION_START_STAGE", "HSTREAM_CALIBRATION_INVALIDATION_ID"})
      result.remove(name);
    result.insert("HM_GAME_DIR", QString::fromStdString(candidate.workspace->root.string()));
    result.insert("HM_MAX_CONTROL_POINTS", QString::number(candidate.settings.control_points));
    result.insert("HM_STITCH_CALIBRATION_FRAME_COUNT", QString::number(candidate.settings.frame_count));
    result.insert("HSTREAM_RENDER_AUDIO_MUTED", "1");
    result.insert("USE_NEW_NVSTREAMMUX", "yes");
    return result;
  }

  void show_status(const QString& message, bool error = false) {
    status->setText(message);
    status->setStyleSheet(error ? "color:#b42318;" : "color:#475467;");
  }

  void update_controls() {
    const bool previewing = preview_process && preview_process->state() != QProcess::NotRunning;
    const bool promoting = promotion_worker != nullptr;
    const bool stopping = pending_group_shutdowns > 0;
    const int row = table->currentRow();
    const bool selected = row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].complete;
    const bool selected_queued = row >= 0 && row < static_cast<int>(candidates.size()) && !batch_started;
    const bool editing_batch = !batch_active && !batch_started && !previewing && !promoting && !stopping && !closing;
    add_to_batch->setEnabled(editing_batch && candidates.size() < 64);
    remove_from_batch->setEnabled(editing_batch && selected_queued);
    clear_batch->setEnabled(!batch_active && !previewing && !promoting && !stopping && !closing && !candidates.empty());
    start_batch->setEnabled(editing_batch && !candidates.empty());
    cancel->setEnabled(batch_active && !closing);
    table->setEnabled(!batch_active && !previewing && !promoting && !closing);
    preview->setEnabled(selected && !batch_active && !previewing && !promoting && !stopping && !closing);
    stop_preview->setEnabled(previewing && !closing);
    apply->setEnabled(selected && !batch_active && !previewing && !promoting && !closing);
    inspect_frames->setEnabled(
        row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].has_selected_frames && !batch_active &&
        !previewing && !promoting && !stopping && !closing);
    shared_rotation->setEnabled(editing_batch);
    rotations->setEnabled(editing_batch && !shared_rotation->isChecked());
    prefer_player_frames->setEnabled(editing_batch);
    scan_duration->setEnabled(editing_batch && prefer_player_frames->isChecked());
    for (QWidget* input : std::array<QWidget*, 3>{control_points, frame_counts, start_frames})
      input->setEnabled(editing_batch);
    progress->setVisible(batch_started);
  }

  void append_output(QProcess* process) {
    const QString output = QString::fromUtf8(process->readAllStandardOutput());
    if (!output.isEmpty()) {
      log->moveCursor(QTextCursor::End);
      log->insertPlainText(output);
      log->moveCursor(QTextCursor::End);
    }
  }

  void append_output_message(const QString& message) {
    log->moveCursor(QTextCursor::End);
    log->insertPlainText(message);
    log->moveCursor(QTextCursor::End);
  }

  Candidate* baseline_for(const Candidate& candidate) {
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& baseline) {
      return baseline.sequence == candidate.baseline_sequence && baseline.baseline_sequence == 0;
    });
    return found == candidates.end() ? nullptr : &*found;
  }

  std::filesystem::path scan_report_path(const Candidate& candidate) const {
    return std::filesystem::path(session->path().toStdString()) /
        ("player-scan-" + std::to_string(candidate.sequence) + ".yaml");
  }

  void finalize_candidate_completion() {
    if (!candidate_completion_pending)
      return;
    const uint64_t generation = process_generation;
    QTimer::singleShot(0, dialog, [this, generation]() {
      if (!candidate_completion_pending || generation != process_generation)
        return;
      Candidate& finished = candidates[pending_candidate_row];
      absl::Status configured = absl::OkStatus();
      bool continue_with_solve = false;
      const bool may_be_complete = !current_candidate_workspace_unsafe && !cancelling && !closing &&
          pending_candidate_startup_error.isEmpty() && pending_candidate_exit_status == QProcess::NormalExit &&
          pending_candidate_exit_code == 0;
      if (may_be_complete && finished.phase == CandidatePhase::kScan) {
        Candidate* baseline = baseline_for(finished);
        if (!baseline || !baseline->complete || !baseline->workspace) {
          configured = absl::FailedPreconditionError("The scan baseline is no longer available");
        } else {
          auto selection = PreparePlayerSelectedStitchingExperiment(
              *baseline->workspace, *finished.workspace, scan_report_path(finished), finished.scan_duration_seconds);
          if (!selection.ok()) {
            configured = selection.status();
          } else {
            append_output_message(QString::fromStdString(selection->diagnostic) + "\n");
            if (selection->available) {
              continue_with_solve = true;
              finished.has_selected_frames = true;
              finished.phase = CandidatePhase::kCalibration;
            } else {
              configured = absl::FailedPreconditionError("Unavailable: " + selection->diagnostic);
            }
          }
        }
      } else if (may_be_complete) {
        configured = CompleteStitchingExperimentWorkspace(*finished.workspace, finished.requires_ice_mask);
        finished.complete = configured.ok();
      }
      if (finished.complete) {
        finished.failure.clear();
        table->item(pending_candidate_row, 5)->setText("Ready");
      } else if (!continue_with_solve) {
        if (current_candidate_workspace_unsafe)
          finished.failure = "Process group still active; workspace retained";
        else if (cancelling || closing)
          finished.failure = "Cancelled";
        else if (!pending_candidate_startup_error.isEmpty())
          finished.failure = pending_candidate_startup_error;
        else if (pending_candidate_exit_status != QProcess::NormalExit)
          finished.failure = "Runner crashed";
        else if (pending_candidate_exit_code != 0)
          finished.failure = QString("Runner exited %1").arg(pending_candidate_exit_code);
        else if (!configured.ok())
          finished.failure = QString::fromStdString(configured.ToString());
        else
          finished.failure = "Calibration failed";
        table->item(pending_candidate_row, 5)->setText(finished.failure);
        table->item(pending_candidate_row, 5)->setToolTip(finished.failure);
      }
      calibration_process.reset();
      calibration_process_group = 0;
      calibration_process_token.clear();
      candidate_completion_pending = false;
      current_candidate_workspace_unsafe = false;
      pending_candidate_row = -1;
      pending_candidate_exit_code = -1;
      pending_candidate_exit_status = QProcess::CrashExit;
      pending_candidate_startup_error.clear();
      if (continue_with_solve)
        launch_candidate_phase();
      else
        launch_next_candidate();
      maybe_finish_close();
    });
  }

  void finalize_preview_completion() {
    if (!preview_completion_pending)
      return;
    QTimer::singleShot(0, dialog, [this]() {
      if (!preview_completion_pending)
        return;
      const bool replay = preview_replay_pending && !stopping_preview && !closing;
      preview_replay_pending = false;
      preview_process.reset();
      preview_process_group = 0;
      preview_process_token.clear();
      preview_completion_pending = false;
      preview_candidate_row = -1;
      update_controls();
      if (replay)
        start_preview();
      else
        maybe_finish_close();
    });
  }

  void stop_preview_process() {
    stopping_preview = true;
    preview_replay_pending = false;
    const bool process_running = preview_process && preview_process->state() != QProcess::NotRunning;
    if (!process_running && !experiment_session_alive(preview_process_group, preview_process_token))
      return;
    if (!process_running || preview_process->write("q") < 0 || !preview_process->waitForBytesWritten(250))
      interrupt_process(preview_process.get(), preview_process_group, preview_process_token);
    schedule_forced_stop(/*calibration=*/false);
  }

  void finish_shutdown_ticket(bool calibration, uint64_t ticket, bool retain_workspace = false) {
    std::optional<uint64_t>& active = calibration ? calibration_shutdown_ticket : preview_shutdown_ticket;
    if (!active.has_value() || *active != ticket)
      return;
    active.reset();
    --pending_group_shutdowns;
    qint64& process_group = calibration ? calibration_process_group : preview_process_group;
    process_group = 0;
    (calibration ? calibration_process_token : preview_process_token).clear();
    if (retain_workspace) {
      if (session) {
        session->setAutoRemove(false);
        session_retained = true;
      }
      if (calibration) {
        current_candidate_workspace_unsafe = true;
        cancelling = true;
      } else {
        preview_replay_pending = false;
        if (preview_candidate_row >= 0 && preview_candidate_row < static_cast<int>(candidates.size())) {
          Candidate& candidate = candidates[preview_candidate_row];
          candidate.complete = false;
          candidate.failure = "Preview process group still active; workspace retained";
          table->item(candidate.row, 5)->setText(candidate.failure);
        }
      }
    }
    QProcess* process = calibration ? calibration_process.get() : preview_process.get();
    const bool process_finished = !process || process->state() == QProcess::NotRunning;
    if (process_finished) {
      if (calibration)
        finalize_candidate_completion();
      else
        finalize_preview_completion();
    }
    update_controls();
    maybe_finish_close();
  }

  void settle_finished_process(bool calibration) {
    std::optional<uint64_t>& active = calibration ? calibration_shutdown_ticket : preview_shutdown_ticket;
    qint64& process_group = calibration ? calibration_process_group : preview_process_group;
    const QString& process_token = calibration ? calibration_process_token : preview_process_token;
    if (active.has_value()) {
      if (!experiment_session_alive(process_group, process_token))
        finish_shutdown_ticket(calibration, *active);
      return;
    }
    if (experiment_session_alive(process_group, process_token)) {
      QProcess* process = calibration ? calibration_process.get() : preview_process.get();
      interrupt_process(process, process_group, process_token);
      schedule_forced_stop(calibration);
      return;
    }
    process_group = 0;
    (calibration ? calibration_process_token : preview_process_token).clear();
    if (calibration)
      finalize_candidate_completion();
    else
      finalize_preview_completion();
  }

  void schedule_forced_stop(bool calibration) {
    QProcess* process = calibration ? calibration_process.get() : preview_process.get();
    qint64& process_group = calibration ? calibration_process_group : preview_process_group;
    const QString& process_token = calibration ? calibration_process_token : preview_process_token;
    const bool process_running = process && process->state() != QProcess::NotRunning;
    if (!process_running && !experiment_session_alive(process_group, process_token))
      return;
    std::optional<uint64_t>& active = calibration ? calibration_shutdown_ticket : preview_shutdown_ticket;
    if (active.has_value())
      return;
    const uint64_t ticket = ++next_shutdown_ticket;
    active = ticket;
    ++pending_group_shutdowns;
    const qint64 captured_group = process_group;
    const QString captured_token = process_token;
    const QPointer<QProcess> guarded_process(process);
    QTimer::singleShot(3000, dialog, [this, calibration, ticket, captured_group, captured_token, guarded_process]() {
      const std::optional<uint64_t>& current = calibration ? calibration_shutdown_ticket : preview_shutdown_ticket;
      if (!current.has_value() || *current != ticket)
        return;
      const qint64 effective_group =
          captured_group > 0 ? captured_group : (calibration ? calibration_process_group : preview_process_group);
      const QString effective_token = !captured_token.isEmpty()
          ? captured_token
          : (calibration ? calibration_process_token : preview_process_token);
      kill_process(guarded_process.data(), effective_group, effective_token);
      QTimer::singleShot(100, dialog, [this, calibration, ticket, effective_group, effective_token]() {
        const bool group_unverified = effective_group <= 0 || effective_token.isEmpty();
        const bool descendants_remain = group_unverified || experiment_session_alive(effective_group, effective_token);
        if (descendants_remain) {
          const QString retained_path = session ? session->path() : QString("(unknown path)");
          const QString message =
              QString(
                  "An experiment process group could not be confirmed stopped; retaining its temporary workspace "
                  "at %1.")
                  .arg(retained_path);
          append_output_message(message + "\n");
          qWarning().noquote() << "Stitching Experiments:" << message;
        }
        finish_shutdown_ticket(calibration, ticket, descendants_remain);
      });
    });
  }

  void stop_calibration_process() {
    const bool process_running = calibration_process && calibration_process->state() != QProcess::NotRunning;
    if (!process_running && !experiment_session_alive(calibration_process_group, calibration_process_token))
      return;
    interrupt_process(calibration_process.get(), calibration_process_group, calibration_process_token);
    schedule_forced_stop(/*calibration=*/true);
  }

  void maybe_finish_close() {
    if (!closing)
      return;
    const bool calibrating = calibration_process && calibration_process->state() != QProcess::NotRunning;
    const bool previewing = preview_process && preview_process->state() != QProcess::NotRunning;
    if (calibrating || previewing || promotion_worker || pending_group_shutdowns > 0 || candidate_completion_pending ||
        preview_completion_pending || close_completion_scheduled)
      return;
    close_completion_scheduled = true;
    const int result = pending_dialog_result;
    QTimer::singleShot(0, dialog, [this, result]() { dialog->QDialog::done(result); });
  }

  void finish_candidate(
      int row,
      uint64_t generation,
      int exit_code,
      QProcess::ExitStatus exit_status,
      const QString& startup_error = {}) {
    if (candidate_completion_pending || generation != process_generation || row != running_candidate)
      return;
    candidate_completion_pending = true;
    append_output(calibration_process.get());
    pending_candidate_row = row;
    pending_candidate_exit_code = exit_code;
    pending_candidate_exit_status = exit_status;
    pending_candidate_startup_error = startup_error;
    settle_finished_process(/*calibration=*/true);
  }

  void launch_next_candidate() {
    ++running_candidate;
    if (cancelling || closing || running_candidate >= static_cast<int>(candidates.size())) {
      calibration_process.reset();
      calibration_process_group = 0;
      calibration_process_token.clear();
      batch_active = false;
      for (int row = running_candidate; row < static_cast<int>(candidates.size()); ++row)
        table->item(row, 5)->setText("Cancelled");
      progress->setValue(std::min(running_candidate, static_cast<int>(candidates.size())));
      show_status(
          cancelling || closing
              ? "Batch cancelled. Completed candidates remain available; the main Program is unchanged."
              : "Batch complete. Select a successful row and preview the seam in motion.");
      cancelling = false;
      update_controls();
      return;
    }
    Candidate& candidate = candidates[running_candidate];
    if (candidate.baseline_sequence != 0) {
      Candidate* baseline = baseline_for(candidate);
      if (!baseline || !baseline->complete || !baseline->workspace) {
        candidate.failure = "Unavailable: baseline calibration failed";
        table->item(candidate.row, 5)->setText(candidate.failure);
        QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
        return;
      }
      candidate.phase = CandidatePhase::kScan;
    }
    if (!candidate.workspace.has_value()) {
      auto workspace = CreateStitchingExperimentWorkspace(
          game_directory.toStdString(), session->path().toStdString(), candidate.settings, candidate.sequence);
      if (!workspace.ok()) {
        candidate.failure = QString::fromStdString(workspace.status().ToString());
        table->item(candidate.row, 5)->setText(candidate.failure);
        progress->setValue(running_candidate + 1);
        show_status(QString("Could not prepare Candidate %1: %2").arg(candidate.sequence).arg(candidate.failure), true);
        QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
        return;
      }
      candidate.workspace = std::move(*workspace);
    }
    launch_candidate_phase();
  }

  void launch_candidate_phase() {
    if (cancelling || closing) {
      launch_next_candidate();
      return;
    }
    Candidate& candidate = candidates[running_candidate];
    const bool scanning = candidate.phase == CandidatePhase::kScan;
    Candidate* source = scanning ? baseline_for(candidate) : &candidate;
    if (!source || !source->workspace) {
      candidate.failure = "Unavailable: missing baseline workspace";
      table->item(candidate.row, 5)->setText(candidate.failure);
      QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
      return;
    }
    table->item(candidate.row, 5)->setText(scanning ? "Scanning players…" : "Calibrating…");
    table->selectRow(candidate.row);
    progress->setValue(running_candidate);
    show_status(
        QString(scanning ? "Selecting player-rich frames for candidate %1 of %2…" : "Calibrating candidate %1 of %2…")
            .arg(running_candidate + 1)
            .arg(candidates.size()));

    calibration_process = std::make_unique<QProcess>();
    const uint64_t generation = ++process_generation;
    calibration_process_group = 0;
    calibration_process_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    current_candidate_workspace_unsafe = false;
    calibration_process->setProcessChannelMode(QProcess::MergedChannels);
    calibration_process->setWorkingDirectory(working_directory);
    QProcessEnvironment env = candidate_environment(*source);
    env.insert("HSTREAM_EXPERIMENT_PROCESS_TOKEN", calibration_process_token);
    QStringList args = base_arguments(*source);
    args << "--enable-sinks=FAKE";
    if (scanning) {
      args.removeAll("--stitching-calibration-only");
      args << QString("--stitching-player-scan-output=%1")
                  .arg(QString::fromStdString(scan_report_path(candidate).string()))
           << "--stitching-player-scan-interval-ms=500"
           << QString("--stitching-player-scan-frame-count=%1").arg(candidate.settings.frame_count)
           << QString("--start-time=%1").arg(QString::fromStdString(candidate.settings.stitch_frame_time))
           << QString("-t=%1").arg(candidate.scan_duration_seconds);
    } else {
      env.insert("HSTREAM_CALIBRATION_PENDING", "1");
      env.insert("HSTREAM_CALIBRATION_START_STAGE", "input");
      env.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", QString::fromStdString(candidate.workspace->invalidation_id));
      if (candidate.baseline_sequence == 0)
        args << "--force-reconfigure";
      args << QString("--clean-expected-invalidation-id=%1")
                  .arg(QString::fromStdString(candidate.workspace->invalidation_id))
           << QString("--options=pipeline.hmstitcher.calibration-frame-count=%1").arg(candidate.settings.frame_count)
           << QString("--stitch-frame-time=%1").arg(QString::fromStdString(candidate.settings.stitch_frame_time))
           << "-t=1";
      if (candidate.requires_ice_mask)
        args << "--stitching-calibration-with-ice-mask";
    }
    calibration_process->setProcessEnvironment(env);
    bool uses_process_group = false;
#ifdef Q_OS_UNIX
    uses_process_group = true;
    args.push_front(runner);
    calibration_process->setProgram("/usr/bin/setsid");
#else
    calibration_process->setProgram(runner);
#endif
    calibration_process->setArguments(args);
    QObject::connect(calibration_process.get(), &QProcess::started, dialog, [this, uses_process_group, generation]() {
      if (uses_process_group && generation == process_generation)
        calibration_process_group = calibration_process->processId();
    });
    QObject::connect(calibration_process.get(), &QProcess::readyReadStandardOutput, dialog, [this, generation]() {
      if (generation == process_generation)
        append_output(calibration_process.get());
    });
    QObject::connect(
        calibration_process.get(),
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        dialog,
        [this, row = candidate.row, generation](int exit_code, QProcess::ExitStatus exit_status) {
          finish_candidate(row, generation, exit_code, exit_status);
        });
    QObject::connect(
        calibration_process.get(),
        &QProcess::errorOccurred,
        dialog,
        [this, row = candidate.row, generation](QProcess::ProcessError error) {
          if (error == QProcess::FailedToStart && generation == process_generation)
            finish_candidate(row, generation, -1, QProcess::CrashExit, calibration_process->errorString());
        });
    calibration_process->start();
    update_controls();
  }

  static bool same_settings(const StitchingExperimentSettings& left, const StitchingExperimentSettings& right) {
    return left.control_points == right.control_points && left.frame_count == right.frame_count &&
        left.stitch_frame_time == right.stitch_frame_time && left.rink_rotation_degrees == right.rink_rotation_degrees;
  }

  void append_candidate(Candidate candidate) {
    const int row = table->rowCount();
    candidate.row = row;
    table->insertRow(row);
    const StitchingExperimentSettings& settings = candidate.settings;
    const QString rotation = settings.rink_rotation_degrees ? QString("%1 / %2")
                                                                  .arg((*settings.rink_rotation_degrees)[1], 0, 'g', 6)
                                                                  .arg((*settings.rink_rotation_degrees)[2], 0, 'g', 6)
                                                            : "Saved setting";
    const QStringList columns = {
        candidate.baseline_sequence != 0
            ? QString("Players %1").arg(candidate.sequence)
            : QString(candidate.requires_ice_mask ? "Baseline %1" : "Candidate %1").arg(candidate.sequence),
        QString::number(settings.control_points),
        QString::number(settings.frame_count),
        QString::fromStdString(settings.stitch_frame_time),
        rotation,
        "Queued",
    };
    for (int column = 0; column < columns.size(); ++column) {
      auto* item = new QTableWidgetItem(columns[column]);
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      table->setItem(row, column, item);
    }
    candidates.push_back(std::move(candidate));
  }

  void add_candidates_to_batch() {
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
    std::vector<Candidate> additions;
    std::set<int> required_baselines;
    int sequence = next_candidate_sequence;
    const auto find = [&](const StitchingExperimentSettings& settings, bool automatic) -> const Candidate* {
      const auto matches = [&](const Candidate& item) {
        return same_settings(item.settings, settings) && (item.baseline_sequence != 0) == automatic &&
            (!automatic || item.scan_duration_seconds == scan_duration->value());
      };
      const auto existing = std::find_if(candidates.begin(), candidates.end(), matches);
      if (existing != candidates.end())
        return &*existing;
      const auto pending = std::find_if(additions.begin(), additions.end(), matches);
      return pending == additions.end() ? nullptr : &*pending;
    };
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
            const bool automatic = prefer_player_frames->isChecked() && frame_count > 1;
            const Candidate* baseline = find(settings, false);
            int baseline_sequence = baseline ? baseline->sequence : ++sequence;
            if (!baseline)
              additions.push_back(Candidate{.settings = settings, .sequence = baseline_sequence});
            if (automatic) {
              required_baselines.insert(baseline_sequence);
              if (!find(settings, true)) {
                additions.push_back(
                    Candidate{
                        .settings = settings,
                        .sequence = ++sequence,
                        .baseline_sequence = baseline_sequence,
                        .scan_duration_seconds = scan_duration->value(),
                    });
              }
            }
            if (candidates.size() + additions.size() > 64) {
              show_status(
                  "Adding these options, including their baselines, would exceed the 64-candidate batch limit.", true);
              return;
            }
          }
        }
      }
    }
    if (additions.empty()) {
      show_status("Every combination from these options is already in the batch.");
      return;
    }
    for (Candidate& baseline : candidates) {
      if (required_baselines.count(baseline.sequence)) {
        baseline.requires_ice_mask = true;
        table->item(baseline.row, 0)->setText(QString("Baseline %1").arg(baseline.sequence));
      }
    }
    next_candidate_sequence = sequence;
    for (Candidate& candidate : additions) {
      candidate.requires_ice_mask = required_baselines.count(candidate.sequence) != 0;
      append_candidate(std::move(candidate));
    }
    table->selectRow(table->rowCount() - 1);
    show_status(QString("Added %1 candidate%2. The batch now contains %3; add more options or start it.")
                    .arg(additions.size())
                    .arg(additions.size() == 1 ? "" : "s")
                    .arg(candidates.size()));
    update_controls();
  }

  void remove_selected_from_batch() {
    const int row = table->currentRow();
    if (batch_started || row < 0 || row >= static_cast<int>(candidates.size()))
      return;
    const int sequence = candidates[row].sequence;
    // Removing a baseline also removes every dependent automatic candidate;
    // stable sequence identities keep other dependencies intact as rows shift.
    for (int index = static_cast<int>(candidates.size()) - 1; index >= 0; --index) {
      if (candidates[index].sequence == sequence || candidates[index].baseline_sequence == sequence) {
        candidates.erase(candidates.begin() + index);
        table->removeRow(index);
      }
    }
    for (int index = 0; index < static_cast<int>(candidates.size()); ++index) {
      Candidate& candidate = candidates[index];
      candidate.row = index;
      if (candidate.baseline_sequence == 0) {
        candidate.requires_ice_mask = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& item) {
          return item.baseline_sequence == candidate.sequence;
        });
        table->item(index, 0)->setText(
            QString(candidate.requires_ice_mask ? "Baseline %1" : "Candidate %1").arg(candidate.sequence));
      }
    }
    if (!candidates.empty()) {
      table->selectRow(std::min(row, static_cast<int>(candidates.size()) - 1));
      show_status(QString("Removed the candidate. %1 remain in the batch.").arg(candidates.size()));
    } else {
      session.reset();
      next_candidate_sequence = 0;
      show_status("Removed the last candidate and discarded its private experiment data.");
    }
    update_controls();
  }

  void clear_candidate_batch() {
    if (batch_active)
      return;
    const bool retained = session_retained && session;
    const QString retained_path = retained ? session->path() : QString();
    candidates.clear();
    table->setRowCount(0);
    session.reset();
    session_retained = false;
    next_candidate_sequence = 0;
    running_candidate = -1;
    batch_started = false;
    batch_active = false;
    cancelling = false;
    progress->setRange(0, 0);
    progress->setValue(0);
    if (retained) {
      show_status(
          QString(
              "Batch cleared. An unconfirmed process workspace remains at %1 until it is safe to remove; the main "
              "Program was not changed.")
              .arg(retained_path),
          true);
    } else {
      show_status("Private experiment batch discarded. No additional changes were made to the main Program.");
    }
    update_controls();
  }

  void start_candidate_batch() {
    if (batch_active || batch_started || candidates.empty())
      return;
    session = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hstream-stitch-experiments-XXXXXX");
    if (!session->isValid()) {
      show_status("Could not create a private stitching experiment directory.", true);
      session.reset();
      return;
    }
    batch_started = true;
    batch_active = true;
    running_candidate = -1;
    cancelling = false;
    progress->setRange(0, static_cast<int>(candidates.size()));
    progress->setValue(0);
    show_status(QString("Starting %1 queued calibration candidates…").arg(candidates.size()));
    update_controls();
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
    preview_replay_pending = false;
    preview_candidate_row = row;
    Candidate& candidate = candidates[row];
    preview_process = std::make_unique<QProcess>();
    preview_process_group = 0;
    preview_process_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preview_process->setProcessChannelMode(QProcess::MergedChannels);
    preview_process->setWorkingDirectory(working_directory);
    QProcessEnvironment preview_environment = candidate_environment(candidate);
    preview_environment.insert("HSTREAM_EXPERIMENT_PROCESS_TOKEN", preview_process_token);
    preview_process->setProcessEnvironment(preview_environment);
    QStringList args = base_arguments(candidate);
    args << "--enable-sinks=RENDER"
         << "--show" << QString("--start-time=%1").arg(format_time(preview_start->time()))
         << QString("--time-limit=%1").arg(preview_duration->value())
         << QString("--ui-preview-windows=stitched:%1").arg(static_cast<qulonglong>(video->winId()))
         << "--ui-preview-active=stitched";
    bool uses_process_group = false;
#ifdef Q_OS_UNIX
    uses_process_group = true;
    args.push_front(runner);
    preview_process->setProgram("/usr/bin/setsid");
#else
    preview_process->setProgram(runner);
#endif
    preview_process->setArguments(args);
    QObject::connect(preview_process.get(), &QProcess::started, dialog, [this, uses_process_group]() {
      if (uses_process_group)
        preview_process_group = preview_process->processId();
    });
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
          preview_replay_pending = !stopping_preview && !closing && loop->isChecked() &&
              exit_status == QProcess::NormalExit && exit_code == 0;
          settle_finished_process(/*calibration=*/false);
        });
    QObject::connect(preview_process.get(), &QProcess::errorOccurred, dialog, [this](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart || preview_completion_pending)
        return;
      preview_completion_pending = true;
      preview_replay_pending = false;
      show_status("Could not start the stitched preview: " + preview_process->errorString(), true);
      settle_finished_process(/*calibration=*/false);
    });
    show_status(QString("Previewing Candidate %1 without crop or play tracking.").arg(candidate.sequence));
    preview_process->start();
    update_controls();
  }

  void apply_selected() {
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].complete)
      return;
    Candidate& candidate = candidates[row];
    show_status(QString("Publishing Candidate %1 to the main Program…").arg(candidate.sequence));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto result = std::make_shared<absl::Status>(absl::UnknownError("Promotion did not complete"));
    const StitchingExperimentWorkspace workspace = *candidate.workspace;
    const std::string destination = game_directory.toStdString();
    const int sequence = candidate.sequence;
    QThread* worker = QThread::create(
        [workspace, destination, result]() { *result = PromoteStitchingExperiment(workspace, destination); });
    promotion_worker = worker;
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, sequence]() {
      if (promotion_worker != worker)
        return;
      promotion_worker = nullptr;
      QApplication::restoreOverrideCursor();
      if (!result->ok()) {
        show_status(QString::fromStdString(result->ToString()), true);
        append_output_message("Candidate publication failed: " + QString::fromStdString(result->ToString()) + "\n");
      } else {
        show_status(QString(
                        "Candidate %1 selected. Its maps and seam will be used by the next main Program run without "
                        "recalibration.")
                        .arg(sequence));
        if (selection_applied)
          selection_applied();
        request_close(QDialog::Accepted);
        return;
      }
      update_controls();
      maybe_finish_close();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    update_controls();
  }

  void inspect_selected_frames() {
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].has_selected_frames ||
        !candidates[row].workspace)
      return;
    auto inspection = InspectStitchingExperimentFrames(*candidates[row].workspace);
    if (!inspection.ok()) {
      show_status(QString::fromStdString(inspection.status().ToString()), true);
      return;
    }
    QDialog viewer(dialog);
    viewer.setObjectName("stitchExperimentFrameInspector");
    viewer.setWindowTitle(QString("Selected frames — Candidate %1").arg(candidates[row].sequence));
    viewer.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    viewer.resize(1180, 780);
    auto* layout = new QVBoxLayout(&viewer);
    auto* explanation = new QLabel(
        "These are the exact synchronized camera pairs selected for calibration. Thumbnails come from the same "
        "extracted images used by the matcher. The coverage grid summarizes people surviving the Program ice mask "
        "in the baseline stitched view; it is not an overlay on the raw cameras.");
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* validation = new QLabel(QString::fromStdString(inspection->source_validation));
    validation->setTextFormat(Qt::PlainText);
    validation->setWordWrap(true);
    layout->addWidget(validation);
    auto* choices = new QTableWidget(static_cast<int>(inspection->frames.size()), 5);
    choices->setObjectName("stitchExperimentSelectedFrames");
    choices->setHorizontalHeaderLabels(
        {"Pair", "Time after anchor", "People in overlap", "Far / middle / near", "Quality"});
    choices->setSelectionBehavior(QAbstractItemView::SelectRows);
    choices->setSelectionMode(QAbstractItemView::SingleSelection);
    choices->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    choices->setMaximumHeight(190);
    for (size_t index = 0; index < inspection->frames.size(); ++index) {
      const auto& frame = inspection->frames[index];
      const QStringList values = {
          index == 0 ? "1 (anchor)" : QString::number(index + 1),
          exact_frame_time(frame.timeline_ns - inspection->frames.front().timeline_ns),
          QString::number(frame.eligible_people),
          QString("%1 / %2 / %3")
              .arg(frame.size_band_counts[0])
              .arg(frame.size_band_counts[1])
              .arg(frame.size_band_counts[2]),
          QString::number(frame.quality, 'g', 6),
      };
      for (int column = 0; column < values.size(); ++column) {
        auto* item = new QTableWidgetItem(values[column]);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        choices->setItem(static_cast<int>(index), column, item);
      }
    }
    layout->addWidget(choices);
    auto* images = new QHBoxLayout();
    std::array<SelectedFrameImage*, 2> camera_images;
    for (size_t camera = 0; camera < camera_images.size(); ++camera) {
      auto* group = new QGroupBox(camera == 0 ? "Left camera" : "Right camera");
      auto* image_layout = new QVBoxLayout(group);
      camera_images[camera] = new SelectedFrameImage(group);
      camera_images[camera]->setObjectName(
          camera == 0 ? "stitchExperimentSelectedLeft" : "stitchExperimentSelectedRight");
      image_layout->addWidget(camera_images[camera]);
      images->addWidget(group, 2);
    }
    auto* coverage_group = new QGroupBox("Baseline stitched scoring coverage");
    auto* coverage_layout = new QVBoxLayout(coverage_group);
    auto* coverage = new SelectedFrameCoverage(coverage_group);
    coverage->setObjectName("stitchExperimentSelectedCoverage");
    coverage_layout->addWidget(coverage, 1);
    auto* legend = new QLabel("Blue: far · Green: middle · Amber: near\nApparent size bands, not measured distances");
    legend->setWordWrap(true);
    coverage_layout->addWidget(legend);
    images->addWidget(coverage_group, 1);
    layout->addLayout(images, 1);
    auto* detail = new QPlainTextEdit();
    detail->setObjectName("stitchExperimentSelectedFrameIdentity");
    detail->setReadOnly(true);
    detail->setMaximumHeight(150);
    layout->addWidget(detail);
    const auto show_pair = [&]() {
      const int selected_row = choices->currentRow();
      if (selected_row < 0 || selected_row >= static_cast<int>(inspection->frames.size()))
        return;
      const auto& frame = inspection->frames[selected_row];
      QString identity = QString("Decode anchor: %1 (%2 ns)\nPair timeline: %3 ns\n")
                             .arg(exact_frame_time(inspection->anchor_ns))
                             .arg(inspection->anchor_ns)
                             .arg(frame.timeline_ns);
      for (size_t camera = 0; camera < camera_images.size(); ++camera) {
        camera_images[camera]->load(frame.thumbnails[camera]);
        identity += QString("%1: %2\n  Source PTS: %3 (%4 ns); decoded sequence: %5\n")
                        .arg(camera == 0 ? "Left" : "Right")
                        .arg(QString::fromStdString(frame.camera_paths[camera]))
                        .arg(exact_frame_time(frame.source_pts_ns[camera]))
                        .arg(frame.source_pts_ns[camera])
                        .arg(frame.decoded_sequences[camera]);
      }
      identity += "Selection fingerprint: " + QString::fromStdString(inspection->fingerprint);
      detail->setPlainText(identity);
      coverage->set_coverage(frame.coverage);
    };
    QObject::connect(choices, &QTableWidget::itemSelectionChanged, &viewer, show_pair);
    auto* close = new QPushButton("Close");
    QObject::connect(close, &QPushButton::clicked, &viewer, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    choices->selectRow(0);
    viewer.exec();
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
    const bool preview_owned = (preview_process && preview_process->state() != QProcess::NotRunning) ||
        preview_shutdown_ticket.has_value() || experiment_session_alive(preview_process_group, preview_process_token);
    const bool calibration_owned = (calibration_process && calibration_process->state() != QProcess::NotRunning) ||
        calibration_shutdown_ticket.has_value() ||
        experiment_session_alive(calibration_process_group, calibration_process_token);
    if (preview_owned)
      kill_process(preview_process.get(), preview_process_group, preview_process_token);
    if (calibration_owned)
      kill_process(calibration_process.get(), calibration_process_group, calibration_process_token);
    const bool unverified_process =
        (preview_owned && preview_process_group <= 0) || (calibration_owned && calibration_process_group <= 0);
    if (session &&
        (unverified_process || experiment_session_alive(preview_process_group, preview_process_token) ||
         experiment_session_alive(calibration_process_group, calibration_process_token)))
      session->setAutoRemove(false);
    if (promotion_worker) {
      promotion_worker->wait();
      delete promotion_worker;
      promotion_worker = nullptr;
    }
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
  setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
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
      "Add as many option combinations as you want to the batch, start it when ready, and leave it running. When "
      "you return, replay the same moving passage across each completed seam. Nothing changes the main stitching "
      "configuration unless you explicitly choose Use selected in main Program. Optional player selection uses "
      "the same ice filtering as Program; moving previews do not run crop or tracking.");
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* matrix_group = new QGroupBox("Candidate matrix");
  matrix_group->setObjectName("stitchExperimentMatrix");
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
  s.prefer_player_frames = new QCheckBox("Prefer player-rich frames");
  s.prefer_player_frames->setObjectName("stitchExperimentPreferPlayerFrames");
  s.prefer_player_frames->setToolTip(
      "Add an automatic candidate alongside its ordinary baseline. People are filtered by the Program ice mask. "
      "One-frame candidates keep only the anchor.");
  s.scan_duration = new QSpinBox();
  s.scan_duration->setObjectName("stitchExperimentPlayerScanDuration");
  s.scan_duration->setRange(1, 300);
  s.scan_duration->setValue(60);
  s.scan_duration->setSuffix(" s");
  s.scan_duration->setEnabled(false);
  auto* player_options = new QHBoxLayout();
  player_options->addWidget(s.prefer_player_frames);
  player_options->addWidget(s.scan_duration);
  matrix_layout->addRow("Player search", player_options);
  connect(s.prefer_player_frames, &QCheckBox::toggled, this, [&s]() { s.update_controls(); });
  auto* matrix_actions = new QHBoxLayout();
  s.add_to_batch = new QPushButton("Add options to batch");
  s.add_to_batch->setObjectName("addStitchExperimentsToBatchButton");
  s.remove_from_batch = new QPushButton("Remove selected");
  s.remove_from_batch->setObjectName("removeStitchExperimentFromBatchButton");
  s.clear_batch = new QPushButton("Discard batch");
  s.clear_batch->setObjectName("clearStitchExperimentBatchButton");
  s.start_batch = new QPushButton("Start batch");
  s.start_batch->setObjectName("startStitchExperimentBatchButton");
  s.cancel = new QPushButton("Cancel batch");
  s.cancel->setObjectName("cancelStitchExperimentsButton");
  s.cancel->setEnabled(false);
  matrix_actions->addWidget(s.add_to_batch);
  matrix_actions->addWidget(s.remove_from_batch);
  matrix_actions->addWidget(s.clear_batch);
  matrix_layout->addRow(matrix_actions);
  auto* batch_actions = new QHBoxLayout();
  batch_actions->addWidget(s.start_batch);
  batch_actions->addWidget(s.cancel);
  matrix_layout->addRow(batch_actions);

  auto* splitter = s.splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName("stitchExperimentSplitter");
  splitter->setChildrenCollapsible(false);
  auto* candidate_panel = new QWidget();
  candidate_panel->setObjectName("stitchExperimentCandidatePanel");
  auto* candidate_layout = new QVBoxLayout(candidate_panel);
  candidate_layout->setContentsMargins(0, 0, 0, 0);
  candidate_layout->addWidget(matrix_group);
  s.table = new QTableWidget(0, 6);
  s.table->setObjectName("stitchExperimentCandidates");
  s.table->setHorizontalHeaderLabels({"Candidate", "CP", "Frames", "First frame", "Rink pitch / roll", "Status"});
  s.table->setSelectionBehavior(QAbstractItemView::SelectRows);
  s.table->setSelectionMode(QAbstractItemView::SingleSelection);
  s.table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  s.table->horizontalHeader()->setStretchLastSection(true);
  candidate_layout->addWidget(s.table, 1);
  splitter->addWidget(candidate_panel);

  auto* preview_panel = new QWidget();
  preview_panel->setObjectName("stitchExperimentPreviewPanel");
  auto* preview_layout = new QVBoxLayout(preview_panel);
  s.video = new StitchingExperimentVideoTarget(preview_panel);
  s.video->setObjectName("stitchExperimentVideo");
  preview_layout->addWidget(s.video, 1);
  auto* preview_controls = new QFormLayout();
  preview_controls->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
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
  s.expand_preview = new QPushButton("Expand preview");
  s.expand_preview->setObjectName("maximizeStitchExperimentButton");
  s.expand_preview->setAutoDefault(false);
  s.expand_preview->setToolTip("Expand the preview within this dialog (or double-click the preview).");
  auto* start_label = new QLabel("Passage start");
  start_label->setObjectName("stitchExperimentPreviewStartLabel");
  start_label->setBuddy(s.preview_start);
  auto* duration_label = new QLabel("Duration");
  duration_label->setObjectName("stitchExperimentPreviewDurationLabel");
  duration_label->setBuddy(s.preview_duration);
  auto* duration_controls = new QHBoxLayout();
  duration_controls->addWidget(s.preview_duration);
  duration_controls->addWidget(s.loop);
  preview_controls->addRow(start_label, s.preview_start);
  preview_controls->addRow(duration_label, duration_controls);
  preview_layout->addLayout(preview_controls);
  auto* preview_actions = new QHBoxLayout();
  preview_actions->addWidget(s.preview);
  preview_actions->addWidget(s.stop_preview);
  preview_actions->addWidget(s.expand_preview);
  preview_layout->addLayout(preview_actions);
  splitter->addWidget(preview_panel);
  splitter->setStretchFactor(0, 2);
  splitter->setStretchFactor(1, 3);
  splitter->setSizes({520, 760});
  root->addWidget(splitter, 1);

  s.progress = new QProgressBar();
  s.progress->setVisible(false);
  s.status = new QLabel("Choose options and add their combinations to the batch. The main Program remains unchanged.");
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
  s.preview_focus_siblings = {intro, candidate_panel, s.log};
  auto* bottom = new QHBoxLayout();
  s.apply = new QPushButton("Use selected in main Program");
  s.apply->setObjectName("applyStitchExperimentButton");
  s.inspect_frames = new QPushButton("Inspect selected frames");
  s.inspect_frames->setObjectName("inspectStitchExperimentFramesButton");
  auto* close = new QPushButton("Close");
  close->setObjectName("closeStitchExperimentButton");
  bottom->addWidget(s.apply);
  bottom->addWidget(s.inspect_frames);
  bottom->addStretch(1);
  bottom->addWidget(close);
  root->addLayout(bottom);

  connect(s.add_to_batch, &QPushButton::clicked, this, [&s]() { s.add_candidates_to_batch(); });
  connect(s.remove_from_batch, &QPushButton::clicked, this, [&s]() { s.remove_selected_from_batch(); });
  connect(s.clear_batch, &QPushButton::clicked, this, [&s]() { s.clear_candidate_batch(); });
  connect(s.start_batch, &QPushButton::clicked, this, [&s]() { s.start_candidate_batch(); });
  connect(s.cancel, &QPushButton::clicked, this, [&s]() {
    s.cancelling = true;
    s.show_status("Cancelling batch…");
    s.stop_calibration_process();
  });
  connect(s.table, &QTableWidget::itemSelectionChanged, this, [&s]() { s.update_controls(); });
  connect(s.table, &QTableWidget::cellDoubleClicked, this, [&s](int, int) { s.start_preview(); });
  connect(s.preview, &QPushButton::clicked, this, [&s]() { s.start_preview(); });
  connect(s.stop_preview, &QPushButton::clicked, this, [&s]() {
    s.stop_preview_process();
    s.show_status("Stopping preview…");
  });
  s.video->toggle_focus = [&s]() { s.set_preview_focus(!s.preview_focused); };
  connect(s.expand_preview, &QPushButton::clicked, this, s.video->toggle_focus);
  connect(s.apply, &QPushButton::clicked, this, [&s]() { s.apply_selected(); });
  connect(s.inspect_frames, &QPushButton::clicked, this, [&s]() { s.inspect_selected_frames(); });
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

void StitchingExperimentDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && impl_->preview_focused) {
    impl_->set_preview_focus(false);
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
}
