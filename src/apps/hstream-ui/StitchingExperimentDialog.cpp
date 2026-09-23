#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/CalibrationFrameView.h"
#include "src/apps/hstream-ui/MatchEditorDialog.h"

#include "src/apps/hstream-ui/StitchingExperimentBackend.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
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
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

// Keep the explicit window action in sync with title-bar and window-manager changes.
class WindowSizeButton : public QToolButton {
 public:
  explicit WindowSizeButton(QDialog* dialog) : QToolButton(dialog), dialog_(dialog) {
    dialog_->installEventFilter(this);
    connect(this, &QToolButton::clicked, dialog_, [this]() {
      if (dialog_->isMaximized())
        dialog_->showNormal();
      else
        dialog_->showMaximized();
    });
    update_action();
  }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object == dialog_ && event->type() == QEvent::WindowStateChange)
      update_action();
    return QToolButton::eventFilter(object, event);
  }

 private:
  void update_action() {
    const bool maximized = dialog_->isMaximized();
    setIcon(action_icon(maximized ? ActionIcon::Restore : ActionIcon::Expand));
    setText(maximized ? "Restore window" : "Maximize window");
    setToolTip(text());
    setAccessibleName(text());
  }
  QDialog* dialog_;
};

void add_image_navigation(QVBoxLayout* layout, CalibrationFrameView* view) {
  auto* controls = new QHBoxLayout();
  const auto add = [&](const char* name, const char* label, ActionIcon icon, auto method) {
    auto* button = new QToolButton(view->parentWidget());
    button->setObjectName(view->objectName() + name);
    button->setIcon(action_icon(icon));
    button->setText(label);
    button->setToolTip(label);
    button->setAccessibleName(label);
    QObject::connect(button, &QToolButton::clicked, view, method);
    controls->addWidget(button);
  };
  add("ZoomOut", "Zoom out", ActionIcon::ZoomOut, &CalibrationFrameView::zoomOut);
  add("ZoomIn", "Zoom in", ActionIcon::ZoomIn, &CalibrationFrameView::zoomIn);
  add("ActualSize", "Actual size (1:1)", ActionIcon::ActualSize, &CalibrationFrameView::actualSize);
  add("Fit", "Fit image", ActionIcon::Fit, &CalibrationFrameView::fitImage);
  controls->addStretch();
  layout->addLayout(controls);
}

constexpr char kStitchedPreviewOptions[] =
    "pipeline.streammux.batch-size=2,pipeline.streammux.sync-inputs=0,"
    "pipeline.streammux.batched-push-timeout=2147483647,pipeline.streammux.frame-num-reset-on-stream-reset=0,"
    "pipeline.streammux.frame-num-reset-on-eos=0,pipeline.hmstitcher.show=0";

class StitchingExperimentVideoTarget : public QWidget {
 public:
  explicit StitchingExperimentVideoTarget(QWidget* parent) : QWidget(parent) {
    if (QGuiApplication::platformName() == "xcb")
      setAttribute(Qt::WA_NativeWindow);
    setMinimumSize(320, 180);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    QPalette background = palette();
    background.setColor(QPalette::Window, Qt::black);
    setPalette(background);
    setAutoFillBackground(true);
  }

  void set_renderer_active(bool active) {
    // Qt owns idle pixels. While the runner owns the native target, keep its
    // frames out of the backing store without replacing the window's XID.
    const bool direct = active && QGuiApplication::platformName() == "xcb";
    setAttribute(Qt::WA_PaintOnScreen, direct);
    setAttribute(Qt::WA_NoSystemBackground, direct);
    setAutoFillBackground(!active);
    update();
  }

  QPaintEngine* paintEngine() const override {
    return testAttribute(Qt::WA_PaintOnScreen) ? nullptr : QWidget::paintEngine();
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

// Resolve only the two canonical feature settings needed by this editor. The
// runner continues to own the remaining configuration and native properties.
void overlay_feature_settings(YAML::Node& effective, const YAML::Node& layer) {
  const YAML::Node stitching = layer && layer.IsMap() ? layer["stitching"] : YAML::Node();
  if (!stitching || stitching.IsNull())
    return;
  if (!stitching.IsMap())
    throw std::runtime_error("stitching must be a configuration map");
  for (const char* key : {"control_point_matcher", "control_point_resolution"}) {
    if (stitching[key])
      effective["stitching"][key] = YAML::Clone(stitching[key]);
  }
}

hm::stitching::ControlPointMatcher feature_matcher(const YAML::Node& config) {
  const YAML::Node stitching = config["stitching"];
  const YAML::Node value = stitching && stitching.IsMap() ? stitching["control_point_matcher"] : YAML::Node();
  const auto matcher = hm::stitching::ParseControlPointMatcher(
      value && !value.IsNull() ? value.as<std::string>() : "superpoint-lightglue");
  if (!matcher.ok())
    throw std::runtime_error(matcher.status().ToString());
  return *matcher;
}

QString fixed_feature_size(hm::stitching::ControlPointMatcher matcher) {
  using hm::stitching::ControlPointMatcher;
  switch (matcher) {
    case ControlPointMatcher::kAkazeHamming:
      return "1920 px maximum";
    case ControlPointMatcher::kLoFTR:
      return "1600 px maximum";
    case ControlPointMatcher::kDeDoDeLightGlue:
      return "1024 × 576";
    case ControlPointMatcher::kSuperPointLightGlue:
      return {};
  }
  return {};
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

// Read only bounded metadata here. Store publication revalidates workspace
// ownership, count and anchor; source/input validation stays on the reuse worker.
absl::StatusOr<std::string> recover_selection_fingerprint(
    const StitchingExperimentStore& store,
    const StitchingExperimentWorkspace& workspace) {
  auto directory = hm::stitching::PinnedDirectory::Open(store.game_directory, "experiment game");
  if (!directory.ok())
    return directory.status();
  for (const auto& component : workspace.game_directory.lexically_relative(store.game_directory)) {
    auto child = directory->OpenChild(component.string(), "saved experiment directory");
    if (!child.ok())
      return child.status();
    if (!child->has_value())
      return absl::NotFoundError("Saved experiment directory is missing");
    *directory = std::move(**child);
  }
  auto text = hm::stitching::read_bounded_regular_file_no_follow(
      directory->path() / "config.yaml", 4 * 1024 * 1024, "saved experiment config");
  if (!text.ok())
    return text.status();
  try {
    const YAML::Node config = YAML::Load(*text);
    const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    if (!calibration.IsMap() || calibration["invalidation_id"].as<std::string>("") != workspace.invalidation_id ||
        calibration["control_points"].as<int>(0) != workspace.settings.control_points ||
        calibration["frame_count"].as<int>(0) != workspace.settings.frame_count ||
        config["stitching"]["calibration_frame_count"].as<int>(0) != workspace.settings.frame_count)
      return absl::FailedPreconditionError("Saved experiment owner or settings changed");
    const YAML::Node selection = config["stitching"]["calibration_frame_selection"];
    const YAML::Node inputs = config["stitching"]["calibration_frame_inputs_fingerprint"];
    if (!selection && !inputs)
      return std::string();
    auto plan = hm::stitching::ParsePlayerFrameSelectionPlan(selection);
    if (!plan.ok())
      return plan.status();
    if (inputs && (!inputs.IsScalar() || inputs.as<std::string>() != plan->fingerprint))
      return absl::FailedPreconditionError("Saved frame inputs do not match their selection");
    return plan->fingerprint;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid saved experiment config: " + std::string(error.what()));
  }
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
    int selection_owner_sequence{0};
    std::string saved_selection_fingerprint;
    int scan_duration_seconds{60};
    bool requires_ice_mask{false};
    CandidatePhase phase{CandidatePhase::kCalibration};
    bool has_selected_frames{false};
    bool selection_reuse_blocked{false};
    bool complete{false};
    bool queued{true};
    bool main_calibration{false};
    std::optional<StoredStitchingExperiment> stored;
    QString reservation_token;
    QString failure;
    QString process_output_tail;
    // Only additions made in this dialog suppress repeated Add requests. Saved
    // rows remain input owners, while a reopened matrix may intentionally rerun.
    QByteArray requested_config_revision;
  };

  StitchingExperimentDialog* dialog;
  QString game_directory;
  QString runner;
  QString working_directory;
  QString pipeline_config;
  QProcessEnvironment environment;
  StitchingExperimentSettings initial_settings;
  std::function<void()> selection_applied;
  QLineEdit* control_points{nullptr};
  QLineEdit* frame_counts{nullptr};
  QLineEdit* start_frames{nullptr};
  QComboBox* control_point_resolution{nullptr};
  YAML::Node feature_defaults;
  QString feature_settings_error;
  bool feature_size_selectable{false};
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
  QPushButton* view_runner_log{nullptr};
  QLabel* status{nullptr};
  QLabel* cache_location{nullptr};
  QProgressBar* progress{nullptr};
  QPlainTextEdit* log{nullptr};
  std::unique_ptr<QTemporaryDir> session;
  std::optional<StitchingExperimentStore> store;
  QString store_error;
  std::vector<Candidate> candidates;
  std::unique_ptr<QProcess> calibration_process;
  std::unique_ptr<QProcess> preview_process;
  QThread* promotion_worker{nullptr};
  QThread* preparation_worker{nullptr};
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
  int batch_only_row{-1};
  int open_editor_after_row{-1};
  bool batch_started{false};
  bool batch_active{false};
  bool cancelling{false};
  bool stopping_preview{false};
  bool closing{false};
  bool close_prompt_active{false};
  bool results_applied{false};
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
    expand_preview->setIcon(action_icon(focused ? ActionIcon::Restore : ActionIcon::Expand));
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
    result.insert("HM_OUTPUT_WORK_DIR", QString::fromStdString((candidate.workspace->root / "runner-output").string()));
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

  static std::pair<QString, QString> frame_selection_policy(const Candidate& candidate) {
    if (candidate.settings.frame_count == 1) {
      return {
          "Anchor only",
          "One-frame calibration uses the reference pair; no additional player-rich frames are selected."};
    }
    const bool frozen = candidate.has_selected_frames || !candidate.saved_selection_fingerprint.empty() ||
        (candidate.stored && !candidate.stored->selection_fingerprint.empty());
    const bool scan_planned =
        candidate.baseline_sequence != 0 || (candidate.stored && !candidate.stored->baseline_workspace_key.empty());
    if (candidate.selection_reuse_blocked || (candidate.main_calibration && !candidate.failure.isEmpty())) {
      return {
          frozen || scan_planned ? "Player-rich (unavailable)" : "Unconfirmed",
          "Saved frame inputs or process ownership could not be verified. See this candidate's status."};
    }
    if (frozen) {
      return {
          "Player-rich (saved)",
          "Uses this candidate's frozen player-rich frame selection, including its reference pair. "
          "Changing the checkbox does not replace saved frames."};
    }
    if (scan_planned || candidate.selection_owner_sequence != 0) {
      return {
          !candidate.queued && !candidate.failure.isEmpty() ? "Player-rich (unavailable)" : "Player-rich (pending)",
          "This candidate requests a player-rich selection from its baseline scan or shared frame owner, "
          "but no selection has been frozen in this candidate yet."};
    }
    return {
        candidate.requires_ice_mask ? "Ordinary (baseline)" : "Ordinary",
        candidate.requires_ice_mask
            ? "Uses ordinary frame spacing to prepare the baseline and rink mask for the separate player scan."
            : "Uses ordinary frame spacing. This candidate has no player-rich selection or pending player scan."};
  }

  void update_controls() {
    for (const Candidate& candidate : candidates) {
      auto* item = table->item(candidate.row, 6);
      if (!item)
        continue;
      const auto [label, description] = frame_selection_policy(candidate);
      if (item->text() != label)
        item->setText(label);
      if (item->toolTip() != description)
        item->setToolTip(description);
    }
    const bool previewing = preview_process && preview_process->state() != QProcess::NotRunning;
    const bool promoting = promotion_worker != nullptr || preparation_worker != nullptr;
    const bool stopping = pending_group_shutdowns > 0;
    const int row = table->currentRow();
    const bool selected = row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].complete;
    const bool selected_removable = row >= 0 && row < static_cast<int>(candidates.size()) &&
        !candidates[row].main_calibration && !candidates[row].complete &&
        (candidates[row].queued || !candidates[row].failure.isEmpty());
    const bool editing_batch =
        store && store_error.isEmpty() && !batch_active && !previewing && !promoting && !stopping && !closing;
    const auto queued_count =
        std::count_if(candidates.begin(), candidates.end(), [](const Candidate& item) { return item.queued; });
    add_to_batch->setEnabled(editing_batch && feature_settings_error.isEmpty() && queued_count < 64);
    remove_from_batch->setEnabled(
        store && !batch_active && !previewing && !promoting && !stopping && !closing && selected_removable);
    clear_batch->setEnabled(
        store && !batch_active && !previewing && !promoting && !stopping && !closing &&
        (!candidates.empty() || !store_error.isEmpty()));
    start_batch->setEnabled(editing_batch && queued_count > 0);
    cancel->setEnabled(batch_active && !closing);
    table->setEnabled(!batch_active && !previewing && !promoting && !closing);
    preview->setEnabled(
        selected && !candidates[row].main_calibration && !batch_active && !previewing && !promoting && !stopping &&
        !closing);
    stop_preview->setEnabled(previewing && !closing);
    apply->setEnabled(
        selected && !candidates[row].main_calibration && !batch_active && !previewing && !promoting && !closing);
    inspect_frames->setEnabled(
        row >= 0 && row < static_cast<int>(candidates.size()) && candidate_has_inspection(candidates[row]) &&
        !batch_active && !previewing && !promoting && !stopping && !closing);
    view_runner_log->setEnabled(
        row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].workspace &&
        QFileInfo::exists(
            QString::fromStdString((candidates[row].workspace->game_directory / "runner.log").string())) &&
        !batch_active && !previewing && !promoting && !stopping && !closing);
    shared_rotation->setEnabled(editing_batch);
    rotations->setEnabled(editing_batch && !shared_rotation->isChecked());
    prefer_player_frames->setEnabled(editing_batch);
    scan_duration->setEnabled(editing_batch && prefer_player_frames->isChecked());
    control_point_resolution->setEnabled(editing_batch && feature_settings_error.isEmpty() && feature_size_selectable);
    for (QWidget* input : std::array<QWidget*, 3>{control_points, frame_counts, start_frames})
      input->setEnabled(editing_batch);
    progress->setVisible(batch_started);
  }

