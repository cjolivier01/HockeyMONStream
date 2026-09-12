#include "src/apps/hstream-ui/CameraExperimentDialog.h"
#include "hstream/src/libs/recording/Database.h"

#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"
#include "src/apps/hstream-ui/CameraExperimentSource.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace {
namespace replay = hm::playtracker_replay;

class ExperimentVideoTarget : public QWidget {
 public:
  explicit ExperimentVideoTarget(QWidget* parent) : QWidget(parent) {
    if (QGuiApplication::platformName() == "xcb") {
      setAttribute(Qt::WA_NativeWindow);
      setAttribute(Qt::WA_PaintOnScreen);
      setAttribute(Qt::WA_NoSystemBackground);
    }
    setAutoFillBackground(false);
    setMinimumSize(480, 270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  QPaintEngine* paintEngine() const override {
    return nullptr;
  }
};

class CameraPathPlot : public QWidget {
 public:
  explicit CameraPathPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(150);
  }
  std::shared_ptr<const std::vector<replay::Frame>> original;
  std::shared_ptr<const std::vector<replay::Frame>> trial;
  unsigned canvas_width{1};
  std::size_t frame{0};

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#101b2d"));
    p.setPen(QColor("#c7d2e1"));
    p.drawText(14, 22, "Program camera horizontal position");
    p.setPen(QColor("#94a3b8"));
    p.drawText(width() - 220, 22, "Recorded");
    p.setPen(QColor("#41dbc0"));
    p.drawText(width() - 122, 22, "Selected trial");
    const QRectF plot(14, 38, width() - 28, height() - 58);
    p.setPen(QPen(QColor("#26364d"), 1));
    for (int i = 0; i <= 4; ++i)
      p.drawLine(
          QPointF(plot.left(), plot.top() + plot.height() * i / 4),
          QPointF(plot.right(), plot.top() + plot.height() * i / 4));
    auto curve = [&](const std::shared_ptr<const std::vector<replay::Frame>>& frames, const QColor& color) {
      if (!frames || frames->size() < 2)
        return;
      QPainterPath path;
      bool started = false;
      for (std::size_t i = 0; i < frames->size(); ++i) {
        const auto& box = frames->at(i).follower;
        if (!box) {
          started = false;
          continue;
        }
        const QPointF point(
            plot.left() + plot.width() * i / (frames->size() - 1),
            plot.bottom() - plot.height() * (box->left + box->width() / 2) / std::max(1U, canvas_width));
        if (started)
          path.lineTo(point);
        else
          path.moveTo(point);
        started = true;
      }
      p.setPen(QPen(color, 2));
      p.drawPath(path);
    };
    curve(original, QColor("#94a3b8"));
    curve(trial, QColor("#41dbc0"));
    if (trial && trial->size() > 1) {
      const double x = plot.left() + plot.width() * std::min(frame, trial->size() - 1) / (trial->size() - 1);
      p.setPen(QPen(QColor("#f6be62"), 1, Qt::DashLine));
      p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
  }
};

QString completed_manifest(const QString& directory) {
  const QDir dir(directory);
  for (const QFileInfo& info : dir.entryInfoList({"hstream_telemetry*.db"}, QDir::Files, QDir::Time)) {
    try {
      hm::recording::Database db(info.absoluteFilePath().toStdString());
      db.Validate();
      hm::recording::Statement runs(db.get(), "SELECT 1 FROM runs WHERE completed=1 LIMIT 1");
      if (runs.Next())
        return info.absoluteFilePath();
    } catch (const std::exception& e) {
      qWarning("Cannot inspect recording: %s", e.what());
    }
  }
  for (const QFileInfo& info : dir.entryInfoList({"hstream_telemetry*.json"}, QDir::Files, QDir::Time)) {
    QFile file(info.absoluteFilePath());
    if (file.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(file.readAll()).object()["completed"].toBool())
      return info.absoluteFilePath();
  }
  return {};
}

std::pair<unsigned, unsigned> probe_media(const QString& path, std::string* error) {
  QProcess probe;
  probe.start(
      "ffprobe",
      {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "json", path});
  if (!probe.waitForStarted(3000) || !probe.waitForFinished(10000) || probe.exitCode() != 0) {
    probe.kill();
    probe.waitForFinished(1000);
    *error = "Could not inspect panorama video with ffprobe: " + probe.readAllStandardError().toStdString();
    return {};
  }
  const QJsonArray streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray();
  if (streams.empty()) {
    *error = "The selected panorama has no video stream.";
    return {};
  }
  const QJsonObject video = streams.first().toObject();
  return {video["width"].toInt(), video["height"].toInt()};
}

} // namespace

struct CameraExperimentDialog::Impl {
  struct Control {
    CameraSliderSpec spec;
    QCheckBox* enabled;
    QDoubleSpinBox* value;
  };
  struct WorkResult {
    std::shared_ptr<replay::ReplaySession> session;
    std::optional<replay::TrialResult> trial;
    std::string error;
    std::pair<unsigned, unsigned> media_size;
    QString media_path;
    std::optional<replay::StitchingMedia> stitching;
  };
  CameraExperimentDialog* dialog;
  QLineEdit* manifest{nullptr};
  QComboBox* run_id{nullptr};
  QLineEdit* media_path{nullptr};
  QLineEdit* game_path{nullptr};
  QComboBox* source_mode{nullptr};
  QDoubleSpinBox* in{nullptr};
  QDoubleSpinBox* duration{nullptr};
  QDoubleSpinBox* video_origin{nullptr};
  QLineEdit* legacy_arena{nullptr};
  QLineEdit* legacy_config{nullptr};
  QCheckBox* uncropped{nullptr};
  QCheckBox* fast{nullptr};
  QCheckBox* follower{nullptr};
  QCheckBox* loop{nullptr};
  QLineEdit* trial_name{nullptr};
  QComboBox* comparison{nullptr};
  QPushButton* prepare{nullptr};
  QPushButton* apply{nullptr};
  QPushButton* cancel{nullptr};
  QPushButton* play{nullptr};
  QPushButton* save{nullptr};
  QPushButton* capture{nullptr};
  QLabel* status{nullptr};
  QLabel* provenance{nullptr};
  QLabel* frame_label{nullptr};
  QSlider* timeline{nullptr};
  QWidget* settings{nullptr};
  QWidget* parameter_tabs{nullptr};
  QPushButton* previous{nullptr};
  QPushButton* next{nullptr};
  QProgressBar* progress{nullptr};
  QLabel* activity{nullptr};
  bool calculating{false};
  bool frame_ready{false};
  std::optional<int> closing_result;
  ExperimentVideoTarget* video{nullptr};
  CameraPathPlot* plot{nullptr};
  std::vector<Control> controls;
  std::map<QString, double> initial_controls;
  std::shared_ptr<replay::ReplaySession> session;
  std::vector<replay::TrialResult> trials;
  std::shared_ptr<const std::vector<replay::Frame>> selected;
  CameraExperimentPreviewWorker preview;
  std::future<WorkResult> work;
  std::shared_ptr<std::atomic<bool>> cancellation;
  bool preview_open{false};
  bool playing{false};
  bool changing_selection{false};
  bool source_dirty{false};
  std::size_t current{0};
  std::pair<unsigned, unsigned> media_size;
  QString prepared_media;
  std::optional<replay::StitchingMedia> prepared_stitching;
  replay::MediaBinding binding;

  explicit Impl(CameraExperimentDialog* owner) : dialog(owner) {}

  bool original_cameras() const {
    return source_mode->currentIndex() == 0;
  }
  QString source_path() const {
    return (original_cameras() ? game_path : media_path)->text().trimmed();
  }
  replay::MediaBinding media_binding() const {
    return {
        prepared_stitching ? QDir(prepared_media).filePath("config.yaml").toStdString() : prepared_media.toStdString(),
        selected->front().pts_ns,
        static_cast<std::uint64_t>(std::llround(video_origin->value() * 1e9)),
        media_size.first,
        media_size.second,
        prepared_stitching};
  }

  void show_status(const QString& text, bool error = false) {
    status->setText(text);
    status->setStyleSheet(error ? "color:#b42318;" : "color:#475467;");
    if (error)
      qWarning().noquote() << "Camera experiment:" << text;
  }

  void update_controls() {
    const auto state = preview.Poll();
    const bool loading = state.busy || state.seeking;
    const bool active = calculating || loading || closing_result.has_value();
    const bool editable = !active && !playing;
    const bool prepared = session && !source_dirty;
    prepare->setEnabled(editable);
    apply->setEnabled(editable && prepared);
    cancel->setEnabled(active && !closing_result);
    settings->setEnabled(editable);
    parameter_tabs->setEnabled(editable);
    fast->setEnabled(editable);
    follower->setEnabled(editable);
    trial_name->setEnabled(editable);
    comparison->setEnabled(!active && prepared);
    timeline->setEnabled(!active && prepared && !playing);
    previous->setEnabled(!active && prepared && !playing);
    next->setEnabled(!active && prepared && !playing);
    play->setEnabled(!calculating && !closing_result && prepared && (!loading || playing));
    play->setText(playing ? "Pause" : "Play preview");
    save->setEnabled(editable && prepared && comparison->currentIndex() >= 2);
    capture->setEnabled(!active && prepared && frame_ready);
    loop->setEnabled(!calculating && !closing_result);
    progress->setVisible(active);
    activity->setVisible(active || playing);
    activity->setText(
        closing_result    ? "Finishing background work before closing…"
            : calculating ? "Preparing experiment…"
            : loading     ? "Loading or stopping preview…"
            : playing     ? "Playing preview · Pause to edit camera settings"
                          : "");
  }

  void busy(bool active) {
    calculating = active;
    update_controls();
  }

  void close_preview() {
    preview.Close();
    preview_open = false;
    playing = false;
    frame_ready = false;
    update_controls();
  }

  void invalidate_source() {
    if (!session)
      return;
    source_dirty = true;
    apply->setEnabled(false);
    play->setEnabled(false);
    capture->setEnabled(false);
    save->setEnabled(false);
    close_preview();
    show_status("Recording or range changed. Prepare its historical start before applying a trial.");
  }