  bool candidate_has_inspection(const Candidate& candidate) const {
    if (!candidate.workspace || candidate.selection_reuse_blocked)
      return false;
    return candidate.has_selected_frames ||
        QFileInfo::exists(
               QString::fromStdString((candidate.workspace->game_directory / "calibration-frame-inspection" /
                                       candidate.workspace->invalidation_id / "frames.yaml")
                                          .string()));
  }

  bool can_apply_selected() const {
    const int row = table->currentRow();
    return row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].complete &&
        !candidates[row].main_calibration && candidates[row].workspace && !candidates[row].selection_reuse_blocked &&
        !batch_active && !promotion_worker && !preparation_worker && !preview_process && pending_group_shutdowns == 0 &&
        !candidate_completion_pending && !preview_completion_pending;
  }

  StoredStitchingExperiment candidate_record(const Candidate& candidate, const std::string& state) const {
    StoredStitchingExperiment saved = candidate.stored.value_or(StoredStitchingExperiment{});
    saved.workspace = *candidate.workspace;
    saved.state = state;
    saved.requires_ice_mask = candidate.requires_ice_mask;
    saved.failure = candidate.failure.toStdString();
    saved.reservation_token = candidate.reservation_token.toStdString();
    if (!candidate.stored) {
      saved.sequence = candidate.sequence;
      saved.baseline_sequence = candidate.baseline_sequence;
      saved.selection_owner_sequence = candidate.selection_owner_sequence;
      saved.saved_selection_fingerprint = candidate.saved_selection_fingerprint;
      saved.scan_duration_seconds = candidate.scan_duration_seconds;
    }
    return saved;
  }

  bool ensure_session() {
    if (session && session->isValid())
      return true;
    const QString sessions = QString::fromStdString((store->directory / "sessions").string());
    if (!QDir().mkpath(sessions)) {
      show_status("Could not create the persistent experiment session directory.", true);
      return false;
    }
    session = std::make_unique<QTemporaryDir>(sessions + "/session-XXXXXX");
    session->setAutoRemove(false);
    if (!session->isValid()) {
      show_status("Could not create a private stitching experiment directory.", true);
      session.reset();
      return false;
    }
    return true;
  }

  void prepare_queued_candidates(int first_row) {
    struct Result {
      std::vector<Candidate> rows;
      absl::Status status;
    };
    auto result = std::make_shared<Result>();
    result->rows.assign(candidates.begin() + first_row, candidates.end());
    const auto source_game = game_directory.toStdString();
    const auto session_root = session->path().toStdString();
    const auto persistent = *store;
    std::map<int, std::string> workspace_keys;
    for (const Candidate& candidate : candidates) {
      if (candidate.workspace && candidate.stored)
        workspace_keys[candidate.sequence] =
            candidate.workspace->game_directory.lexically_relative(persistent.directory).generic_string();
    }
    QThread* worker = QThread::create([result, source_game, session_root, persistent, workspace_keys]() mutable {
      for (Candidate& candidate : result->rows) {
        if (!result->status.ok()) {
          candidate.failure = "Unavailable: an earlier queued workspace could not be prepared";
          candidate.queued = false;
          continue;
        }
        if ((candidate.baseline_sequence && !workspace_keys.count(candidate.baseline_sequence)) ||
            (candidate.selection_owner_sequence && candidate.selection_owner_sequence != candidate.sequence &&
             !workspace_keys.count(candidate.selection_owner_sequence))) {
          result->status = absl::FailedPreconditionError("A queued candidate dependency was not durably prepared");
          candidate.failure = QString::fromStdString(result->status.ToString());
          candidate.queued = false;
          continue;
        }
        auto workspace =
            CreateStitchingExperimentWorkspace(source_game, session_root, candidate.settings, candidate.sequence);
        if (!workspace.ok()) {
          candidate.failure = QString::fromStdString(workspace.status().ToString());
          candidate.queued = false;
          result->status = workspace.status();
          continue;
        }
        candidate.workspace = *workspace;
        candidate.settings = workspace->settings;
        // Add's earlier main lookup may race a config update before this copy.
        // Bind both the queue expectation and catalog to the snapshot we own.
        const auto fingerprint =
            ReusableStitchingExperimentSelectionFingerprint(workspace->game_directory, candidate.settings);
        if (!fingerprint.ok()) {
          candidate.failure = QString::fromStdString(fingerprint.status().ToString());
          candidate.queued = false;
          result->status = fingerprint.status();
          continue;
        }
        if (!candidate.saved_selection_fingerprint.empty() && *fingerprint != candidate.saved_selection_fingerprint) {
          result->status =
              absl::AbortedError("The saved frame selection changed while this candidate was being prepared");
          candidate.failure = QString::fromStdString(result->status.ToString());
          candidate.queued = false;
          continue;
        }
        candidate.saved_selection_fingerprint = *fingerprint;
        const auto workspace_key = workspace->game_directory.lexically_relative(persistent.directory).generic_string();
        StoredStitchingExperiment record;
        record.workspace = *workspace;
        record.state = "queued";
        record.sequence = candidate.sequence;
        record.baseline_sequence = candidate.baseline_sequence;
        record.selection_owner_sequence = candidate.selection_owner_sequence;
        if (candidate.baseline_sequence)
          record.baseline_workspace_key = workspace_keys.at(candidate.baseline_sequence);
        if (candidate.selection_owner_sequence)
          record.selection_owner_workspace_key = candidate.selection_owner_sequence == candidate.sequence
              ? workspace_key
              : workspace_keys.at(candidate.selection_owner_sequence);
        record.saved_selection_fingerprint = candidate.saved_selection_fingerprint;
        record.scan_duration_seconds = candidate.scan_duration_seconds;
        record.requires_ice_mask = candidate.requires_ice_mask;
        record.selection_fingerprint = candidate.saved_selection_fingerprint;
        const auto saved = SaveStitchingExperiment(persistent, record);
        if (!saved.ok()) {
          candidate.failure = QString::fromStdString(saved.ToString());
          candidate.queued = false;
          result->status = saved;
        } else {
          candidate.has_selected_frames = !record.selection_fingerprint.empty();
          candidate.stored = std::move(record);
          workspace_keys[candidate.sequence] = workspace_key;
        }
      }
    });
    preparation_worker = worker;
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, first_row]() {
      if (preparation_worker != worker)
        return;
      preparation_worker = nullptr;
      for (size_t index = 0; index < result->rows.size(); ++index) {
        const int row = first_row + static_cast<int>(index);
        candidates[row] = std::move(result->rows[index]);
        if (candidates[row].has_selected_frames)
          table->item(row, 0)->setText(QString("Players %1").arg(candidates[row].sequence));
        table->item(row, 5)->setText(candidates[row].queued ? "Queued" : candidates[row].failure);
        table->item(row, 7)->setText(candidate_image_size(candidates[row]));
      }
      if (!result->status.ok()) {
        store_error = QString::fromStdString(result->status.ToString());
        show_status("Could not persist the queued candidates: " + store_error, true);
      } else {
        show_status("Queued candidates saved in this game. Add more options or start the batch.");
      }
      update_controls();
      maybe_finish_close();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    show_status("Saving queued candidates and any retained calibration frames…");
    worker->start();
    update_controls();
  }

  absl::Status persist_candidate(Candidate& candidate, const std::string& state, bool publish_selection = false) {
    if (!store || !candidate.workspace || candidate.main_calibration)
      return absl::FailedPreconditionError("The persistent experiment workspace is unavailable");
    StoredStitchingExperiment saved = candidate_record(candidate, state);
    if (candidate.has_selected_frames && saved.selection_fingerprint.empty()) {
      auto fingerprint =
          ReusableStitchingExperimentSelectionFingerprint(candidate.workspace->game_directory, candidate.settings);
      if (!fingerprint.ok())
        return fingerprint.status();
      saved.selection_fingerprint = *fingerprint;
    }
    if (state == "running" || state == "scan") {
      saved.process_session_id = calibration_process_group;
      saved.process_token = calibration_process_token.toStdString();
    } else if (state != "quarantined") {
      saved.process_session_id = 0;
      saved.process_token.clear();
    }
    if (state == "complete") {
      auto lock = hm::stitching::HuginProject::RecoverAndLock(candidate.workspace->game_directory);
      if (!lock.ok())
        return lock.status();
      auto generation = hm::stitching::HuginProject::GenerationId(candidate.workspace->game_directory, **lock);
      if (!generation.ok())
        return generation.status();
      saved.artifact_generation_id = *generation;
    }
    const auto persisted =
        SaveStitchingExperiment(*store, saved, publish_selection, candidate.saved_selection_fingerprint);
    if (persisted.ok())
      candidate.stored = std::move(saved);
    return persisted;
  }

  absl::Status persist_calibration_process() {
    Candidate& candidate = candidates[running_candidate];
    const auto state = candidate.phase == CandidatePhase::kScan ? "scan" : "running";
    const auto saved = persist_candidate(candidate, state);
    if (!saved.ok())
      return saved;
    for (Candidate& owner : candidates) {
      if (owner.queued && owner.baseline_sequence == candidate.sequence &&
          owner.selection_owner_sequence == owner.sequence && !owner.reservation_token.isEmpty()) {
        const auto claimed = persist_candidate(owner, "running");
        if (!claimed.ok())
          return claimed;
      }
    }
    return absl::OkStatus();
  }

  absl::Status release_reservation(Candidate& candidate) {
    if (!store || !candidate.workspace || candidate.reservation_token.isEmpty() || candidate.selection_reuse_blocked)
      return absl::OkStatus();
    const auto released = ReleaseStitchingExperimentFrameCount(
        *store, candidate.settings.frame_count, *candidate.workspace, candidate.reservation_token.toStdString());
    if (released.ok())
      candidate.reservation_token.clear();
    return released;
  }

  void restore_persistent_store(const StitchingExperimentSettings& displayed_settings) {
    auto opened = OpenStitchingExperimentStore(game_directory.toStdString());
    if (!opened.ok()) {
      store_error = QString::fromStdString(opened.status().ToString());
      show_status(store_error, true);
      return;
    }
    store = *opened;
    cache_location->setText(
        "Frame and experiment cache: " + QString::fromStdString(store->directory.string()) +
        " — retained when this dialog closes.");
    auto catalog = LoadStitchingExperimentStore(*store);
    if (!catalog.ok()) {
      store_error = QString::fromStdString(catalog.status().ToString());
      show_status(store_error + " Use Discard experiments to explicitly remove this retained history.", true);
      return;
    }
    auto main = MainStitchingExperimentWorkspace(game_directory.toStdString(), displayed_settings);
    if (!main.ok()) {
      store_error = QString::fromStdString(main.status().ToString());
      show_status(store_error, true);
      return;
    }
    if (main->has_value()) {
      Candidate candidate;
      candidate.settings = (**main).settings;
      candidate.workspace = **main;
      candidate.sequence = ++next_candidate_sequence;
      candidate.queued = false;
      candidate.main_calibration = true;
      auto fingerprint = ReusableStitchingExperimentSelectionFingerprint((**main).game_directory, candidate.settings);
      if (fingerprint.ok()) {
        candidate.saved_selection_fingerprint = *fingerprint;
        candidate.has_selected_frames = !fingerprint->empty();
      } else
        candidate.failure = QString::fromStdString(fingerprint.status().ToString());
      // Listing and frame inspection stay bounded; Play validates all artifacts on a worker.
      candidate.complete = candidate.failure.isEmpty();
      append_candidate(std::move(candidate));
    }
    std::map<std::string, int> restored_sequences;
    for (const auto& record : catalog->experiments)
      restored_sequences[record.workspace.game_directory.lexically_relative(store->directory).generic_string()] =
          ++next_candidate_sequence;
    for (const auto& record : catalog->experiments) {
      Candidate candidate;
      candidate.settings = record.workspace.settings;
      candidate.workspace = record.workspace;
      candidate.sequence =
          restored_sequences[record.workspace.game_directory.lexically_relative(store->directory).generic_string()];
      candidate.queued = record.state == "queued";
      candidate.stored = record;
      candidate.saved_selection_fingerprint = record.saved_selection_fingerprint;
      candidate.scan_duration_seconds = record.scan_duration_seconds;
      candidate.baseline_sequence =
          record.baseline_workspace_key.empty() ? 0 : restored_sequences[record.baseline_workspace_key];
      candidate.selection_owner_sequence =
          record.selection_owner_workspace_key.empty() ? 0 : restored_sequences[record.selection_owner_workspace_key];
      candidate.reservation_token = QString::fromStdString(record.reservation_token);
      candidate.requires_ice_mask = record.requires_ice_mask;
      candidate.has_selected_frames = !record.selection_fingerprint.empty();
      candidate.failure = QString::fromStdString(record.failure);
      const bool pending_owner = record.state == "running" || record.state == "scan" || record.state == "quarantined" ||
          record.process_session_id != 0 || !record.process_token.empty();
      if (pending_owner &&
          ((record.process_session_id == 0 && record.process_token.empty()) ||
           experiment_session_alive(record.process_session_id, QString::fromStdString(record.process_token)))) {
        candidate.selection_reuse_blocked = true;
        candidate.failure = "Unavailable: previous process ownership is unconfirmed";
      } else {
        // The catalog is bounded. Exact source, image and artifact validation runs
        // when this row is inspected, reused, previewed, or promoted.
        candidate.complete =
            record.state == "complete" && !record.artifact_generation_id.empty() && !candidate.selection_reuse_blocked;
        bool recovered_selection = false;
        if (!candidate.queued && !candidate.has_selected_frames &&
            (pending_owner || record.state == "failed" || candidate.selection_owner_sequence == candidate.sequence)) {
          const auto fingerprint = recover_selection_fingerprint(*store, *candidate.workspace);
          if (!fingerprint.ok()) {
            candidate.selection_reuse_blocked = true;
            candidate.failure = "Unavailable: saved frame selection recovery failed; " +
                QString::fromStdString(fingerprint.status().ToString());
          } else if (!fingerprint->empty()) {
            auto recovered = candidate_record(candidate, candidate.complete ? "complete" : "frozen");
            recovered.selection_fingerprint = *fingerprint;
            recovered.process_session_id = 0;
            recovered.process_token.clear();
            const auto saved = SaveStitchingExperiment(*store, recovered, true);
            if (saved.ok()) {
              candidate.stored = std::move(recovered);
              candidate.queued = false;
              candidate.has_selected_frames = true;
              candidate.reservation_token.clear();
              candidate.failure = "Previous run stopped; saved frame selection recovered";
              catalog->selected_by_count[candidate.settings.frame_count] =
                  candidate.workspace->game_directory.lexically_relative(store->directory).generic_string();
              recovered_selection = true;
            } else {
              candidate.selection_reuse_blocked = true;
              candidate.failure =
                  "Unavailable: saved frame selection recovery failed; " + QString::fromStdString(saved.ToString());
            }
          }
        }
        // Process death and selected-input integrity are independent. Retain a
        // corrupt count's reservation, but clear confirmed-dead runner intent
        // so an explicit discard can still remove that unusable history.
        if (pending_owner && !recovered_selection) {
          if (!candidate.selection_reuse_blocked)
            candidate.failure =
                record.state == "complete" ? QString() : "Previous run stopped before durable completion";
          auto reconciled_record = *candidate.stored;
          const auto reconciled =
              MarkStitchingExperimentProcessStopped(*store, reconciled_record, candidate.failure.toStdString());
          if (reconciled.ok())
            candidate.stored = std::move(reconciled_record);
          if (!reconciled.ok()) {
            candidate.selection_reuse_blocked = true;
            candidate.failure += "; " + QString::fromStdString(reconciled.ToString());
          }
        }
        if (!candidate.selection_reuse_blocked && !candidate.reservation_token.isEmpty()) {
          const auto released = ReleaseStitchingExperimentFrameCount(
              *store, candidate.settings.frame_count, *candidate.workspace, candidate.reservation_token.toStdString());
          if (!released.ok()) {
            candidate.selection_reuse_blocked = true;
            candidate.failure += "; " + QString::fromStdString(released.ToString());
          } else
            candidate.reservation_token.clear();
        }
      }
      if (candidate.selection_reuse_blocked && !candidate.has_selected_frames)
        candidate.selection_owner_sequence = candidate.sequence;
      if (!candidate.queued && !candidate.selection_reuse_blocked &&
          candidate.selection_owner_sequence == candidate.sequence)
        candidate.selection_owner_sequence = 0;
      const auto selected = catalog->selected_by_count.find(candidate.settings.frame_count);
      const auto retained = catalog->retained_by_count.find(candidate.settings.frame_count);
      const std::string owner_key = selected != catalog->selected_by_count.end() ? selected->second
          : retained != catalog->retained_by_count.end()                         ? retained->second
                                                                                 : std::string();
      if (record.workspace.game_directory.lexically_relative(store->directory).generic_string() == owner_key)
        candidate.selection_owner_sequence = candidate.sequence;
      append_candidate(std::move(candidate));
    }
    if (!candidates.empty())
      table->selectRow(0);
  }

  void append_output(QProcess* process) {
    if (!process)
      return;
    const QString output = QString::fromUtf8(process->readAllStandardOutput());
    if (!output.isEmpty()) {
      const int output_row = process == calibration_process.get() ? running_candidate : preview_candidate_row;
      if (output_row >= 0 && output_row < static_cast<int>(candidates.size()) && candidates[output_row].workspace) {
        QFile retained(
            QString::fromStdString((candidates[output_row].workspace->game_directory / "runner.log").string()));
        const QByteArray bytes = output.toUtf8();
        if (!retained.open(QIODevice::WriteOnly | QIODevice::Append) || retained.write(bytes) != bytes.size())
          show_status("Could not retain experiment runner output: " + retained.errorString(), true);
      }
      if (process == calibration_process.get() && running_candidate >= 0 &&
          running_candidate < static_cast<int>(candidates.size())) {
        QString& tail = candidates[running_candidate].process_output_tail;
        tail = (tail + output).right(64 * 1024);
      }
      log->moveCursor(QTextCursor::End);
      log->insertPlainText(output);
      log->moveCursor(QTextCursor::End);
    }
  }

  QString candidate_process_error(const Candidate& candidate) const {
    const QRegularExpression error(
        "(?:FAILED_PRECONDITION|INVALID_ARGUMENT|ABORTED|INTERNAL|RESOURCE_EXHAUSTED|NOT_FOUND|UNAVAILABLE|DATA_LOSS|CANCELLED):[^\\r\\n]+|"
        "ERROR from pipeline:[^\\r\\n]+|HSTREAM_CALIBRATION[^\\r\\n]*status=error[^\\r\\n]*message=([^\\r\\n]+)");
    auto matches = error.globalMatch(candidate.process_output_tail);
    QString result;
    while (matches.hasNext()) {
      const auto match = matches.next();
      result = match.captured(1).isEmpty() ? match.captured(0) : match.captured(1);
    }
    return result.left(1500);
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

  Candidate* selection_owner_for(const Candidate& candidate) {
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& owner) {
      return owner.sequence == candidate.selection_owner_sequence;
    });
    return found == candidates.end() ? nullptr : &*found;
  }

  std::filesystem::path scan_report_path(const Candidate& candidate) const {
    return candidate.workspace->root / ("player-scan-" + std::to_string(candidate.sequence) + ".yaml");
  }

  void finalize_candidate_completion() {
    if (!candidate_completion_pending)
      return;
    const uint64_t generation = process_generation;
    QTimer::singleShot(0, dialog, [this, generation]() {
      if (!candidate_completion_pending || generation != process_generation)
        return;
      Candidate& finished = candidates[pending_candidate_row];
      finished.selection_reuse_blocked = current_candidate_workspace_unsafe;
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
        else if (pending_candidate_exit_code != 0) {
          finished.failure = candidate_process_error(finished);
          if (finished.failure.isEmpty())
            finished.failure = QString("Runner exited %1").arg(pending_candidate_exit_code);
        } else if (!configured.ok())
          finished.failure = QString::fromStdString(configured.ToString());
        else
          finished.failure = "Calibration failed";
        table->item(pending_candidate_row, 5)->setText(finished.failure);
        const QString exit_detail = QString("Runner exited %1%2")
                                        .arg(pending_candidate_exit_code)
                                        .arg(pending_candidate_exit_status == QProcess::CrashExit ? " (crashed)" : "");
        table->item(pending_candidate_row, 5)->setToolTip(finished.failure + "\n" + exit_detail);
        append_output_message(
            QString("Candidate %1: %2; %3\n").arg(finished.sequence).arg(finished.failure).arg(exit_detail));
        show_status(finished.failure, true);
      }
      const std::string saved_state = current_candidate_workspace_unsafe ? "quarantined"
          : finished.complete                                            ? "complete"
          : continue_with_solve                                          ? "frozen"
                                                                         : "failed";
      const auto persisted =
          persist_candidate(finished, saved_state, finished.has_selected_frames && !current_candidate_workspace_unsafe);
      if (!persisted.ok()) {
        finished.complete = false;
        finished.selection_reuse_blocked = true;
        finished.failure = "Could not save candidate state: " + QString::fromStdString(persisted.ToString());
        continue_with_solve = false;
        cancelling = true;
        table->item(finished.row, 5)->setText(finished.failure);
        show_status(finished.failure, true);
      }
      if (!continue_with_solve && !finished.has_selected_frames && !current_candidate_workspace_unsafe)
        (void)release_reservation(finished);
      for (Candidate& owner : candidates) {
        if (owner.queued && owner.baseline_sequence == finished.sequence &&
            owner.selection_owner_sequence == owner.sequence && !owner.reservation_token.isEmpty()) {
          owner.selection_reuse_blocked = current_candidate_workspace_unsafe;
          const auto saved_owner =
              persist_candidate(owner, current_candidate_workspace_unsafe ? "quarantined" : "queued");
          if (!saved_owner.ok()) {
            owner.selection_reuse_blocked = true;
            cancelling = true;
            show_status(QString::fromStdString(saved_owner.ToString()), true);
          }
        }
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
      const auto persisted = persist_preview_process(true);
      if (!persisted.ok())
        show_status(QString::fromStdString(persisted.ToString()), true);
      preview_process.reset();
      preview_process_group = 0;
      preview_process_token.clear();
      preview_completion_pending = false;
      preview_candidate_row = -1;
      video->set_renderer_active(false);
      update_controls();
      if (replay)
        start_preview(true);
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
          candidate.selection_reuse_blocked = true;
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
    if (calibrating || previewing || promotion_worker || preparation_worker || pending_group_shutdowns > 0 ||
        candidate_completion_pending || preview_completion_pending || close_completion_scheduled)
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
    while (running_candidate < static_cast<int>(candidates.size()) &&
           (!candidates[running_candidate].queued || (batch_only_row >= 0 && running_candidate != batch_only_row)))
      ++running_candidate;
    if (cancelling || closing || running_candidate >= static_cast<int>(candidates.size())) {
      calibration_process.reset();
      calibration_process_group = 0;
      calibration_process_token.clear();
      batch_active = false;
      for (Candidate& pending : candidates) {
        if (!pending.queued || (batch_only_row >= 0 && pending.row != batch_only_row))
          continue;
        if (!pending.selection_reuse_blocked) {
          (void)release_reservation(pending);
          pending.queued = false;
          pending.failure = "Cancelled";
          const auto saved = persist_candidate(pending, "failed");
          if (!saved.ok())
            show_status(QString::fromStdString(saved.ToString()), true);
        }
        table->item(pending.row, 5)
            ->setText(pending.selection_reuse_blocked ? "Unavailable: process ownership unconfirmed" : "Cancelled");
      }
      progress->setValue(std::min(running_candidate, static_cast<int>(candidates.size())));
      show_status(
          cancelling || closing
              ? "Batch cancelled. Completed candidates remain available; the main Program is unchanged."
              : "Batch complete. Select a successful row and preview the seam in motion.");
      cancelling = false;
      batch_only_row = -1;
      update_controls();
      if (open_editor_after_row >= 0 && !closing) {
        const int edit_row = std::exchange(open_editor_after_row, -1);
        // Matching snapshots remain useful when subsequent geometry failed.
        if (candidate_has_inspection(candidates[edit_row]))
          QTimer::singleShot(0, dialog, [this, edit_row]() { open_match_editor(edit_row); });
      }
      return;
    }
    Candidate& candidate = candidates[running_candidate];
    candidate.queued = false;
    if (!candidate.has_selected_frames && candidate.baseline_sequence != 0 &&
        candidate.selection_owner_sequence == candidate.sequence) {
      Candidate* baseline = baseline_for(candidate);
      if (!baseline || !baseline->complete || !baseline->workspace) {
        candidate.failure = "Unavailable: baseline calibration failed";
        (void)persist_candidate(candidate, "failed");
        (void)release_reservation(candidate);
        table->item(candidate.row, 5)->setText(candidate.failure);
        QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
        return;
      }
      candidate.phase = CandidatePhase::kScan;
    }
    if (!candidate.workspace || !candidate.stored) {
      candidate.failure = "Unavailable: this queued candidate was not durably prepared";
      table->item(candidate.row, 5)->setText(candidate.failure);
      QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
      return;
    }
    std::optional<StitchingExperimentWorkspace> owner_workspace;
    if (!candidate.has_selected_frames && candidate.selection_owner_sequence != 0 &&
        candidate.selection_owner_sequence != candidate.sequence) {
      Candidate* owner = selection_owner_for(candidate);
      if (!owner || !owner->has_selected_frames || !owner->workspace || owner->selection_reuse_blocked) {
        candidate.failure = "Unavailable: the shared frame selection was not safely frozen";
        (void)persist_candidate(candidate, "failed");
        table->item(candidate.row, 5)->setText(candidate.failure);
        QTimer::singleShot(0, dialog, [this]() { launch_next_candidate(); });
        return;
      }
      owner_workspace = *owner->workspace;
    }
    const auto workspace = *candidate.workspace;
    const auto expected_fingerprint = candidate.saved_selection_fingerprint;
    auto result = std::make_shared<absl::StatusOr<std::string>>(absl::UnknownError("Frame preparation did not finish"));
    QThread* worker = QThread::create([workspace, owner_workspace, expected_fingerprint, result]() {
      auto fingerprint = ReusableStitchingExperimentSelectionFingerprint(workspace.game_directory, workspace.settings);
      if (!fingerprint.ok()) {
        *result = fingerprint.status();
        return;
      }
      if (*fingerprint != expected_fingerprint) {
        *result = absl::AbortedError("The saved frame selection changed after this candidate was queued");
        return;
      }
      if (owner_workspace) {
        const auto reused = ReuseStitchingExperimentPlayerSelection(*owner_workspace, workspace);
        if (!reused.ok()) {
          *result = reused;
          return;
        }
        fingerprint = ReusableStitchingExperimentSelectionFingerprint(workspace.game_directory, workspace.settings);
      }
      *result = std::move(fingerprint);
    });
    preparation_worker = worker;
    table->item(candidate.row, 5)->setText("Preparing saved frames…");
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, row = candidate.row]() {
      if (preparation_worker != worker)
        return;
      preparation_worker = nullptr;
      Candidate& prepared = candidates[row];
      if (!result->ok() || cancelling || closing) {
        prepared.failure = result->ok() ? "Cancelled" : QString::fromStdString(result->status().ToString());
        if (!result->ok()) {
          // A changed main selection can enter the workspace during Add. Do
          // not free its count when preparation discovers those frozen inputs.
          const auto fingerprint = recover_selection_fingerprint(*store, *prepared.workspace);
          if (!fingerprint.ok() || !fingerprint->empty()) {
            prepared.selection_reuse_blocked = true;
            prepared.selection_owner_sequence = prepared.sequence;
            prepared.failure = "Unavailable: saved frame selection requires recovery; " + prepared.failure;
          }
        }
        const auto persisted = persist_candidate(prepared, "failed");
        if (!persisted.ok())
          prepared.failure += "; " + QString::fromStdString(persisted.ToString());
        const auto released = release_reservation(prepared);
        if (!released.ok())
          prepared.failure += "; " + QString::fromStdString(released.ToString());
        table->item(row, 5)->setText(prepared.failure);
        launch_next_candidate();
        maybe_finish_close();
        return;
      }
      if (!(**result).empty()) {
        prepared.has_selected_frames = true;
        prepared.phase = CandidatePhase::kCalibration;
        append_output_message(
            QString("Candidate %1 reuses %2 saved frame pairs (selection %3); solving without a player scan.\n")
                .arg(prepared.sequence)
                .arg(prepared.settings.frame_count)
                .arg(QString::fromStdString(**result)));
      }
      launch_candidate_phase();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    update_controls();
  }

  void launch_candidate_phase() {
    if (cancelling || closing) {
      launch_next_candidate();
      return;
    }
    Candidate& candidate = candidates[running_candidate];
    const bool scanning = candidate.phase == CandidatePhase::kScan;
    candidate.process_output_tail.clear();
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
      env.insert("HSTREAM_CALIBRATION_START_STAGE", "features");
      env.insert("HSTREAM_CALIBRATION_INVALIDATION_ID", QString::fromStdString(candidate.workspace->invalidation_id));
      env.insert("HSTREAM_STITCH_CALIBRATION_INSPECTION", "1");
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
      if (uses_process_group && generation == process_generation) {
        calibration_process_group = calibration_process->processId();
        const auto persisted = persist_calibration_process();
        if (!persisted.ok()) {
          show_status("Could not record runner ownership: " + QString::fromStdString(persisted.ToString()), true);
          cancelling = true;
          stop_calibration_process();
        }
      }
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
    const auto intended = persist_calibration_process();
    if (!intended.ok()) {
      candidate.failure = "Could not save runner ownership: " + QString::fromStdString(intended.ToString());
      candidate.selection_reuse_blocked = true;
      cancelling = true;
      table->item(candidate.row, 5)->setText(candidate.failure);
      calibration_process.reset();
      calibration_process_token.clear();
      launch_next_candidate();
      return;
    }
    calibration_process->start();
    update_controls();
  }

  static bool same_settings(const StitchingExperimentSettings& left, const StitchingExperimentSettings& right) {
    return left.control_points == right.control_points && left.frame_count == right.frame_count &&
        hm::stitch_frame_time_to_nanoseconds(left.stitch_frame_time) ==
        hm::stitch_frame_time_to_nanoseconds(right.stitch_frame_time) &&
        left.rink_rotation_degrees == right.rink_rotation_degrees &&
        left.control_point_resolution == right.control_point_resolution &&
        left.manual_control_points == right.manual_control_points;
  }

  QString candidate_image_size(const Candidate& candidate) const {
    try {
      YAML::Node effective = YAML::Clone(feature_defaults);
      if (candidate.workspace) {
        const auto config = hm::stitching::load_game_config_file(candidate.workspace->game_directory / "config.yaml");
        if (!config.ok() || !config->has_value())
          return "Unavailable";
        overlay_feature_settings(effective, **config);
        const QString fixed = fixed_feature_size(feature_matcher(effective));
        if (!fixed.isEmpty())
          return fixed;
      } else {
        // Pending additions use the same matcher as the current editor.
        if (!feature_size_selectable)
          return control_point_resolution->currentText();
      }
      if (!candidate.settings.control_point_resolution)
        return "Inherited";
      const auto& resolution = *candidate.settings.control_point_resolution;
      return resolution == "native" ? "Native" : QString::fromStdString(resolution).toUpper();
    } catch (const std::exception&) {
      return "Unavailable";
    }
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
        candidate.main_calibration           ? QString("Main calibration")
            : settings.manual_control_points ? QString("Manual %1").arg(candidate.sequence)
            : candidate.has_selected_frames || candidate.selection_owner_sequence != 0 ||
                !candidate.saved_selection_fingerprint.empty()
            ? QString("Players %1").arg(candidate.sequence)
            : QString(candidate.requires_ice_mask ? "Baseline %1" : "Candidate %1").arg(candidate.sequence),
        settings.manual_control_points ? QString("Edited") : QString::number(settings.control_points),
        QString::number(settings.frame_count),
        QString::fromStdString(settings.stitch_frame_time),
        rotation,
        candidate.queued                   ? "Queued"
            : candidate.complete           ? "Ready"
            : !candidate.failure.isEmpty() ? candidate.failure
                                           : "Saved frame set",
        frame_selection_policy(candidate).first,
        candidate_image_size(candidate),
    };
    for (int column = 0; column < columns.size(); ++column) {
      auto* item = new QTableWidgetItem(columns[column]);
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      table->setItem(row, column, item);
    }
    table->item(row, 6)->setToolTip(frame_selection_policy(candidate).second);
    if (settings.manual_control_points)
      table->item(row, 1)->setToolTip(
          "Uses every saved manual match across all pairs; the automatic per-pair cap does not apply.");
    candidates.push_back(std::move(candidate));
  }

  void add_candidates_to_batch() {
    if (batch_active || preparation_worker || promotion_worker || closing)
      return;
    if (!feature_settings_error.isEmpty()) {
      show_status(feature_settings_error, true);
      return;
    }
    if (!store) {
      show_status(store_error.isEmpty() ? "The persistent experiment cache is unavailable." : store_error, true);
      return;
    }
    QString error;
    auto points = parse_positive_list(
        control_points->text(), hm::stitching::kMinimumCalibrationControlPoints, 5000, "Control-point counts", &error);
    auto frames = parse_positive_list(frame_counts->text(), 1, 16, "Frame counts", &error);
    auto starts = parse_time_list(start_frames->text(), &error);
    std::optional<std::vector<std::array<double, 3>>> rink_rotations;
    if (!shared_rotation->isChecked())
      rink_rotations = parse_rotations(rotations->text(), &error);
    if (!points || !frames || !starts || (!shared_rotation->isChecked() && !rink_rotations)) {
      show_status(error, true);
      return;
    }
    QFile source_config(QDir(game_directory).filePath("config.yaml"));
    constexpr qint64 kMaximumConfigBytes = 4 * 1024 * 1024;
    if (!source_config.open(QIODevice::ReadOnly)) {
      show_status("Could not read the source game configuration: " + source_config.errorString(), true);
      return;
    }
    const QByteArray config_bytes = source_config.read(kMaximumConfigBytes + 1);
    if (source_config.error() != QFileDevice::NoError || config_bytes.size() > kMaximumConfigBytes) {
      show_status("Could not read the source game configuration within the 4 MiB limit.", true);
      return;
    }
    const QByteArray config_revision = QCryptographicHash::hash(config_bytes, QCryptographicHash::Sha256);
    const auto catalog = LoadStitchingExperimentStore(*store);
    if (!catalog.ok()) {
      show_status(QString::fromStdString(catalog.status().ToString()), true);
      return;
    }
    const size_t rotation_count = shared_rotation->isChecked() ? 1 : rink_rotations->size();
    std::vector<Candidate> additions;
    const auto append_addition = [&](Candidate candidate) {
      candidate.requested_config_revision = config_revision;
      additions.push_back(std::move(candidate));
    };
    std::set<int> required_baselines;
    int sequence = next_candidate_sequence;
    const auto find = [&](const auto& matches) -> const Candidate* {
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
                .control_point_resolution = control_point_resolution->currentData().toString().toStdString(),
            };
            auto saved = ReusableStitchingExperimentSelectionFingerprint(game_directory.toStdString(), settings);
            if (!saved.ok()) {
              show_status(QString::fromStdString(saved.status().ToString()), true);
              return;
            }
            const auto cached = catalog->selected_by_count.find(frame_count);
            const auto retained = catalog->retained_by_count.find(frame_count);
            const std::string owner_key = cached != catalog->selected_by_count.end() ? cached->second
                : retained != catalog->retained_by_count.end()                       ? retained->second
                                                                                     : std::string();
            const Candidate* owner = find([&](const Candidate& item) {
              if (item.main_calibration || item.settings.frame_count != frame_count)
                return false;
              if (!owner_key.empty())
                return item.workspace &&
                    item.workspace->game_directory.lexically_relative(store->directory).generic_string() == owner_key;
              return item.selection_owner_sequence == item.sequence && item.saved_selection_fingerprint.empty() &&
                  !item.has_selected_frames && (item.queued || item.selection_reuse_blocked);
            });
            if (saved->empty() && !owner_key.empty() && !owner) {
              show_status("Another dialog saved this frame count. Reopen experiments to reuse its frames.", true);
              return;
            }
            if (saved->empty() && owner && owner->selection_reuse_blocked) {
              show_status("This frame count is unavailable: " + owner->failure, true);
              return;
            }
            const Candidate* same_count = owner ? owner : find([&](const Candidate& item) {
              return item.settings.frame_count == frame_count && !item.main_calibration && item.queued;
            });
            if (saved->empty() && same_count &&
                hm::stitch_frame_time_to_nanoseconds(same_count->settings.stitch_frame_time) !=
                    hm::stitch_frame_time_to_nanoseconds(settings.stitch_frame_time)) {
              show_status(
                  "Candidates with the same frame count share one frame set and reference time. "
                  "Restore that reference or choose a different frame count.",
                  true);
              return;
            }
            const bool selected = !saved->empty() || owner || (prefer_player_frames->isChecked() && frame_count > 1);
            const Candidate* duplicate = find([&](const Candidate& item) {
              const bool item_selected = item.has_selected_frames || item.selection_owner_sequence != 0 ||
                  !item.saved_selection_fingerprint.empty();
              return !item.main_calibration && (item.queued || item.complete || item.has_selected_frames) &&
                  item.requested_config_revision == config_revision && same_settings(item.settings, settings) &&
                  item_selected == selected;
            });
            if (duplicate)
              continue;
            if (!saved->empty()) {
              append_addition(
                  Candidate{.settings = settings, .sequence = ++sequence, .saved_selection_fingerprint = *saved});
            } else if (owner) {
              const int baseline_sequence = owner->baseline_sequence;
              const int owner_sequence = owner->sequence;
              const int duration = owner->scan_duration_seconds;
              if (owner->queued && !owner->has_selected_frames)
                required_baselines.insert(baseline_sequence);
              append_addition(
                  Candidate{
                      .settings = settings,
                      .sequence = ++sequence,
                      .baseline_sequence = baseline_sequence,
                      .selection_owner_sequence = owner_sequence,
                      .scan_duration_seconds = duration});
            } else if (selected) {
              const Candidate* baseline = find([&](const Candidate& item) {
                return !item.main_calibration && (item.queued || (item.complete && item.requires_ice_mask)) &&
                    item.settings.frame_count == frame_count && item.selection_owner_sequence == 0 &&
                    item.saved_selection_fingerprint.empty();
              });
              const int baseline_sequence = baseline ? baseline->sequence : ++sequence;
              if (!baseline)
                append_addition(Candidate{.settings = settings, .sequence = baseline_sequence});
              required_baselines.insert(baseline_sequence);
              const int owner_sequence = ++sequence;
              append_addition(
                  Candidate{
                      .settings = settings,
                      .sequence = owner_sequence,
                      .baseline_sequence = baseline_sequence,
                      .selection_owner_sequence = owner_sequence,
                      .scan_duration_seconds = scan_duration->value()});
            } else {
              append_addition(Candidate{.settings = settings, .sequence = ++sequence});
            }
            if (std::count_if(candidates.begin(), candidates.end(), [](const Candidate& item) { return item.queued; }) +
                    additions.size() >
                64) {
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
    if (!ensure_session())
      return;
    const int first_new_row = static_cast<int>(candidates.size());
    for (Candidate& baseline : candidates) {
      if (required_baselines.count(baseline.sequence)) {
        baseline.requires_ice_mask = true;
        table->item(baseline.row, 0)->setText(QString("Baseline %1").arg(baseline.sequence));
        if (baseline.stored) {
          const auto saved = persist_candidate(baseline, baseline.stored->state);
          if (!saved.ok()) {
            show_status(QString::fromStdString(saved.ToString()), true);
            return;
          }
        }
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
    prepare_queued_candidates(first_new_row);
  }

  void remove_selected_from_batch() {
    const int row = table->currentRow();
    if (batch_active || preparation_worker || promotion_worker || preview_process || pending_group_shutdowns ||
        closing || !store || row < 0 || row >= static_cast<int>(candidates.size()) ||
        candidates[row].main_calibration || candidates[row].complete ||
        (!candidates[row].queued && candidates[row].failure.isEmpty()))
      return;
    // Follow dependencies transitively, including rows added in later sessions.
    std::set<int> sequences{candidates[row].sequence};
    size_t previous_size;
    do {
      previous_size = sequences.size();
      for (const Candidate& candidate : candidates)
        if (sequences.count(candidate.baseline_sequence) || sequences.count(candidate.selection_owner_sequence))
          sequences.insert(candidate.sequence);
    } while (previous_size != sequences.size());
    std::vector<std::string> removed_keys;
    for (Candidate& candidate : candidates) {
      if (sequences.count(candidate.sequence)) {
        if (candidate.main_calibration || candidate.complete || (!candidate.queued && candidate.failure.isEmpty())) {
          show_status(
              "This attempt has successful dependents. Use Discard experiments to remove the entire history.", true);
          return;
        }
        // Preparation can fail before a workspace is published. Such rows have
        // no retained files; remove them from the table alongside durable rows.
        if (!candidate.workspace)
          continue;
        if (!candidate.stored) {
          show_status(
              "This attempt's files were not cataloged. Use Discard experiments to remove its retained files.", true);
          return;
        }
        const auto released = release_reservation(candidate);
        if (!released.ok()) {
          show_status(QString::fromStdString(released.ToString()), true);
          return;
        }
        removed_keys.push_back(
            candidate.workspace->game_directory.lexically_relative(store->directory).generic_string());
      }
    }
    const auto removed = removed_keys.empty() ? absl::OkStatus() : RemoveStitchingExperiments(*store, removed_keys);
    if (!removed.ok()) {
      candidates.clear();
      table->setRowCount(0);
      next_candidate_sequence = 0;
      restore_persistent_store(initial_settings);
      show_status(QString::fromStdString(removed.ToString()), true);
      update_controls();
      return;
    }
    // Removing a baseline also removes every dependent automatic candidate;
    // stable sequence identities keep other dependencies intact as rows shift.
    for (int index = static_cast<int>(candidates.size()) - 1; index >= 0; --index) {
      if (sequences.count(candidates[index].sequence)) {
        candidates.erase(candidates.begin() + index);
        table->removeRow(index);
      }
    }
    for (int index = 0; index < static_cast<int>(candidates.size()); ++index) {
      Candidate& candidate = candidates[index];
      candidate.row = index;
      if (candidate.baseline_sequence == 0 && candidate.queued) {
        candidate.requires_ice_mask = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& item) {
          return item.baseline_sequence == candidate.sequence;
        });
        const auto saved = persist_candidate(candidate, "queued");
        if (!saved.ok())
          show_status(QString::fromStdString(saved.ToString()), true);
        table->item(index, 0)->setText(QString(
                                           !candidate.saved_selection_fingerprint.empty() ? "Players %1"
                                               : candidate.requires_ice_mask              ? "Baseline %1"
                                                                                          : "Candidate %1")
                                           .arg(candidate.sequence));
      }
    }
    if (!store_error.isEmpty() &&
        std::none_of(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& item) { return !item.main_calibration && !item.stored && !item.failure.isEmpty(); }) &&
        LoadStitchingExperimentStore(*store).ok())
      store_error.clear();
    if (!candidates.empty()) {
      table->selectRow(std::min(row, static_cast<int>(candidates.size()) - 1));
      show_status(QString("Removed the candidate. %1 remain in the batch.").arg(candidates.size()));
    } else {
      session.reset();
      next_candidate_sequence = 0;
      show_status("Removed the last candidate and its private files. The main calibration is unchanged.");
    }
    update_controls();
  }

  void clear_candidate_batch() {
    if (!store || batch_active || preparation_worker || promotion_worker || preview_process || pending_group_shutdowns)
      return;
    QMessageBox prompt(
        QMessageBox::Warning,
        "Discard stitching experiments",
        "Delete all experiment rows, frame caches, candidate configurations, previews, and runner logs for this game?",
        QMessageBox::NoButton,
        dialog);
    prompt.setObjectName("stitchExperimentDiscardGuard");
    prompt.setInformativeText(
        "The main game's promoted calibration and its saved frame bundle are preserved. This deletion cannot be undone.");
    auto* discard = prompt.addButton("Discard experiments and cache", QMessageBox::DestructiveRole);
    auto* keep = prompt.addButton("Cancel", QMessageBox::RejectRole);
    discard->setIcon(action_icon(ActionIcon::Delete));
    keep->setIcon(action_icon(ActionIcon::Cancel));
    discard->setObjectName("stitchExperimentDiscardConfirm");
    prompt.setDefaultButton(keep);
    prompt.setEscapeButton(keep);
    prompt.exec();
    if (prompt.clickedButton() != discard)
      return;
    const auto persistent = *store;
    auto result = std::make_shared<absl::Status>();
    QThread* worker =
        QThread::create([persistent, result]() { *result = DiscardStitchingExperimentStore(persistent); });
    preparation_worker = worker;
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result]() {
      if (preparation_worker != worker)
        return;
      preparation_worker = nullptr;
      if (!result->ok()) {
        show_status("Could not discard experiments: " + QString::fromStdString(result->ToString()), true);
      } else {
        candidates.clear();
        table->setRowCount(0);
        session.reset();
        store.reset();
        store_error.clear();
        session_retained = false;
        next_candidate_sequence = 0;
        running_candidate = -1;
        batch_started = false;
        results_applied = false;
        restore_persistent_store(initial_settings);
        show_status("Experiment history and caches deleted. The main calibration is preserved.");
      }
      update_controls();
      maybe_finish_close();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    show_status("Discarding experiment history and caches…");
    worker->start();
    update_controls();
  }

  void start_candidate_batch(int only_row = -1) {
    if (!store || batch_active ||
        !std::any_of(candidates.begin(), candidates.end(), [](const Candidate& item) { return item.queued; }))
      return;
    if (preparation_worker || !ensure_session())
      return;
    for (Candidate& owner : candidates) {
      if (!owner.queued || (only_row >= 0 && owner.row != only_row) ||
          owner.selection_owner_sequence != owner.sequence || owner.has_selected_frames)
        continue;
      if (owner.reservation_token.isEmpty())
        owner.reservation_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
      auto saved = persist_candidate(owner, "queued");
      if (!saved.ok()) {
        show_status(QString::fromStdString(saved.ToString()), true);
        return;
      }
      const auto reserved = ReserveStitchingExperimentFrameCount(
          *store,
          owner.settings.frame_count,
          hm::stitch_frame_time_to_nanoseconds(owner.settings.stitch_frame_time),
          *owner.workspace,
          owner.reservation_token.toStdString());
      if (!reserved.ok() || reserved->has_value()) {
        for (Candidate& pending : candidates)
          if (pending.queued && !pending.reservation_token.isEmpty())
            (void)release_reservation(pending);
        show_status(
            reserved.ok() ? "Another dialog already selected this frame count. Reopen this dialog to reuse it."
                          : QString::fromStdString(reserved.status().ToString()),
            true);
        return;
      }
    }
    batch_started = true;
    session->setAutoRemove(false);
    batch_active = true;
    running_candidate = -1;
    batch_only_row = only_row;
    cancelling = false;
    progress->setRange(0, static_cast<int>(candidates.size()));
    progress->setValue(0);
    show_status(QString("Starting %1 queued calibration candidates…").arg(candidates.size()));
    update_controls();
    launch_next_candidate();
  }

  absl::Status persist_preview_process(bool stopped = false) {
    if (preview_candidate_row < 0 || !store)
      return absl::FailedPreconditionError("The preview candidate is unavailable");
    Candidate& candidate = candidates[preview_candidate_row];
    auto saved = candidate_record(candidate, candidate.selection_reuse_blocked ? "quarantined" : "complete");
    if (stopped && !candidate.selection_reuse_blocked) {
      saved.process_session_id = 0;
      saved.process_token.clear();
    } else if (!stopped) {
      saved.process_session_id = preview_process_group;
      saved.process_token = preview_process_token.toStdString();
    }
    const auto persisted = SaveStitchingExperiment(*store, saved);
    if (persisted.ok())
      candidate.stored = std::move(saved);
    return persisted;
  }

  void start_preview(bool validated = false) {
    if (preview_process && preview_process->state() != QProcess::NotRunning)
      return;
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].complete ||
        candidates[row].main_calibration || preparation_worker)
      return;
    if (!validated) {
      const auto workspace = *candidates[row].workspace;
      const std::string expected =
          candidates[row].stored ? candidates[row].stored->artifact_generation_id : std::string();
      auto result = std::make_shared<absl::Status>();
      QThread* worker = QThread::create([workspace, expected, result]() {
        auto lock = hm::stitching::HuginProject::RecoverAndLock(workspace.game_directory);
        if (!lock.ok()) {
          *result = lock.status();
          return;
        }
        auto generation = hm::stitching::HuginProject::GenerationId(workspace.game_directory, **lock);
        if (!generation.ok())
          *result = generation.status();
        else if (!expected.empty() && *generation != expected)
          *result = absl::AbortedError("The saved candidate artifact generation changed");
      });
      preparation_worker = worker;
      QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, row]() {
        if (preparation_worker != worker)
          return;
        preparation_worker = nullptr;
        if (!result->ok()) {
          candidates[row].complete = false;
          candidates[row].failure = QString::fromStdString(result->ToString());
          table->item(row, 5)->setText(candidates[row].failure);
          show_status(candidates[row].failure, true);
        } else if (!closing)
          start_preview(true);
        update_controls();
        maybe_finish_close();
      });
      QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
      show_status("Validating saved calibration artifacts before preview…");
      worker->start();
      update_controls();
      return;
    }
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
      if (uses_process_group) {
        preview_process_group = preview_process->processId();
        const auto persisted = persist_preview_process();
        if (!persisted.ok()) {
          show_status(QString::fromStdString(persisted.ToString()), true);
          stop_preview_process();
        }
      }
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
    const auto intended = persist_preview_process();
    if (!intended.ok()) {
      show_status("Could not save preview ownership: " + QString::fromStdString(intended.ToString()), true);
      preview_process.reset();
      preview_process_token.clear();
      preview_candidate_row = -1;
      update_controls();
      return;
    }
    video->set_renderer_active(true);
    preview_process->start();
    update_controls();
  }

  void apply_selected() {
    const int row = table->currentRow();
    if (!can_apply_selected())
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
        results_applied = true;
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

  void show_selected_runner_log() {
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].workspace)
      return;
    const QString path = QString::fromStdString((candidates[row].workspace->game_directory / "runner.log").string());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      show_status("Could not read retained runner output: " + file.errorString(), true);
      return;
    }
    constexpr qint64 maximum = 1024 * 1024;
    const bool truncated = file.size() > maximum;
    if (truncated)
      file.seek(file.size() - maximum);
    const QByteArray bytes = file.read(maximum);
    QDialog viewer(dialog);
    viewer.setObjectName("stitchExperimentRunnerLogViewer");
    viewer.setWindowTitle(QString("Runner output — Candidate %1").arg(candidates[row].sequence));
    viewer.resize(1050, 650);
    auto* layout = new QVBoxLayout(&viewer);
    auto* location = new QLabel((truncated ? "Showing the last 1 MiB of " : "") + path);
    location->setTextFormat(Qt::PlainText);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location->setWordWrap(true);
    layout->addWidget(location);
    auto* contents = new QPlainTextEdit(QString::fromUtf8(bytes));
    contents->setObjectName("stitchExperimentRetainedRunnerLog");
    contents->setReadOnly(true);
    layout->addWidget(contents, 1);
    auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close");
    QObject::connect(close, &QPushButton::clicked, &viewer, &QDialog::accept);
    layout->addWidget(close, 0, Qt::AlignRight);
    viewer.exec();
  }

  void create_match_candidate(
      int source_row,
      std::optional<hm::stitching::CalibrationMatchSet> edited = std::nullopt,
      std::string expected_snapshot = {},
      QPointer<MatchEditorDialog> editor = {}) {
    if (!store || preparation_worker || batch_active || closing)
      return;
    if (!ensure_session()) {
      if (editor)
        editor->setSaving(false, status->text());
      return;
    }
    const auto source = *candidates[source_row].workspace;
    const auto root = std::filesystem::path(session->path().toStdString());
    const auto persistent = *store;
    const int sequence = ++next_candidate_sequence;
    auto result = std::make_shared<absl::StatusOr<StoredStitchingExperiment>>(
        absl::UnknownError("Match candidate preparation did not finish"));
    const bool preparing = !edited;
    QThread* worker =
        QThread::create([source, root, persistent, sequence, edited = std::move(edited), expected_snapshot, result]() {
          auto workspace = edited
              ? CreateEditedStitchingExperimentWorkspace(source, root, expected_snapshot, *edited, sequence)
              : CreateStitchingExperimentEditableCopy(source, root, sequence);
          if (!workspace.ok()) {
            *result = workspace.status();
            return;
          }
          auto selection =
              ReusableStitchingExperimentSelectionFingerprint(workspace->game_directory, workspace->settings);
          if (!selection.ok()) {
            *result = selection.status();
            return;
          }
          StoredStitchingExperiment record;
          record.workspace = *workspace;
          record.sequence = sequence;
          record.state = "queued";
          record.selection_fingerprint = *selection;
          record.saved_selection_fingerprint = *selection;
          auto status = SaveStitchingExperiment(persistent, record);
          if (!status.ok()) {
            *result = status;
            return;
          }
          *result = std::move(record);
        });
    preparation_worker = worker;
    if (editor)
      editor->setSaving(true);
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, preparing, editor]() {
      if (preparation_worker != worker)
        return;
      preparation_worker = nullptr;
      if (!result->ok()) {
        const auto error = QString::fromStdString(result->status().ToString());
        show_status(error, true);
        if (editor)
          editor->setSaving(false, error);
      } else {
        if (editor) {
          editor->setSaving(false);
          editor->accept();
        }
        Candidate candidate;
        candidate.stored = **result;
        candidate.workspace = (**result).workspace;
        candidate.settings = (**result).workspace.settings;
        candidate.sequence = (**result).sequence;
        candidate.saved_selection_fingerprint = (**result).selection_fingerprint;
        candidate.has_selected_frames = !candidate.saved_selection_fingerprint.empty();
        append_candidate(std::move(candidate));
        const int row = static_cast<int>(candidates.size()) - 1;
        table->selectRow(row);
        if (!closing) {
          if (preparing)
            open_editor_after_row = row;
          start_candidate_batch(row);
        }
      }
      update_controls();
      maybe_finish_close();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    show_status(
        preparing ? "Preparing an editable copy with original camera images…"
                  : "Saving edited matches into a new candidate…");
    worker->start();
    update_controls();
  }

  void open_match_editor(int row) {
    if (closing || preparation_worker || batch_active || row < 0 || row >= static_cast<int>(candidates.size()) ||
        !candidates[row].workspace)
      return;
    using MatchSet = hm::stitching::CalibrationMatchSet;
    struct Result {
      absl::Status status;
      MatchSet matches;
      MatchSet automatic;
    };
    const auto workspace = *candidates[row].workspace;
    auto result = std::make_shared<Result>();
    QThread* worker = QThread::create([workspace, result]() {
      auto config = hm::stitching::load_game_config_file(workspace.game_directory / "config.yaml");
      if (!config.ok()) {
        result->status = config.status();
        return;
      }
      if (!config->has_value()) {
        result->status = absl::NotFoundError("Calibration configuration is missing");
        return;
      }
      try {
        const auto snapshot = (**config)["hstream_ui"]["stitching_calibration"]["match_snapshot"];
        if (!snapshot || !snapshot.IsScalar()) {
          result->status = absl::NotFoundError("Editable coordinates are unavailable. Prepare an editable copy first.");
          return;
        }
        auto matches = hm::stitching::LoadCalibrationMatches(workspace.game_directory, snapshot.as<std::string>());
        if (!matches.ok()) {
          result->status = matches.status();
          return;
        }
        result->matches = std::move(*matches);
        if (!result->matches.manual) {
          result->automatic = result->matches;
        } else {
          auto automatic = hm::stitching::LoadCalibrationMatches(
              workspace.game_directory, result->matches.automatic_fingerprint, false);
          if (!automatic.ok()) {
            result->status = automatic.status();
            return;
          }
          result->automatic = std::move(*automatic);
        }
      } catch (const std::exception& error) {
        result->status = absl::InvalidArgumentError(error.what());
      }
    });
    preparation_worker = worker;
    QObject::connect(worker, &QThread::finished, dialog, [this, worker, result, row]() {
      if (preparation_worker != worker)
        return;
      preparation_worker = nullptr;
      update_controls();
      if (closing) {
        maybe_finish_close();
        return;
      }
      if (!result->status.ok()) {
        show_status(QString::fromStdString(result->status.ToString()), true);
        return;
      }
      const auto expected = result->matches.fingerprint;
      MatchEditorDialog editor(std::move(result->matches), std::move(result->automatic), dialog);
      editor.setSaveHandler(
          [this, row, expected, &editor]() { create_match_candidate(row, editor.editedSet(), expected, &editor); });
      editor.exec();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    show_status("Loading original camera images and editable matches…");
    worker->start();
    update_controls();
  }

  void inspect_selected_frames() {
    const int row = table->currentRow();
    if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidate_has_inspection(candidates[row]))
      return;
    auto inspection = InspectStitchingExperimentFrames(*candidates[row].workspace);
    if (!inspection.ok()) {
      show_status(QString::fromStdString(inspection.status().ToString()), true);
      return;
    }
    // Mutter does not maximize transient windows with the Dialog window type,
    // even when Qt reports WindowMaximized. Keep QDialog ownership/modality,
    // but request an ordinary, resizable native window.
    QDialog viewer(dialog, Qt::Window);
    viewer.setObjectName("stitchExperimentFrameInspector");
    viewer.setWindowTitle(QString("Calibration frames — Candidate %1").arg(candidates[row].sequence));
    viewer.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    viewer.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    viewer.setSizeGripEnabled(true);
    viewer.resize(1180, 780);
    auto* layout = new QVBoxLayout(&viewer);
    auto* explanation = new QLabel(
        inspection->player_selected
            ? "These are the exact synchronized camera pairs selected for calibration. Thumbnails come from the same "
              "extracted images used by the matcher. The coverage grid summarizes people surviving the Program ice mask "
              "in the baseline stitched view; it is not an overlay on the raw cameras."
            : "These are this candidate's ordinary calibration pairs. Thumbnails come from the captured images supplied "
              "to the matcher. Source times are shown when recorded; player scores are unavailable for ordinary capture.");
    explanation->setWordWrap(true);
    auto* heading = new QHBoxLayout();
    heading->addWidget(explanation, 1);
    auto* maximize = new WindowSizeButton(&viewer);
    maximize->setObjectName("maximizeCalibrationFramesWindowButton");
    heading->addWidget(maximize, 0, Qt::AlignTop);
    layout->addLayout(heading);
    auto* validation = new QLabel(QString::fromStdString(inspection->source_validation));
    validation->setTextFormat(Qt::PlainText);
    validation->setWordWrap(true);
    layout->addWidget(validation);
    auto* choices = new QTableWidget(static_cast<int>(inspection->frames.size()), 5);
    choices->setObjectName("stitchExperimentSelectedFrames");
    choices->setHorizontalHeaderLabels(
        inspection->player_selected
            ? QStringList{"Pair", "Time after anchor", "People in overlap", "Far / middle / near", "Quality"}
            : QStringList{"Pair", "Left source time", "Right source time", "", ""});
    if (!inspection->player_selected) {
      choices->hideColumn(3);
      choices->hideColumn(4);
    }
    choices->setSelectionBehavior(QAbstractItemView::SelectRows);
    choices->setSelectionMode(QAbstractItemView::SingleSelection);
    choices->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    choices->setMaximumHeight(190);
    for (size_t index = 0; index < inspection->frames.size(); ++index) {
      const auto& frame = inspection->frames[index];
      const QStringList values = inspection->player_selected ? QStringList{
          index == 0 ? "1 (anchor)" : QString::number(index + 1),
          exact_frame_time(frame.timeline_ns - inspection->frames.front().timeline_ns),
          QString::number(frame.eligible_people),
          QString("%1 / %2 / %3")
              .arg(frame.size_band_counts[0])
              .arg(frame.size_band_counts[1])
              .arg(frame.size_band_counts[2]),
          QString::number(frame.quality, 'g', 6),
      } : QStringList{
          QString::number(index + 1),
          frame.camera_paths[0].empty() ? "Unavailable" : QString::number(frame.source_seconds[0], 'f', 6) + " s",
          frame.camera_paths[1].empty() ? "Unavailable" : QString::number(frame.source_seconds[1], 'f', 6) + " s",
          "", ""};
      for (int column = 0; column < values.size(); ++column) {
        auto* item = new QTableWidgetItem(values[column]);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        choices->setItem(static_cast<int>(index), column, item);
      }
    }
    layout->addWidget(choices);
    auto* match_controls = new QHBoxLayout();
    auto* show_matches = new QCheckBox("Show matches");
    show_matches->setObjectName("stitchExperimentShowMatches");
    auto* points_only = new QCheckBox("Points only");
    points_only->setObjectName("stitchExperimentMatchPointsOnly");
    auto* match_status = new QLabel();
    match_status->setObjectName("stitchExperimentMatchStatus");
    match_status->setWordWrap(true);
    match_controls->addWidget(show_matches);
    match_controls->addWidget(points_only);
    match_controls->addWidget(match_status, 1);
    layout->addLayout(match_controls);
    auto* images = new QHBoxLayout();
    std::array<CalibrationFrameView*, 2> camera_images;
    std::array<QGroupBox*, 2> camera_groups;
    for (size_t camera = 0; camera < camera_images.size(); ++camera) {
      auto* group = new QGroupBox(camera == 0 ? "Left camera" : "Right camera");
      camera_groups[camera] = group;
      auto* image_layout = new QVBoxLayout(group);
      camera_images[camera] = new CalibrationFrameView(group);
      camera_images[camera]->setObjectName(
          camera == 0 ? "stitchExperimentSelectedLeft" : "stitchExperimentSelectedRight");
      add_image_navigation(image_layout, camera_images[camera]);
      image_layout->addWidget(camera_images[camera]);
      images->addWidget(group, 2);
    }
    auto* match_group = new QGroupBox("Matched control points — left / right camera");
    auto* match_layout = new QVBoxLayout(match_group);
    auto* match_image = new CalibrationFrameView(match_group);
    match_image->setObjectName("stitchExperimentSelectedMatches");
    add_image_navigation(match_layout, match_image);
    match_layout->addWidget(match_image);
    images->addWidget(match_group, 4);
    auto* coverage_group = new QGroupBox("Baseline stitched scoring coverage");
    auto* coverage_layout = new QVBoxLayout(coverage_group);
    auto* coverage = new SelectedFrameCoverage(coverage_group);
    coverage->setObjectName("stitchExperimentSelectedCoverage");
    coverage_layout->addWidget(coverage, 1);
    auto* legend = new QLabel("Blue: far · Green: middle · Amber: near\nApparent size bands, not measured distances");
    legend->setWordWrap(true);
    coverage_layout->addWidget(legend);
    images->addWidget(coverage_group, 1);
    coverage_group->setVisible(inspection->player_selected);
    layout->addLayout(images, 1);
    auto* detail = new QPlainTextEdit();
    detail->setObjectName("stitchExperimentSelectedFrameIdentity");
    detail->setReadOnly(true);
    detail->setMaximumHeight(150);
    layout->addWidget(detail);
    const auto show_match_view = [&]() {
      const int selected_row = choices->currentRow();
      if (selected_row < 0 || selected_row >= static_cast<int>(inspection->frames.size()))
        return;
      const auto& frame = inspection->frames[selected_row];
      const bool available = std::all_of(frame.match_images.begin(), frame.match_images.end(), [](const auto& path) {
        const QFileInfo info(QString::fromStdString(path.string()));
        return info.isFile() && !info.isSymLink();
      });
      show_matches->setEnabled(available);
      const bool showing = available && show_matches->isChecked();
      points_only->setEnabled(showing);
      match_group->setVisible(showing);
      for (auto* group : camera_groups)
        group->setVisible(!showing);
      match_status->setText(
          available ? "Retained matches for this pair, before multi-frame pooling and geometric validation."
                    : "Match images were not recorded for this pair. New calibrations save them after matching.");
      if (showing)
        match_image->load(frame.match_images[points_only->isChecked() ? 0 : 1], 2048);
    };
    const auto show_pair = [&]() {
      const int selected_row = choices->currentRow();
      if (selected_row < 0 || selected_row >= static_cast<int>(inspection->frames.size()))
        return;
      const auto& frame = inspection->frames[selected_row];
      QString identity = inspection->player_selected ? QString("Decode anchor: %1 (%2 ns)\nPair timeline: %3 ns\n")
                                                           .arg(exact_frame_time(inspection->anchor_ns))
                                                           .arg(inspection->anchor_ns)
                                                           .arg(frame.timeline_ns)
                                                     : QString();
      for (size_t camera = 0; camera < camera_images.size(); ++camera) {
        camera_images[camera]->load(frame.thumbnails[camera]);
        identity += inspection->player_selected
            ? QString("%1: %2\n  Source PTS: %3 (%4 ns); decoded sequence: %5\n")
                  .arg(camera == 0 ? "Left" : "Right")
                  .arg(QString::fromStdString(frame.camera_paths[camera]))
                  .arg(exact_frame_time(frame.source_pts_ns[camera]))
                  .arg(frame.source_pts_ns[camera])
                  .arg(frame.decoded_sequences[camera])
            : QString("%1: %2\n  Source time: %3\n")
                  .arg(camera == 0 ? "Left" : "Right")
                  .arg(
                      frame.camera_paths[camera].empty() ? "Source metadata unavailable"
                                                         : QString::fromStdString(frame.camera_paths[camera]))
                  .arg(
                      frame.camera_paths[camera].empty()
                          ? "Unavailable"
                          : QString::number(frame.source_seconds[camera], 'f', 9) + " s");
      }
      if (inspection->player_selected)
        identity += "Selection fingerprint: " + QString::fromStdString(inspection->fingerprint);
      detail->setPlainText(identity);
      coverage->set_coverage(frame.coverage);
      show_match_view();
    };
    QObject::connect(choices, &QTableWidget::itemSelectionChanged, &viewer, show_pair);
    QObject::connect(show_matches, &QCheckBox::toggled, &viewer, show_match_view);
    QObject::connect(points_only, &QCheckBox::toggled, &viewer, show_match_view);
    bool editable = false;
    const auto config = hm::stitching::load_game_config_file(candidates[row].workspace->game_directory / "config.yaml");
    if (config.ok() && config->has_value()) {
      const YAML::Node saved = (**config)["hstream_ui"]["stitching_calibration"]["match_snapshot"];
      editable = saved && saved.IsScalar();
    }
    auto* footer = new QHBoxLayout();
    auto* edit = new QPushButton(
        action_icon(editable ? ActionIcon::Inspect : ActionIcon::Prepare),
        editable ? "Edit matches…" : "Prepare editable copy…");
    edit->setObjectName("stitchExperimentEditMatches");
    edit->setToolTip(
        editable
            ? "Edit original-resolution points, then save and recalibrate a new candidate."
            : "This older result has only JPEG previews. Recalculate a private copy to retain editable points and full-resolution images.");
    edit->setEnabled(store && store_error.isEmpty());
    bool edit_requested = false;
    QObject::connect(edit, &QPushButton::clicked, &viewer, [&]() {
      edit_requested = true;
      viewer.accept();
    });
    footer->addWidget(edit);
    footer->addStretch();
    auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close");
    QObject::connect(close, &QPushButton::clicked, &viewer, &QDialog::accept);
    footer->addWidget(close);
    layout->addLayout(footer);
    choices->selectRow(0);
    viewer.exec();
    if (edit_requested) {
      if (editable)
        open_match_editor(row);
      else
        create_match_candidate(row);
    }
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
    if (closing || close_prompt_active)
      return;
    if (promotion_worker) {
      show_status("Candidate publication is still running; the dialog will close after it succeeds.");
      return;
    }
    const bool unapplied_results =
        !results_applied && std::any_of(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
          return !candidate.main_calibration && (candidate.complete || candidate.has_selected_frames);
        });
    if (unapplied_results) {
      close_prompt_active = true;
      QMessageBox prompt(
          QMessageBox::Question,
          "Keep stitching experiment results",
          "This batch has calibration results or saved frame selections that have not been applied.",
          QMessageBox::NoButton,
          dialog);
      prompt.setObjectName("stitchExperimentCloseGuard");
      prompt.setInformativeText(
          "Keeping results closes without applying. Every experiment and frame set remains in this game for the next time you open the dialog.");
      auto* use = prompt.addButton("Use selected in main Program", QMessageBox::AcceptRole);
      auto* discard = prompt.addButton("Keep results and close", QMessageBox::ActionRole);
      auto* keep = prompt.addButton("Cancel", QMessageBox::RejectRole);
      use->setIcon(action_icon(ActionIcon::Apply));
      discard->setIcon(action_icon(ActionIcon::Save));
      keep->setIcon(action_icon(ActionIcon::Cancel));
      use->setObjectName("stitchExperimentCloseUse");
      discard->setObjectName("stitchExperimentCloseDiscard");
      keep->setObjectName("stitchExperimentCloseCancel");
      prompt.setDefaultButton(keep);
      prompt.setEscapeButton(keep);
      use->setEnabled(can_apply_selected());
      if (!use->isEnabled())
        prompt.setInformativeText(
            "Select a ready candidate after all runners and previews stop to apply it. "
            "Keep results and close preserves this game's experiment history. Cancel keeps this dialog open.");
      prompt.exec();
      close_prompt_active = false;
      if (prompt.clickedButton() == use) {
        apply_selected();
        return;
      }
      if (prompt.clickedButton() != discard)
        return;
    }
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
    if (preparation_worker) {
      preparation_worker->wait();
      delete preparation_worker;
      preparation_worker = nullptr;
    }
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
    // A native Dialog window cannot maximize on Mutter. Qt::Window retains
    // QDialog's parent/modal lifecycle while allowing normal window actions.
    : QDialog(parent, Qt::Window), impl_(std::make_unique<Impl>(this)) {
  setObjectName("stitchingExperimentDialog");
  setWindowTitle("Stitching Experiments");
  setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setSizeGripEnabled(true);
  resize(1280, 820);
  auto& s = *impl_;
  s.game_directory = game_directory;
  s.runner = runner;
  s.working_directory = working_directory;
  s.pipeline_config = pipeline_config;
  s.environment = environment;
  s.initial_settings = {control_points, frame_count, stitch_frame_time.toStdString(), std::nullopt};
  s.selection_applied = std::move(selection_applied);

  auto* root = new QVBoxLayout(this);
  auto* intro = new QLabel(
      "Add as many option combinations as you want to the batch, start it when ready, and leave it running. When "
      "you return, replay the same moving passage across each completed seam. Nothing changes the main stitching "
      "configuration unless you explicitly choose Use selected in main Program. Optional player selection uses "
      "the same ice filtering as Program; moving previews do not run crop or tracking.");
  intro->setWordWrap(true);
  auto* heading = new QHBoxLayout();
  heading->addWidget(intro, 1);
  auto* maximize = new WindowSizeButton(this);
  maximize->setObjectName("maximizeStitchExperimentWindowButton");
  heading->addWidget(maximize, 0, Qt::AlignTop);
  root->addLayout(heading);

  auto* matrix_group = new QGroupBox("Candidate matrix");
  matrix_group->setObjectName("stitchExperimentMatrix");
  auto* matrix_layout = new QFormLayout(matrix_group);
  s.control_points = new QLineEdit(QString::number(control_points));
  s.control_points->setObjectName("stitchExperimentControlPoints");
  s.control_points->setPlaceholderText("600,900,1500");
  s.control_points->setToolTip(
      "Maximum retained matches per synchronized frame pair. Contributions add across pairs: "
      "100 with 2 frames gives up to 200 total before geometric validation.");
  s.frame_counts = new QLineEdit(QString::number(frame_count));
  s.frame_counts->setObjectName("stitchExperimentFrameCounts");
  s.frame_counts->setPlaceholderText("1,4,8");
  s.start_frames = new QLineEdit(stitch_frame_time);
  s.start_frames->setObjectName("stitchExperimentStartFrames");
  s.start_frames->setPlaceholderText("00:05:00,00:10:00.500");
  s.control_point_resolution = new QComboBox();
  s.control_point_resolution->setObjectName("stitchExperimentControlPointResolution");
  try {
    const auto baseline = environment.contains("HM_CONFIG_ROOT")
        ? hm::baseline_config::load_from_root(environment.value("HM_CONFIG_ROOT").toStdString())
        : hm::baseline_config::load();
    if (!baseline.ok())
      throw std::runtime_error(baseline.status().ToString());
    const auto user = hm::user_config::load_or_create();
    if (!user.ok())
      throw std::runtime_error(user.status().ToString());
    overlay_feature_settings(s.feature_defaults, baseline->values);
    overlay_feature_settings(s.feature_defaults, *user);
    YAML::Node effective = YAML::Clone(s.feature_defaults);
    const auto saved =
        hm::stitching::load_game_config_file(std::filesystem::path(game_directory.toStdString()) / "config.yaml");
    if (!saved.ok())
      throw std::runtime_error(saved.status().ToString());
    if (saved->has_value()) {
      YAML::Node game = YAML::Clone(**saved);
      hm::stitching::restore_generated_control_point_resolution(game);
      overlay_feature_settings(effective, game);
    }
    const auto resolution = hm::stitching::read_control_point_resolution(effective);
    if (!resolution.ok())
      throw std::runtime_error(resolution.status().ToString());
    const auto matcher = feature_matcher(effective);
    const QString size = hm::stitching::ControlPointResolutionName(*resolution);
    s.feature_size_selectable = matcher == hm::stitching::ControlPointMatcher::kSuperPointLightGlue;
    if (s.feature_size_selectable) {
      s.control_point_resolution->addItem("Native (full size)", "native");
      s.control_point_resolution->addItem("1K (1024 px long edge)", "1k");
      s.control_point_resolution->addItem("2K (2048 × 1152)", "2k");
      s.control_point_resolution->setCurrentIndex(s.control_point_resolution->findData(size));
      s.control_point_resolution->setToolTip(
          "SuperPoint + LightGlue input size. Native uses the original pixels; 1K uses a 1024-pixel long edge; "
          "2K fits each image into 2048 × 1152. Resizing preserves aspect ratio. "
          "Each added candidate keeps this choice independently of later changes.");
    } else {
      const QString fixed = fixed_feature_size(matcher);
      s.control_point_resolution->addItem(fixed, size);
      s.control_point_resolution->setToolTip("This matcher uses a fixed processing size: " + fixed + ".");
    }
    s.initial_settings.control_point_resolution = size.toStdString();
  } catch (const std::exception& error) {
    s.feature_settings_error = "Cannot load feature image settings: " + QString::fromUtf8(error.what());
    s.control_point_resolution->addItem("Unavailable");
    s.control_point_resolution->setToolTip(s.feature_settings_error);
  }
  s.shared_rotation = new QCheckBox("Use the game’s saved rink leveling for every candidate");
  s.shared_rotation->setObjectName("stitchExperimentSharedRotation");
  s.shared_rotation->setChecked(true);
  s.rotations = new QLineEdit("0/0");
  s.rotations->setObjectName("stitchExperimentRotations");
  s.rotations->setPlaceholderText("pitch/roll pairs: 0/0,-1.5/0.5");
  s.rotations->setEnabled(false);
  connect(s.shared_rotation, &QCheckBox::toggled, this, [&s](bool checked) { s.rotations->setEnabled(!checked); });
  matrix_layout->addRow("CP limits per frame pair", s.control_points);
  matrix_layout->addRow("Feature image size", s.control_point_resolution);
  matrix_layout->addRow("Frame counts", s.frame_counts);
  matrix_layout->addRow("First calibration frames", s.start_frames);
  matrix_layout->addRow(s.shared_rotation);
  matrix_layout->addRow("Rink pitch/roll variants", s.rotations);
  s.prefer_player_frames = new QCheckBox("Prefer player-rich frames");
  s.prefer_player_frames->setObjectName("stitchExperimentPreferPlayerFrames");
  s.prefer_player_frames->setChecked(true);
  s.prefer_player_frames->setToolTip(
      "Choose player-rich frames once per frame count, using one ordinary baseline and the Program ice mask. "
      "Later options reuse that exact selection and its original search duration, even when this box is unchecked. "
      "Saved selections are always retained. One-frame candidates keep only the anchor.");
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
  s.add_to_batch = new QPushButton(action_icon(ActionIcon::Add), "Add options to batch");
  s.add_to_batch->setObjectName("addStitchExperimentsToBatchButton");
  s.remove_from_batch = new QPushButton(action_icon(ActionIcon::Remove), "Remove selected");
  s.remove_from_batch->setObjectName("removeStitchExperimentFromBatchButton");
  s.clear_batch = new QPushButton(action_icon(ActionIcon::Delete), "Discard experiments…");
  s.clear_batch->setObjectName("clearStitchExperimentBatchButton");
  s.start_batch = new QPushButton(action_icon(ActionIcon::Play), "Start batch");
  s.start_batch->setObjectName("startStitchExperimentBatchButton");
  s.cancel = new QPushButton(action_icon(ActionIcon::Cancel), "Cancel batch");
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
  s.table = new QTableWidget(0, 8);
  s.table->setObjectName("stitchExperimentCandidates");
  s.table->setHorizontalHeaderLabels(
      {"Candidate", "CP", "Frames", "First frame", "Rink pitch / roll", "Status", "Frame selection", "Image size"});
  s.table->horizontalHeaderItem(1)->setToolTip("Maximum control points per synchronized frame pair.");
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
  s.preview = new QPushButton(action_icon(ActionIcon::Play), "Play selected");
  s.preview->setObjectName("previewStitchExperimentButton");
  s.stop_preview = new QPushButton(action_icon(ActionIcon::Stop), "Stop");
  s.stop_preview->setObjectName("stopStitchExperimentPreviewButton");
  s.expand_preview = new QPushButton(action_icon(ActionIcon::Expand), "Expand preview");
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
  s.cache_location = new QLabel();
  s.cache_location->setObjectName("stitchExperimentCacheLocation");
  s.cache_location->setWordWrap(true);
  s.cache_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
  s.log = new QPlainTextEdit();
  s.log->setObjectName("stitchExperimentLog");
  s.log->setReadOnly(true);
  s.log->setMaximumBlockCount(4000);
  s.log->setMaximumHeight(130);
  root->addWidget(s.progress);
  root->addWidget(s.status);
  root->addWidget(s.cache_location);
  root->addWidget(s.log);
  s.preview_focus_siblings = {intro, candidate_panel, s.log};
  auto* bottom = new QHBoxLayout();
  s.apply = new QPushButton(action_icon(ActionIcon::Apply), "Use selected in main Program");
  s.apply->setObjectName("applyStitchExperimentButton");
  s.inspect_frames = new QPushButton(action_icon(ActionIcon::Inspect), "Inspect calibration frames");
  s.inspect_frames->setObjectName("inspectStitchExperimentFramesButton");
  s.view_runner_log = new QPushButton(action_icon(ActionIcon::Document), "View runner log");
  s.view_runner_log->setObjectName("viewStitchExperimentRunnerLogButton");
  auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close");
  close->setObjectName("closeStitchExperimentButton");
  bottom->addWidget(s.apply);
  bottom->addWidget(s.inspect_frames);
  bottom->addWidget(s.view_runner_log);
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
  connect(s.view_runner_log, &QPushButton::clicked, this, [&s]() { s.show_selected_runner_log(); });
  connect(close, &QPushButton::clicked, this, [this]() { this->close(); });
  s.restore_persistent_store(s.initial_settings);
  if (!s.feature_settings_error.isEmpty())
    s.show_status(s.feature_settings_error, true);
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