  void add_control(QFormLayout* layout, const CameraSliderSpec& spec) {
    if (!spec.runtime_key)
      return;
    QString label = QString::fromLatin1(spec.label);
    label.replace(" x100", "").replace(" x10", "");
    auto* enabled = new QCheckBox(label);
    enabled->setObjectName("experimentOverride_" + QString::fromLatin1(spec.id));
    auto* value = new QDoubleSpinBox();
    value->setObjectName("experimentValue_" + QString::fromLatin1(spec.id));
    value->setRange(spec.minimum / static_cast<double>(spec.divisor), spec.maximum / static_cast<double>(spec.divisor));
    value->setDecimals(
        std::string(spec.id) == "Oversized_Player_Percent" ? 2
            : spec.divisor == 1                            ? 0
            : spec.divisor == 10                           ? 1
                                                           : 2);
    value->setSingleStep(1.0 / spec.divisor);
    value->setValue(spec.default_value / static_cast<double>(spec.divisor));
    value->setEnabled(false);
    value->setToolTip(
        "Enable this override to change it at the selected start frame. Unchecked controls retain recorded values.");
    QObject::connect(enabled, &QCheckBox::toggled, value, &QWidget::setEnabled);
    const auto initial = initial_controls.find(QString::fromLatin1(spec.id));
    if (initial != initial_controls.end()) {
      value->setValue(initial->second / spec.divisor);
      enabled->setChecked(true);
    }
    layout->addRow(enabled, value);
    controls.push_back({spec, enabled, value});
  }

  absl::StatusOr<DsPlayTrackerRuntimeTuning> tuning() const {
    YAML::Node root;
    auto tracker = root["play-tracker"];
    tracker["hstream-apply-to-fast-box"] = fast->isChecked();
    tracker["hstream-apply-to-follower-box"] = follower->isChecked();
    // The runtime parser requires a target declaration. The replay adapter
    // subsequently validates the actual restored tracker topology.
    tracker["live-boxes"].push_back(YAML::Node(YAML::NodeType::Map));
    tracker["hstream-runtime-tuning"] = YAML::Node(YAML::NodeType::Map);
    auto values = tracker["hstream-runtime-tuning"];
    for (const auto& control : controls) {
      if (!control.enabled->isChecked())
        continue;
      const std::string key = control.spec.runtime_key;
      if (key == "cancel-stop-on-opposite-dir" || key == "ignore-oversized-bboxes")
        values[key] = control.value->value() != 0;
      else if (control.spec.divisor == 1 && key != "oversized-bbox-percent")
        values[key] = static_cast<int>(control.value->value());
      else
        values[key] = control.value->value();
    }
    // The same parser/validation used by live Program changes handles all
    // experiment values and their runtime units.
    return DsPlayTrackerLoadRuntimeTuningContents(YAML::Dump(root));
  }

  void prepare_session() {
    if (work.valid())
      return;
    replay::PrepareOptions options;
    options.manifest_path = manifest->text().trimmed().toStdString();
    options.run_id = run_id->currentData().toString().toStdString();
    options.start_seconds = in->value();
    options.duration_seconds = duration->value();
    options.legacy_config_path = legacy_config->text().trimmed().toStdString();
    if (!legacy_arena->text().trimmed().isEmpty()) {
      const QStringList values = legacy_arena->text().split(',', Qt::KeepEmptyParts);
      bool valid = values.size() == 4;
      float coordinates[4]{};
      for (int i = 0; i < values.size() && i < 4; ++i) {
        bool ok = false;
        coordinates[i] = values[i].trimmed().toFloat(&ok);
        valid = valid && ok && std::isfinite(coordinates[i]);
      }
      if (!valid || coordinates[2] <= coordinates[0] || coordinates[3] <= coordinates[1]) {
        show_status("Historical arena must be left, top, right, bottom in recorded canvas pixels.", true);
        return;
      }
      options.legacy_arena = hm::BBox(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
    }
    const QString media = source_path();
    const bool raw = original_cameras();
    cancellation = std::make_shared<std::atomic<bool>>(false);
    auto token = cancellation;
    close_preview();
    busy(true);
    show_status("Preparing historical state and verifying the original camera trajectory…");
    work = std::async(std::launch::async, [options, token, media, raw]() {
      WorkResult result;
      auto prepared = replay::ReplaySession::Prepare(options, token.get());
      if (!prepared.ok()) {
        result.error = prepared.status().ToString();
        return result;
      }
      result.session = *prepared;
      result.media_path = media;
      if (raw) {
        auto sources = PrepareExperimentSources(media.toStdString(), result.session->width(), result.session->height());
        if (!sources.ok())
          result.error = sources.status().ToString();
        else {
          result.stitching = std::move(*sources);
          result.media_size = {result.session->width(), result.session->height()};
        }
      } else if (!media.isEmpty()) {
        result.media_size = probe_media(media, &result.error);
      }
      return result;
    });
  }

  void run_trial() {
    if (!session || work.valid())
      return;
    if (!confirm_preview_source())
      return;
    auto values = tuning();
    if (!values.ok()) {
      show_status(QString::fromStdString(values.status().ToString()), true);
      return;
    }
    const auto prepared = session;
    const auto parameters = *values;
    const std::string name = trial_name->text().trimmed().isEmpty() ? "Trial " + std::to_string(trials.size() + 1)
                                                                    : trial_name->text().trimmed().toStdString();
    cancellation = std::make_shared<std::atomic<bool>>(false);
    auto token = cancellation;
    close_preview();
    busy(true);
    show_status("Restoring the same starting state and calculating this trial…");
    work = std::async(std::launch::async, [prepared, parameters, token, name]() {
      WorkResult result;
      auto trial = prepared->RunTrial(name, parameters, token.get());
      if (trial.ok())
        result.trial = std::move(*trial);
      else
        result.error = trial.status().ToString();
      return result;
    });
  }

  bool confirm_preview_source() {
    if (!uncropped->isChecked()) {
      show_status(
          QString("Playback needs source confirmation. Check “%1” under Video source confirmation, then try again.")
              .arg(uncropped->text()),
          true);
      uncropped->setFocus(Qt::OtherFocusReason);
      return false;
    }
    return true;
  }

  bool open_preview(bool play_video = false, std::size_t index = 0) {
    if (!session || !selected || selected->empty() || !confirm_preview_source())
      return false;
    if (QGuiApplication::platformName() != "xcb") {
      show_status("Video preview requires an X11 display. Camera trajectories remain available here.", true);
      return false;
    }
    if (source_path() != prepared_media || prepared_media.isEmpty()) {
      show_status("Select the original game directory or panorama, then prepare the recording again.", true);
      return false;
    }
    if (!replay::ValidatePanoramaGeometry(media_size.first, media_size.second, session->width(), session->height())
             .ok()) {
      show_status(
          QString(
              "Panorama is %1×%2; this recording uses %3×%4. Select the uncropped canvas with the same aspect ratio.")
              .arg(media_size.first)
              .arg(media_size.second)
              .arg(session->width())
              .arg(session->height()),
          true);
      return false;
    }
    binding = media_binding();
    const auto media = binding;
    const auto frames = selected;
    const auto width = session->width();
    const auto height = session->height();
    const auto end = session->end_pts_ns();
    const auto window = video->winId(); // QWidget access stays on Qt's thread.
    const bool reopen = !preview_open;
    const bool repeat = loop->isChecked();
    preview.Submit(
        [media, frames, width, height, end, window, reopen, repeat, play_video, index](auto& engine, auto* error) {
          if (reopen &&
              !engine.Open(
                  media,
                  width,
                  height,
                  window,
                  frames->front().edge_rotation_left,
                  frames->front().edge_rotation_right,
                  error))
            return false;
          engine.SetLoop(repeat);
          return engine.SetTrajectory(frames, end, error, play_video, index);
        });
    preview_open = true;
    playing = play_video;
    frame_ready = false;
    show_status("Loading preview… Every pass uses the prepared historical state.");
    update_controls();
    return true;
  }

  void select_comparison(bool replay_now = false) {
    if (!session || changing_selection)
      return;
    const int index = comparison->currentIndex();
    if (index == 0)
      selected = std::make_shared<const std::vector<replay::Frame>>(session->original());
    else if (index == 1)
      selected = std::make_shared<const std::vector<replay::Frame>>(session->baseline());
    else if (index >= 2 && static_cast<std::size_t>(index - 2) < trials.size())
      selected = std::make_shared<const std::vector<replay::Frame>>(trials[index - 2].frames);
    else
      return;
    plot->trial = selected;
    plot->frame = 0;
    plot->update();
    current = 0;
    capture->setEnabled(false);
    timeline->setRange(0, selected->empty() ? 0 : static_cast<int>(selected->size() - 1));
    timeline->setValue(0);
    save->setEnabled(index >= 2 && !source_dirty && !work.valid());
    update_frame();
    if (!source_dirty && (preview_open || replay_now)) {
      open_preview(replay_now);
    }
  }

  void update_frame() {
    if (!selected || selected->empty() || current >= selected->size())
      return;
    const auto& frame = selected->at(current);
    QString text = QString("Sample %1  ·  %2 s  ·  frame %3 / %4")
                       .arg(frame.sample_id)
                       .arg((frame.pts_ns - selected->front().pts_ns) / 1e9, 0, 'f', 3)
                       .arg(current + 1)
                       .arg(selected->size());
    if (frame.follower)
      text += QString("  ·  Program (%1, %2) %3×%4")
                  .arg(frame.follower->left, 0, 'f', 1)
                  .arg(frame.follower->top, 0, 'f', 1)
                  .arg(frame.follower->width(), 0, 'f', 1)
                  .arg(frame.follower->height(), 0, 'f', 1);
    frame_label->setText(text);
    if (!timeline->isSliderDown())
      timeline->setValue(static_cast<int>(current));
    plot->frame = current;
    plot->update();
  }

  void seek(std::size_t index, bool play_video = false) {
    if (!selected || index >= selected->size())
      return;
    current = index;
    update_frame();
    if (preview_open) {
      capture->setEnabled(false);
      frame_ready = false;
      preview.Submit([index, play_video](auto& engine, auto* error) { return engine.Seek(index, play_video, error); });
      playing = play_video;
      update_controls();
    }
  }

  void poll() {
    if (work.valid() && work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      WorkResult result;
      try {
        result = work.get();
      } catch (const std::exception& error) {
        result.error = error.what();
      } catch (...) {
        result.error = "Experiment preparation failed.";
      }
      const bool cancelled = cancellation && cancellation->load();
      if (cancelled) {
        show_status("Preparation cancelled. The previous prepared trial remains available.");
      } else if (!result.error.empty()) {
        show_status(QString::fromStdString(result.error), true);
      } else if (result.session) {
        close_preview();
        session = std::move(result.session);
        source_dirty = false;
        trials.clear();
        media_size = result.media_size;
        prepared_media = result.media_path;
        prepared_stitching = std::move(result.stitching);
        changing_selection = true;
        comparison->clear();
        comparison->addItems({"Recorded original", "Recomputed baseline"});
        comparison->setCurrentIndex(1);
        changing_selection = false;
        plot->original = std::make_shared<const std::vector<replay::Frame>>(session->original());
        plot->canvas_width = session->width();
        provenance->setText(QString::fromStdString(session->provenance()));
        select_comparison();
        play->setEnabled(true);
        show_status(
            QString("Historical start ready · %1 samples · %2×%3 canvas. Select overrides, then Apply && replay.")
                .arg(session->baseline().size())
                .arg(session->width())
                .arg(session->height()));
      } else if (result.trial) {
        trials.push_back(std::move(*result.trial));
        changing_selection = true;
        comparison->addItem(QString::fromStdString(trials.back().name));
        comparison->setCurrentIndex(comparison->count() - 1);
        changing_selection = false;
        select_comparison(true);
        trial_name->setText(QString("Trial %1").arg(trials.size() + 1));
      }
      busy(false);
    }
    const auto state = preview.Poll();
    if (preview_open) {
      if (!state.busy)
        playing = state.playing;
      frame_ready = state.frame.has_value() && !state.seeking && !state.busy;
      if (!state.error.empty()) {
        show_status(QString::fromStdString(state.error), true);
        close_preview();
      } else if (state.frame) {
        current = *state.frame;
        update_frame();
      }
    }
    update_controls();
    if (closing_result && !work.valid() && !state.busy && !state.open) {
      const int result = *closing_result;
      QTimer::singleShot(0, dialog, [owner = dialog, result] { owner->done(result); });
    }
  }
};

CameraExperimentDialog::CameraExperimentDialog(
    const QString& game_directory,
    QWidget* parent,
    const std::map<QString, double>& camera_controls)
    : QDialog(parent), impl_(std::make_unique<Impl>(this)) {
  auto& s = *impl_;
  s.initial_controls = camera_controls;
  setWindowTitle("HStream · Camera experiments");
  setObjectName("cameraExperimentDialog");
  resize(1440, 980);
  auto* root = new QVBoxLayout(this);
  auto* title = new QLabel("Camera experiments");
  title->setStyleSheet("font-size:24px;font-weight:600;color:#152b43;");
  root->addWidget(title);
  root->addWidget(new QLabel(
      "Replay a short passage from its recorded camera state. Each trial uses the same player observations."));

  s.settings = new QWidget();
  auto* source_layout = new QFormLayout(s.settings);
  source_layout->setContentsMargins(0, 4, 0, 4);
  auto file_row = [&](const char* name, const QString& label, const QString& filter, QLineEdit** field) {
    auto* row = new QWidget();
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    *field = new QLineEdit();
    (*field)->setObjectName(name);
    layout->addWidget(*field, 1);
    auto* browse = new QPushButton("Browse…");
    layout->addWidget(browse);
    QLineEdit* edit = *field;
    connect(browse, &QPushButton::clicked, this, [this, edit, filter, game_directory]() {
      const QString initial = edit->text().isEmpty() ? game_directory : edit->text();
      const QString path = filter == "directory"
          ? QFileDialog::getExistingDirectory(this, "Select original game directory", initial)
          : QFileDialog::getOpenFileName(this, "Select experiment artifact", initial, filter);
      if (!path.isEmpty())
        edit->setText(path);
    });
    source_layout->addRow(label, row);
  };
  file_row(
      "experimentManifest",
      "DriveGPT recording",
      "Telemetry database (*.db *.sqlite);;Legacy recording (hstream_telemetry*.json)",
      &s.manifest);
  s.source_mode = new QComboBox();
  s.source_mode->setObjectName("experimentSourceMode");
  s.source_mode->addItems({"Original cameras · stitch during replay", "Saved uncropped panorama"});
  source_layout->addRow("Video source", s.source_mode);
  file_row("experimentGameDirectory", "Original game directory", "directory", &s.game_path);
  s.game_path->setText(game_directory);
  file_row("experimentMedia", "Uncropped panorama", "Video (*.mp4 *.mkv *.mov *.MP4)", &s.media_path);
  s.manifest->setPlaceholderText("Select a completed telemetry database");
  s.run_id = new QComboBox;
  s.run_id->setObjectName("experimentRunId");
  source_layout->addRow("Recording", s.run_id);
  const auto update_recordings = [&s]() {
    const QString previous = s.run_id->currentData().toString();
    const QSignalBlocker blocker(s.run_id);
    s.run_id->clear();
    const QString path = s.manifest->text().trimmed();
    if (path.endsWith(".db") || path.endsWith(".sqlite")) {
      try {
        hm::recording::Database db(path.toStdString());
        db.Validate();
        hm::recording::Statement runs(
            db.get(),
            "SELECT run_id,game_id,started_utc FROM runs WHERE completed=1 ORDER BY started_utc DESC LIMIT 10000");
        while (runs.Next()) {
          const QString id = QString::fromStdString(runs.Text(0));
          s.run_id->addItem(QString::fromStdString(runs.Text(1) + " · " + runs.Text(2)) + " · " + id.left(8), id);
        }
      } catch (const std::exception& e) {
        qWarning("Cannot list recordings: %s", e.what());
      }
    }
    if (s.run_id->count() == 0)
      s.run_id->addItem("Select a completed recording", QString());
    const int index = s.run_id->findData(previous);
    if (index >= 0)
      s.run_id->setCurrentIndex(index);
    s.run_id->setEnabled(s.run_id->count() > 1);
  };
  connect(s.manifest, &QLineEdit::textChanged, this, update_recordings);
  s.media_path->setPlaceholderText("Optional uncropped archive; proportional downsizing is supported");
  QString selected_manifest = completed_manifest(game_directory);
  if (selected_manifest.isEmpty()) {
    const QString output_root = qEnvironmentVariable("HM_OUTPUT_WORK_DIR", QDir::home().filePath("hstream_output"));
    selected_manifest = completed_manifest(QDir(output_root).filePath(QFileInfo(game_directory).fileName()));
  }
  s.manifest->setText(selected_manifest);
  update_recordings();
  const QStringList panoramas = QDir(game_directory).entryList({"*-stitched_output*.mp4"}, QDir::Files, QDir::Time);
  if (!panoramas.empty())
    s.media_path->setText(QDir(game_directory).filePath(panoramas.front()));
  auto* range = new QHBoxLayout();
  auto time_control = [&](const char* name, const QString& label, double initial, double maximum) {
    range->addWidget(new QLabel(label));
    auto* value = new QDoubleSpinBox();
    value->setObjectName(name);
    value->setRange(0, maximum);
    value->setDecimals(3);
    value->setSuffix(" s");
    value->setValue(initial);
    range->addWidget(value);
    return value;
  };
  s.in = time_control("experimentIn", "In recording", 0, 86400);
  s.duration = time_control("experimentDuration", "Duration", 10, 120);
  s.duration->setMinimum(0.1);
  s.video_origin = time_control("experimentVideoOrigin", "First selected frame in video", 0, 86400);
  range->addStretch();
  source_layout->addRow(range);
  s.uncropped = new QCheckBox();
  s.uncropped->setObjectName("experimentUncropped");
  source_layout->addRow("Video source confirmation", s.uncropped);
  root->addWidget(s.settings);

  auto* splitter = new QSplitter(Qt::Horizontal);
  auto* left = new QWidget();
  auto* left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 8, 0);
  s.video = new ExperimentVideoTarget(left);
  s.video->setObjectName("experimentVideoTarget");
  auto* video_frame = new QGroupBox("Program preview");
  auto* video_layout = new QVBoxLayout(video_frame);
  video_layout->addWidget(s.video);
  left_layout->addWidget(video_frame, 1);
  s.timeline = new QSlider(Qt::Horizontal);
  s.timeline->setObjectName("experimentTimeline");
  s.timeline->setRange(0, 0);
  left_layout->addWidget(s.timeline);
  s.frame_label = new QLabel("Prepare a recording to inspect individual samples.");
  s.frame_label->setObjectName("experimentFrameStatus");
  s.frame_label->setWordWrap(true);
  left_layout->addWidget(s.frame_label);
  auto* transport = new QHBoxLayout();
  auto* previous = s.previous = new QPushButton("◀ Frame");
  previous->setObjectName("experimentPrevious");
  auto* next = s.next = new QPushButton("Frame ▶");
  next->setObjectName("experimentNext");
  s.play = new QPushButton("Play preview");
  s.play->setObjectName("experimentPlay");
  s.play->setEnabled(false);
  s.loop = new QCheckBox("Repeat range");
  s.loop->setObjectName("experimentLoop");
  s.loop->setChecked(true);
  s.comparison = new QComboBox();
  s.comparison->setObjectName("experimentComparison");
  s.comparison->setMinimumWidth(180);
  transport->addWidget(previous);
  transport->addWidget(s.play);
  transport->addWidget(next);
  transport->addWidget(s.loop);
  transport->addStretch();
  transport->addWidget(new QLabel("View"));
  transport->addWidget(s.comparison);
  left_layout->addLayout(transport);
  s.plot = new CameraPathPlot(left);
  s.plot->setObjectName("experimentCameraPlot");
  left_layout->addWidget(s.plot);
  splitter->addWidget(left);

  auto* right = new QWidget();
  right->setMinimumWidth(390);
  auto* right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(4, 0, 0, 0);
  auto* help = new QLabel(
      camera_controls.empty()
          ? "Enable only the parameters to change. Unchecked controls preserve their recorded values."
          : "Trials start with the main window’s current camera controls. Uncheck a parameter to use its recorded value.");
  help->setWordWrap(true);
  right_layout->addWidget(help);
  auto* targets = new QHBoxLayout();
  s.fast = new QCheckBox("Fast box");
  s.fast->setObjectName("experimentFastBox");
  s.follower = new QCheckBox("Program / follower box");
  s.follower->setObjectName("experimentFollowerBox");
  s.follower->setChecked(true);
  if (const auto value = camera_controls.find("Apply_To_Fast_Box"); value != camera_controls.end())
    s.fast->setChecked(value->second != 0);
  if (const auto value = camera_controls.find("Apply_To_Follower_Box"); value != camera_controls.end())
    s.follower->setChecked(value->second != 0);
  targets->addWidget(s.fast);
  targets->addWidget(s.follower);
  right_layout->addLayout(targets);
  auto* tabs = new QTabWidget();
  s.parameter_tabs = tabs;
  tabs->setObjectName("experimentControlTabs");
  const auto defaults = [](const QString& id) { return id == "Zoom_In_Aggressiveness" ? 25 : 0; };
  auto control_tab = [&](const QString& label, const std::vector<CameraSliderSpec>& specs) {
    auto* content = new QWidget();
    auto* form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const auto& spec : specs)
      s.add_control(form, spec);
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    tabs->addTab(scroll, label);
  };
  control_tab("Tracking", tracking_control_specs(defaults));
  control_tab("Motion", motion_control_specs(defaults));
  control_tab(
      "Players",
      {{"Ignore_Largest_Count",
        "Ignore largest players",
        0,
        std::numeric_limits<int>::max(),
        0,
        "ignore-largest-bbox-count"},
       {"Ignore_Oversized_Players", "Ignore oversized players (0 / 1)", 0, 1, 0, "ignore-oversized-bboxes"},
       {"Oversized_Player_Percent", "Larger than average (%)", 0, 1000, 50, "oversized-bbox-percent"}});
  auto* legacy = new QWidget();
  auto* legacy_layout = new QFormLayout(legacy);
  auto* legacy_help = new QLabel(
      "Older exports require historical arena coordinates. Supply the original resolved policy if its archived file is missing. Reconstruction must pass camera trajectory verification.");
  legacy_help->setWordWrap(true);
  legacy_layout->addRow(legacy_help);
  s.legacy_arena = new QLineEdit();
  s.legacy_arena->setObjectName("experimentLegacyArena");
  s.legacy_arena->setPlaceholderText("left, top, right, bottom");
  legacy_layout->addRow("Historical arena", s.legacy_arena);
  s.legacy_config = new QLineEdit();
  s.legacy_config->setObjectName("experimentLegacyConfig");
  s.legacy_config->setPlaceholderText("Original resolved policy YAML (optional)");
  legacy_layout->addRow("Policy file", s.legacy_config);
  tabs->addTab(legacy, "Legacy");
  right_layout->addWidget(tabs, 1);
  s.trial_name = new QLineEdit("Trial 1");
  s.trial_name->setObjectName("experimentTrialName");
  s.trial_name->setMaxLength(100);
  right_layout->addWidget(s.trial_name);
  s.apply = new QPushButton("Apply && replay");
  s.apply->setObjectName("experimentApply");
  s.apply->setEnabled(false);
  s.apply->setStyleSheet(
      "QPushButton{background:#117866;color:white;padding:10px;font-weight:600;} QPushButton:disabled{background:#a5bdb7;}");
  right_layout->addWidget(s.apply);
  auto* outputs = new QHBoxLayout();
  s.save = new QPushButton("Save trial…");
  s.save->setObjectName("experimentSave");
  s.save->setEnabled(false);
  s.capture = new QPushButton("Screenshot…");
  s.capture->setObjectName("experimentScreenshot");
  s.capture->setEnabled(false);
  outputs->addWidget(s.save);
  outputs->addWidget(s.capture);
  right_layout->addLayout(outputs);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);
  root->addWidget(splitter, 1);
  s.provenance = new QLabel("Historical state has not been prepared.");
  s.provenance->setObjectName("experimentProvenance");
  s.provenance->setWordWrap(true);
  root->addWidget(s.provenance);
  s.progress = new QProgressBar();
  s.progress->setObjectName("experimentProgress");
  s.progress->setRange(0, 0);
  s.progress->setTextVisible(false);
  s.progress->setFixedHeight(8);
  s.progress->hide();
  root->addWidget(s.progress);
  s.activity = new QLabel();
  s.activity->setObjectName("experimentActivity");
  s.activity->hide();
  root->addWidget(s.activity);
  s.status = new QLabel("Select a completed DriveGPT recording and a short range, then prepare its historical start.");
  s.status->setObjectName("experimentStatus");
  s.status->setWordWrap(true);
  root->addWidget(s.status);
  auto* bottom = new QHBoxLayout();
  s.prepare = new QPushButton("Prepare historical start");
  s.prepare->setObjectName("experimentPrepare");
  s.cancel = new QPushButton("Cancel preparation");
  s.cancel->setObjectName("experimentCancel");
  s.cancel->setEnabled(false);
  auto* close = new QPushButton("Close");
  bottom->addWidget(s.prepare);
  bottom->addWidget(s.cancel);
  bottom->addStretch();
  bottom->addWidget(close);
  root->addLayout(bottom);

  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(s.prepare, &QPushButton::clicked, this, [this]() { impl_->prepare_session(); });
  connect(s.apply, &QPushButton::clicked, this, [this]() { impl_->run_trial(); });
  connect(s.cancel, &QPushButton::clicked, this, [this]() {
    if (impl_->cancellation)
      impl_->cancellation->store(true);
    impl_->close_preview();
    impl_->show_status("Cancelling background work…");
  });
  auto update_source_mode = [&s]() {
    s.game_path->parentWidget()->setEnabled(s.original_cameras());
    s.media_path->parentWidget()->setEnabled(!s.original_cameras());
    s.uncropped->setText(
        s.original_cameras() ? "These are the corresponding sources and stitching geometry"
                             : "This is the corresponding uncropped canvas, without padding");
  };
  connect(s.source_mode, &QComboBox::currentIndexChanged, this, [this, update_source_mode]() {
    update_source_mode();
    impl_->uncropped->setChecked(false);
    impl_->invalidate_source();
  });
  // A configured game defaults to direct replay; empty standalone dialogs
  // retain the metadata-only / optional panorama workflow.
  s.source_mode->setCurrentIndex(QFileInfo(QDir(game_directory).filePath("config.yaml")).isFile() ? 0 : 1);
  update_source_mode();
  connect(s.in, &QDoubleSpinBox::valueChanged, s.video_origin, &QDoubleSpinBox::setValue);
  for (QLineEdit* source : {s.manifest, s.media_path, s.game_path, s.legacy_arena, s.legacy_config})
    connect(source, &QLineEdit::textChanged, this, [this]() { impl_->invalidate_source(); });
  connect(s.run_id, &QComboBox::currentIndexChanged, this, [this]() { impl_->invalidate_source(); });
  for (QDoubleSpinBox* source : {s.in, s.duration})
    connect(source, &QDoubleSpinBox::valueChanged, this, [this]() { impl_->invalidate_source(); });
  connect(s.video_origin, &QDoubleSpinBox::valueChanged, this, [this]() { impl_->close_preview(); });
  connect(s.uncropped, &QCheckBox::toggled, this, [this](bool checked) {
    if (!checked) {
      impl_->close_preview();
    }
  });
  connect(s.comparison, &QComboBox::currentIndexChanged, this, [this](int) { impl_->select_comparison(); });
  connect(s.timeline, &QSlider::sliderReleased, this, [this]() { impl_->seek(impl_->timeline->value()); });
  connect(previous, &QPushButton::clicked, this, [this]() {
    if (impl_->current > 0)
      impl_->seek(impl_->current - 1);
  });
  connect(next, &QPushButton::clicked, this, [this]() { impl_->seek(impl_->current + 1); });
  connect(s.loop, &QCheckBox::toggled, this, [this](bool checked) {
    if (impl_->preview_open)
      impl_->preview.Submit([checked](auto& engine, auto*) {
        engine.SetLoop(checked);
        return true;
      });
  });
  connect(s.play, &QPushButton::clicked, this, [this]() {
    auto& state = *impl_;
    if (state.playing) {
      state.preview.Submit([](auto& engine, auto*) {
        engine.Pause();
        return true;
      });
      state.playing = false;
      state.update_controls();
      return;
    }
    if (state.preview_open) {
      const auto index = state.current;
      state.preview.Submit([index](auto& engine, auto* error) { return engine.Seek(index, true, error); });
      state.playing = true;
      state.update_controls();
    } else {
      state.open_preview(true, state.current);
    }
  });
  connect(s.save, &QPushButton::clicked, this, [this]() {
    auto& state = *impl_;
    const int index = state.comparison->currentIndex() - 2;
    if (!state.session || state.source_dirty || state.work.valid() || index < 0 ||
        static_cast<std::size_t>(index) >= state.trials.size())
      return;
    const QString path =
        QFileDialog::getSaveFileName(this, "Save camera experiment", "camera-trial.yaml", "YAML (*.yaml)");
    if (path.isEmpty())
      return;
    const auto media = state.media_binding();
    const auto status = state.session->SaveTrial(path.toStdString(), state.trials[index], media);
    state.show_status(
        status.ok() ? "Trial and its source/video binding saved." : QString::fromStdString(status.ToString()),
        !status.ok());
  });
  connect(s.capture, &QPushButton::clicked, this, [this]() {
    const QString path =
        QFileDialog::getSaveFileName(this, "Save camera preview screenshot", "camera-experiment.png", "PNG (*.png)");
    if (path.isEmpty())
      return;
    QString error;
    if (!captureScreenshot(path, &error))
      impl_->show_status(error, true);
    else
      impl_->show_status("Experiment screenshot saved with the presented GPU frame.");
  });
  auto* poll = new QTimer(this);
  connect(poll, &QTimer::timeout, this, [this]() { impl_->poll(); });
  poll->start(30);
  s.update_controls();
}

CameraExperimentDialog::~CameraExperimentDialog() {
  if (impl_->cancellation)
    impl_->cancellation->store(true);
  if (impl_->work.valid())
    impl_->work.wait();
  // Fence the renderer while its Qt native child is still alive.
  impl_->preview.Close();
}

void CameraExperimentDialog::done(int result) {
  const auto state = impl_->preview.Poll();
  if (!impl_->work.valid() && !state.busy && !state.open) {
    QDialog::done(result);
    return;
  }
  if (!impl_->closing_result) {
    impl_->closing_result = result;
    if (impl_->cancellation)
      impl_->cancellation->store(true);
    impl_->close_preview();
  }
}

void CameraExperimentDialog::closeEvent(QCloseEvent* event) {
  const auto state = impl_->preview.Poll();
  if (impl_->work.valid() || state.busy || state.open) {
    event->ignore();
    done(QDialog::Rejected);
  } else {
    QDialog::closeEvent(event);
  }
}

bool CameraExperimentDialog::captureScreenshot(const QString& path, QString* error) {
  QPixmap screenshot = grab();
  if (impl_->preview_open) {
    QTemporaryDir temporary;
    const QString frame_path = temporary.filePath("preview.png");
    std::string capture_error;
    if (!impl_->preview.Capture(frame_path.toStdString(), &capture_error)) {
      if (error)
        *error = QString::fromStdString(capture_error);
      return false;
    }
    const QImage frame(frame_path);
    QPainter painter(&screenshot);
    const QPoint top_left = impl_->video->mapTo(this, QPoint(0, 0));
    const QRect target(top_left, impl_->video->size());
    painter.fillRect(target, Qt::black);
    const QSize fitted = frame.size().scaled(target.size(), Qt::KeepAspectRatio);
    const QRect fitted_rect(target.center() - QPoint(fitted.width() / 2, fitted.height() / 2), fitted);
    painter.drawImage(fitted_rect, frame);
  }
  if (!screenshot.save(path)) {
    if (error)
      *error = "Could not save the experiment screenshot.";
    return false;
  }
  return true;
}
