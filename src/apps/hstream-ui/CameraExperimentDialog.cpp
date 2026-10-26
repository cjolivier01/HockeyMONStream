#include "src/apps/hstream-ui/CameraExperimentDialog.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/recording/Database.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/ArchiveCatalog.h"
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include "src/apps/hstream-ui/CameraControlSpecs.h"
#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"
#include "src/apps/hstream-ui/CameraExperimentSource.h"
#include "src/apps/hstream-ui/CameraPlotSeries.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygon>
#include <QtWidgets/QApplication>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace {
namespace replay = hm::playtracker_replay;

class ExperimentTimeEdit : public QDoubleSpinBox {
 protected:
  QString textFromValue(double seconds) const override {
    return hm::ui::FormatHighlightTime(std::llround(seconds * 1000));
  }
  double valueFromText(const QString& text) const override {
    qint64 ms = 0;
    return hm::ui::ParseHighlightTime(text, &ms) ? ms / 1000.0 : value();
  }
  QValidator::State validate(QString& text, int&) const override {
    qint64 ms = 0;
    if (hm::ui::ParseHighlightTime(text, &ms) && ms / 1000.0 >= minimum() && ms / 1000.0 <= maximum())
      return QValidator::Acceptable;
    for (const QChar c : text)
      if (!c.isDigit() && c != ':' && c != '.' && !c.isSpace())
        return QValidator::Invalid;
    return QValidator::Intermediate;
  }
};

class ExperimentVideoTarget : public hm::ui::NativePreviewSurface {
 public:
  explicit ExperimentVideoTarget(QWidget* parent) : NativePreviewSurface(parent) {
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
  std::function<void()> toggleFocus;
  std::function<void()> requestRedraw;

 protected:
  void resizeEvent(QResizeEvent* event) override {
    NativePreviewSurface::resizeEvent(event);
    if (requestRedraw)
      requestRedraw();
  }
  void showEvent(QShowEvent* event) override {
    NativePreviewSurface::showEvent(event);
    if (requestRedraw)
      requestRedraw();
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && toggleFocus) {
      toggleFocus();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }
};

// Drawings the horizontal-position plot can carry. Each is an independent
// checkbox above the plot, so a crowded passage can be narrowed to the one
// quantity under investigation.
enum PlotLayer {
  kPlotRecorded,
  kPlotProgram,
  kPlotCrop,
  kPlotFast,
  kPlotPlayers,
  kPlotCluster,
  kPlotBreakaway,
  kPlotLayerCount,
};

struct PlotLayerSpec {
  const char* id;
  const char* label;
  const char* legend; // The checkbox, on the dialog's light background.
  const char* tip;
};

constexpr PlotLayerSpec kPlotLayers[kPlotLayerCount] = {
    {"Recorded", "Recorded", "#64748b", "Horizontal center of the Program camera as it was recorded."},
    {"Program", "Program center", "#0f766e", "Horizontal center of the selected trial's Program camera."},
    {"Crop",
     "Crop width",
     "#0b6b5e",
     "Left and right edges of the Program crop. A narrow band is zoomed in; a wide one is zoomed out."},
    {"Fast", "Fast box", "#a16207", "Center and edges of the fast box that the Program camera chases."},
    {"Players",
     "Players",
     "#1d4ed8",
     "Horizontal center of every tracked player. Players the trial excluded from camera logic are dimmed."},
    {"Cluster", "Cluster", "#6d28d9", "Horizontal extent of the player cluster the policy framed."},
    {"Breakaway",
     "Breakaway",
     "#b42318",
     "Marks the frames where the policy is catching a breakaway, with the leading edge it is chasing and the "
     "frame it is trying to reach."},
};

std::optional<double> box_center(const std::optional<hm::BBox>& box) {
  if (!box)
    return std::nullopt;
  return (box->left + box->right) / 2.0;
}

std::optional<std::pair<double, double>> box_extent(const std::optional<hm::BBox>& box) {
  if (!box)
    return std::nullopt;
  return std::pair<double, double>(box->left, box->right);
}

class CameraPathPlot : public QWidget {
 public:
  explicit CameraPathPlot(QWidget* parent, const YAML::Node& defaults) : QWidget(parent) {
    setMinimumHeight(190);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName("Visual timeline");
    setToolTip("Click to show a frame. Press Play preview to continue from there.");
    for (int i = 0; i < kPlotLayerCount; ++i)
      layers[i] = defaults["plot_layers"][kPlotLayers[i].id].as<bool>();
  }
  std::shared_ptr<const std::vector<replay::Frame>> original;
  std::shared_ptr<const std::vector<replay::Frame>> trial;
  unsigned canvas_width{1};
  std::size_t frame{0};
  std::array<bool, kPlotLayerCount> layers{};
  std::function<void(std::size_t)> selectFrame;

 protected:
  QRectF plotRect() const {
    return QRectF(14, 32, width() - 28, height() - 84);
  }
  void mousePressEvent(QMouseEvent* event) override {
    const QRectF plot = plotRect();
    if (event->button() == Qt::LeftButton && trial && !trial->empty() && selectFrame && plot.width() > 0 &&
        plot.contains(event->position())) {
      const double fraction = std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
      selectFrame(static_cast<std::size_t>(std::llround(fraction * (trial->size() - 1))));
      event->accept();
      return;
    }
    QWidget::mousePressEvent(event);
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#101b2d"));
    p.setPen(QColor("#c7d2e1"));
    // Naming the axis range is what makes a band readable: a crop drawn a third
    // of the way up the plot means nothing without knowing the canvas it spans.
    p.drawText(
        14,
        22,
        trial ? QString("Visual timeline  ·  Horizontal position: 0–%1 px").arg(canvas_width)
              : QString("Visual timeline"));
    // Two caption lines are reserved: with every layer enabled the readout is
    // long enough to wrap, and silently clipping a value defeats the point.
    const QRectF plot = plotRect();
    p.setPen(QPen(QColor("#26364d"), 1));
    for (int i = 0; i <= 4; ++i)
      p.drawLine(
          QPointF(plot.left(), plot.top() + plot.height() * i / 4),
          QPointF(plot.right(), plot.top() + plot.height() * i / 4));

    const double span = std::max(1U, canvas_width);
    const auto column = [&](std::size_t index, std::size_t count) {
      return hm::ui::PlotColumnX(index, count, plot.left(), plot.width());
    };
    const auto row = [&](double position) { return plot.bottom() - plot.height() * position / span; };
    // Drawing cost follows the widget rather than the clip; see ReducePlotColumns.
    const auto reduce = [&](const std::vector<replay::Frame>& frames, const auto& extent) {
      return hm::ui::ReducePlotColumns(
          frames.size(), plot.left(), plot.width(), [&](std::size_t i) { return extent(frames[i]); });
    };
    // One subpath per contiguous run, so an absent box breaks the drawing
    // instead of bridging across the gap it left. Where a column covers a
    // range the line spans it, which keeps a fast swing from being thinned
    // down to whichever frame happened to be sampled.
    const auto trace = [&](const std::vector<replay::Frame>& frames, const auto& value, const QPen& pen) {
      QPainterPath path;
      bool started = false;
      const auto as_range = [&](const replay::Frame& f) -> std::optional<std::pair<double, double>> {
        const std::optional<double> position = value(f);
        if (!position)
          return std::nullopt;
        return std::pair<double, double>(*position, *position);
      };
      for (const hm::ui::PlotColumn& c : reduce(frames, as_range)) {
        if (!c.range) {
          started = false;
          continue;
        }
        const QPointF point(c.x, row(c.range->first));
        if (started)
          path.lineTo(point);
        else
          path.moveTo(point);
        started = true;
        if (c.range->second != c.range->first)
          path.lineTo(QPointF(c.x, row(c.range->second)));
      }
      p.setBrush(Qt::NoBrush);
      p.setPen(pen);
      p.drawPath(path);
    };
    const auto band =
        [&](const std::vector<replay::Frame>& frames, const auto& extent, const QColor& fill, const QPen& edge) {
          QPolygonF low, high;
          const auto flush = [&]() {
            if (low.size() > 1) {
              QPolygonF area = high;
              for (qsizetype i = low.size() - 1; i >= 0; --i)
                area << low[i];
              p.setPen(Qt::NoPen);
              p.setBrush(fill);
              p.drawPolygon(area);
              p.setBrush(Qt::NoBrush);
              p.setPen(edge);
              p.drawPolyline(low);
              p.drawPolyline(high);
            } else if (low.size() == 1) {
              // A run only one column wide has no area, but it still happened.
              p.setBrush(Qt::NoBrush);
              p.setPen(edge);
              p.drawLine(low[0], high[0]);
            }
            low.clear();
            high.clear();
          };
          for (const hm::ui::PlotColumn& c : reduce(frames, extent)) {
            if (!c.range) {
              flush();
              continue;
            }
            low << QPointF(c.x, row(c.range->first));
            high << QPointF(c.x, row(c.range->second));
          }
          flush();
        };

    p.setClipRect(plot);
    if (trial) {
      if (layers[kPlotCluster])
        band(
            *trial,
            [](const replay::Frame& f) { return box_extent(f.tracking.cluster); },
            QColor(167, 139, 250, 45),
            QPen(QColor(167, 139, 250, 150), 1));
      if (layers[kPlotBreakaway])
        band(
            *trial,
            [](const replay::Frame& f) { return box_extent(f.tracking.breakaway_target); },
            QColor(255, 95, 86, 30),
            QPen(QColor(255, 95, 86, 170), 1, Qt::DashLine));
      if (layers[kPlotFast])
        band(
            *trial,
            [](const replay::Frame& f) { return box_extent(f.fast); },
            QColor(246, 190, 98, 28),
            QPen(QColor(246, 190, 98, 120), 1));
      if (layers[kPlotCrop])
        band(
            *trial,
            [](const replay::Frame& f) { return box_extent(f.follower); },
            QColor(47, 179, 160, 55),
            QPen(QColor(47, 179, 160, 200), 1));
      if (layers[kPlotPlayers]) {
        QPolygonF tracked, excluded;
        int drawn_column = std::numeric_limits<int>::min();
        for (std::size_t i = 0; i < trial->size(); ++i) {
          const auto& overlays = trial->at(i).overlays;
          if (!overlays || overlays->coordinate_width <= 0)
            continue;
          const double x = column(i, trial->size());
          // One sample per pixel column keeps a long passage legible and bounds
          // the point count by the plot's width rather than the clip's length.
          if (static_cast<int>(x) == drawn_column)
            continue;
          drawn_column = static_cast<int>(x);
          const double scale = span / overlays->coordinate_width;
          for (const auto& player : overlays->player_rects)
            (player.ignored ? excluded : tracked)
                << QPointF(x, row((player.rect.left + player.rect.width / 2.0) * scale));
        }
        p.setPen(QPen(QColor("#41617f"), 2));
        p.drawPoints(excluded);
        p.setPen(QPen(QColor("#5fa8e8"), 2));
        p.drawPoints(tracked);
      }
    }
    if (original && layers[kPlotRecorded])
      trace(*original, [](const replay::Frame& f) { return box_center(f.follower); }, QPen(QColor("#94a3b8"), 2));
    if (trial && layers[kPlotFast])
      trace(*trial, [](const replay::Frame& f) { return box_center(f.fast); }, QPen(QColor("#f6be62"), 2));
    if (trial && layers[kPlotProgram])
      trace(*trial, [](const replay::Frame& f) { return box_center(f.follower); }, QPen(QColor("#41dbc0"), 2));
    if (trial && layers[kPlotBreakaway]) {
      if (layers[kPlotProgram])
        trace(
            *trial,
            [](const replay::Frame& f) -> std::optional<double> {
              return f.tracking.breakaway_edge ? box_center(f.follower) : std::nullopt;
            },
            QPen(QColor("#ff5f56"), 2));
      trace(
          *trial,
          [](const replay::Frame& f) -> std::optional<double> {
            if (!f.tracking.breakaway_edge)
              return std::nullopt;
            return f.tracking.breakaway_edge->x;
          },
          QPen(QColor("#ff5f56"), 1, Qt::DotLine));
      // A strip along the top stays visible when the Program line is hidden.
      // Catching a breakaway is a yes or no per frame, so the marked columns
      // are the ones holding at least one such frame.
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 95, 86, 150));
      const std::vector<hm::ui::PlotColumn> catching =
          reduce(*trial, [](const replay::Frame& f) -> std::optional<std::pair<double, double>> {
            if (!f.tracking.breakaway_edge)
              return std::nullopt;
            return std::pair<double, double>(0, 0);
          });
      for (std::size_t i = 0; i < catching.size(); ++i) {
        if (!catching[i].range)
          continue;
        const double x = catching[i].x;
        const double next = i + 1 < catching.size() ? catching[i + 1].x : x + 1;
        p.drawRect(QRectF(x, plot.top() + 1, std::max(1.0, next - x), 3));
      }
      p.setBrush(Qt::NoBrush);
    }
    if (trial && trial->size() > 1) {
      const double x = column(std::min(frame, trial->size() - 1), trial->size());
      p.setPen(QPen(QColor("#e2e8f0"), 1, Qt::DashLine));
      p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    p.setClipping(false);
    p.setPen(QColor("#8ea3bd"));
    QFont caption = p.font();
    if (caption.pointSizeF() > 0)
      caption.setPointSizeF(caption.pointSizeF() * 0.85);
    p.setFont(caption);
    p.drawText(
        QRectF(14, plot.bottom() + 6, width() - 28, 38),
        Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
        readout().join("   ·   "));
  }

 private:
  // Reports the enabled drawings at the playhead, so a value can be read off
  // exactly instead of estimated against the gridlines.
  QStringList readout() const {
    if (!trial || trial->empty())
      return {"Prepare a recording to plot its camera and play tracking."};
    const double span = std::max(1U, canvas_width);
    const replay::Frame& f = trial->at(std::min(frame, trial->size() - 1));
    QStringList parts;
    // Every series holds one frame per sample, so the playhead index addresses
    // all of them alike; see ReplaySession::RunTrial.
    if (layers[kPlotRecorded] && original && !original->empty())
      if (const auto center = box_center(original->at(std::min(frame, original->size() - 1)).follower))
        parts << QString("recorded %1 px").arg(*center, 0, 'f', 0);
    if (layers[kPlotProgram])
      if (const auto center = box_center(f.follower))
        parts << QString("center %1 px").arg(*center, 0, 'f', 0);
    if (layers[kPlotCrop] && f.follower)
      parts << QString("crop %1 px · %2% of canvas")
                   .arg(f.follower->right - f.follower->left, 0, 'f', 0)
                   .arg(100.0 * (f.follower->right - f.follower->left) / span, 0, 'f', 0);
    if (layers[kPlotFast]) {
      if (const auto center = box_center(f.fast))
        parts << QString("fast center %1 px").arg(*center, 0, 'f', 0);
      if (f.fast)
        parts << QString("fast width %1 px").arg(f.fast->right - f.fast->left, 0, 'f', 0);
    }
    if (layers[kPlotPlayers] && f.overlays) {
      std::size_t excluded = 0;
      for (const auto& player : f.overlays->player_rects)
        excluded += player.ignored ? 1 : 0;
      parts << QString("%1 players · %2 excluded")
                   .arg(static_cast<qulonglong>(f.overlays->player_rects.size()))
                   .arg(static_cast<qulonglong>(excluded));
    }
    if (layers[kPlotCluster] && f.tracking.cluster)
      parts << QString("cluster %1–%2 px")
                   .arg(f.tracking.cluster->left, 0, 'f', 0)
                   .arg(f.tracking.cluster->right, 0, 'f', 0);
    if (layers[kPlotBreakaway]) {
      parts
          << (f.tracking.breakaway_edge
                  ? QString("catching breakaway at %1 px").arg(f.tracking.breakaway_edge->x, 0, 'f', 0)
                  : QString("no breakaway"));
      if (f.tracking.breakaway_target)
        parts << QString("breakaway target %1–%2 px")
                     .arg(f.tracking.breakaway_target->left, 0, 'f', 0)
                     .arg(f.tracking.breakaway_target->right, 0, 'f', 0);
    }
    return parts;
  }
};

QString completed_manifest(const QString& directory) {
  const QDir dir(directory);
  for (const QFileInfo& info : dir.entryInfoList({"*_telemetry*.db"}, QDir::Files, QDir::Time)) {
    try {
      if (!hm::recording::TelemetryDatabaseGeneration(info.fileName().toStdString()))
        continue;
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
    std::optional<replay::TrialResult> native_trial;
    std::shared_ptr<const hm::gpu_preview::RinkMaskImage> rink_mask;
    std::string rink_mask_reason;
    std::string error;
    std::pair<unsigned, unsigned> media_size;
    QString media_path;
    std::optional<replay::StitchingMedia> stitching;
    std::shared_ptr<const hm::stitching::PlaybackRinkMap> rink_map;
    std::string rink_map_reason;
  };
  CameraExperimentDialog* dialog;
  QLineEdit* manifest{nullptr};
  QComboBox* run_id{nullptr};
  QLineEdit* media_path{nullptr};
  QLineEdit* game_path{nullptr};
  QComboBox* source_mode{nullptr};
  QDoubleSpinBox* in{nullptr};
  QDoubleSpinBox* playback_start{nullptr};
  QDoubleSpinBox* duration{nullptr};
  QDoubleSpinBox* video_origin{nullptr};
  QLineEdit* legacy_arena{nullptr};
  QLineEdit* legacy_config{nullptr};
  QCheckBox* uncropped{nullptr};
  QCheckBox* fast{nullptr};
  QCheckBox* follower{nullptr};
  QCheckBox* loop{nullptr};
  QCheckBox* realtime{nullptr};
  QCheckBox* draw_players{nullptr};
  QCheckBox* draw_play{nullptr};
  QCheckBox* draw_rink{nullptr};
  QCheckBox* rink_aware{nullptr};
  QCheckBox* rink_anticipate{nullptr};
  QCheckBox* rink_goals{nullptr};
  QCheckBox* rink_zoom{nullptr};
  QLabel* rink_status{nullptr};
  std::shared_ptr<const hm::stitching::PlaybackRinkMap> rink_map;
  std::shared_ptr<const hm::gpu_preview::RinkMaskImage> rink_mask;
  QLineEdit* trial_name{nullptr};
  QComboBox* comparison{nullptr};
  QPushButton* prepare{nullptr};
  QPushButton* apply{nullptr};
  QPushButton* cancel{nullptr};
  QPushButton* play{nullptr};
  QPushButton* stop_preview{nullptr};
  PreviewFocusButton* focus_button{nullptr};
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
  bool preview_loading{false};
  std::optional<int> closing_result;
  ExperimentVideoTarget* video{nullptr};
  QSplitter* splitter{nullptr};
  QWidget* right_panel{nullptr};
  QWidget* plot_panel{nullptr};
  CameraPathPlot* plot{nullptr};
  std::vector<QWidget*> preview_focus_hidden;
  QList<int> normal_splitter_sizes;
  bool preview_focused{false};
  std::vector<Control> controls;
  std::map<QString, double> initial_controls;
  QString trial_prefix;
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
    qint64 media_ms = std::llround((playback_start->value() + video_origin->value()) * 1000);
    hm::ui::ArchiveEntry archive;
    if (!prepared_stitching && hm::ui::LoadArchiveSidecar(prepared_media, &archive) && archive.start_time_known)
      media_ms -= archive.start_time_ms;
    return {
        prepared_stitching ? QDir(prepared_media).filePath("config.yaml").toStdString() : prepared_media.toStdString(),
        selected->front().pts_ns,
        media_ms < 0 ? std::numeric_limits<uint64_t>::max() : static_cast<std::uint64_t>(media_ms) * 1000000,
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

  void set_preview_focus(bool focused) {
    if (focused == preview_focused)
      return;
    const bool remap_video = video->isVisible();
    if (remap_video)
      video->hide();
    if (focused) {
      normal_splitter_sizes = splitter->sizes();
      preview_focus_hidden.clear();
      for (QWidget* widget : dialog->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (widget != splitter && widget->isVisible()) {
          preview_focus_hidden.push_back(widget);
          widget->hide();
        }
      }
      for (QWidget* widget : {right_panel, plot_panel}) {
        if (widget->isVisible()) {
          preview_focus_hidden.push_back(widget);
          widget->hide();
        }
      }
    } else {
      for (QWidget* widget : preview_focus_hidden)
        widget->show();
      preview_focus_hidden.clear();
    }
    preview_focused = focused;
    focus_button->setFocused(focused);
    dialog->layout()->activate();
    if (!focused)
      splitter->setSizes(normal_splitter_sizes);
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (remap_video) {
      video->show();
      video->raise();
    }
    update_controls();
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
    // A stopped preview owns a snapshot of its inputs. Source edits remain safe
    // while that graph is being released, without stealing focus on each key.
    settings->setEnabled(!calculating && !closing_result && !playing && (!loading || !preview_open));
    parameter_tabs->setEnabled(editable);
    rink_aware->setEnabled(editable && prepared && bool(rink_map));
    for (auto* option : {rink_anticipate, rink_goals, rink_zoom})
      option->setEnabled(editable && prepared && bool(rink_map) && rink_aware->isChecked());
    fast->setEnabled(editable);
    follower->setEnabled(editable);
    trial_name->setEnabled(editable);
    comparison->setEnabled(!active && prepared);
    plot->setEnabled(!calculating && !closing_result && prepared);
    timeline->setEnabled(!active && prepared && !playing);
    previous->setEnabled(!active && prepared && !playing);
    next->setEnabled(!active && prepared && !playing);
    realtime->setEnabled(editable);
    draw_players->setEnabled(!active);
    draw_play->setEnabled(!active);
    draw_rink->setEnabled(!active && prepared && bool(rink_mask));
    play->setEnabled(!calculating && !closing_result && prepared && (!loading || playing));
    play->setText(playing ? "Pause" : "Play preview");
    play->setIcon(action_icon(playing ? ActionIcon::Pause : ActionIcon::Play));
    stop_preview->setEnabled((preview_open || preview_focused) && !closing_result);
    save->setEnabled(editable && prepared && uncropped->isChecked() && comparison->currentIndex() >= 2);
    capture->setEnabled(!active && prepared && frame_ready);
    loop->setEnabled(!calculating && !closing_result);
    progress->setVisible(active && !preview_focused);
    activity->setVisible((active || playing) && !preview_focused);
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
    preview_loading = false;
    set_preview_focus(false);
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
    value->setDecimals(spec.divisor == 1 ? 0 : spec.divisor == 10 ? 1 : 2);
    if (std::string(spec.id) == "Oversized_Player_Percent") {
      // Match the main UI's native percentage domain without rounding or
      // clamping a copied value before it becomes an enabled trial override.
      value->setDecimals(std::numeric_limits<double>::max_digits10);
      value->setRange(0, std::numeric_limits<double>::max());
      value->setMaximumWidth(180);
    }
    value->setSingleStep(1.0 / spec.divisor);
    value->setValue(spec.default_value / static_cast<double>(spec.divisor));
    value->setEnabled(false);
    value->setToolTip(
        "Enable this override to change it at the selected start frame. Unchecked controls retain recorded values.");
    QObject::connect(enabled, &QCheckBox::toggled, value, &QWidget::setEnabled);
    const auto initial = initial_controls.find(QString::fromLatin1(spec.id));
    if (initial != initial_controls.end()) {
      const double copied = initial->second / spec.divisor;
      // Loaded main-window controls can extend beyond the presentation ranges.
      value->setRange(std::min(value->minimum(), copied), std::max(value->maximum(), copied));
      value->setValue(copied);
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
    options.prepare_overlays = true;
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
    const qint64 playback_ms = std::llround(playback_start->value() * 1000);
    work = std::async(std::launch::async, [options, token, media, raw, playback_ms]() mutable {
      WorkResult result;
      try {
        const qint64 recorded_origin = CameraExperimentRecordingSourceOrigin(options).value_or(playback_ms);
        options.start_seconds += (playback_ms - recorded_origin) / 1000.0;
        if (options.start_seconds < 0) {
          result.error = "Playback start plus Start offset precedes the selected recording";
          return result;
        }
      } catch (const std::exception& error) {
        result.error = error.what();
        return result;
      }
      auto prepared = replay::ReplaySession::Prepare(options, token.get());
      if (!prepared.ok()) {
        result.error = prepared.status().ToString();
        return result;
      }
      result.session = *prepared;
      result.rink_mask_reason = result.session->rink_mask_unavailable_reason();
      const auto& png = result.session->rink_mask_png();
      if (!png.empty()) {
        hm::gpu_preview::RinkMaskLoadOptions mask_options;
        mask_options.downsample_to_texture_budget = true;
        mask_options.maximum_source_dimension = std::max(result.session->width(), result.session->height());
        mask_options.maximum_source_pixels = hm::gpu_preview::kMaximumPreviewRinkMaskSourcePixels;
        mask_options.maximum_resource_bytes = hm::gpu_preview::kMaximumPreviewRinkMaskResourceBytes;
        auto decoded = hm::gpu_preview::decode_rink_mask_png({png.begin(), png.end()}, {}, mask_options);
        if (decoded &&
            hm::gpu_preview::rink_mask_dimensions_match(
                decoded.image, result.session->width(), result.session->height())) {
          result.rink_mask = std::make_shared<const hm::gpu_preview::RinkMaskImage>(std::move(decoded.image));
        } else {
          result.rink_mask_reason =
              decoded ? "Archived rink mask does not match the recorded canvas." : decoded.message;
        }
      }
      result.media_path = media;
      if (raw) {
        auto sources = PrepareExperimentSources(media.toStdString(), result.session->width(), result.session->height());
        if (!sources.ok())
          result.error = sources.status().ToString();
        else {
          result.stitching = std::move(*sources);
          result.media_size = {result.session->width(), result.session->height()};
          auto map = hm::stitching::LoadPlaybackRinkMap(media.toStdString(), [token] { return token->load(); });
          if (map.ok()) {
            const auto rotation = result.session->RinkCameraRotation(**map);
            if (rotation.ok() && std::abs(*rotation - result.stitching->rotation) < 1e-8)
              result.rink_map = *map;
            else
              result.rink_map_reason = rotation.ok()
                  ? "The selected cameras use a different rotation from this recording."
                  : std::string(rotation.status().message());
          } else {
            result.rink_map_reason = std::string(map.status().message());
          }
        }
      } else if (!media.isEmpty()) {
        result.media_size = probe_media(media, &result.error);
        result.rink_map_reason = "Select Original cameras to bind a reviewed rink map to this recording.";
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
    std::optional<replay::RinkCameraSetup> rink_setup;
    if (rink_aware->isChecked()) {
      if (!rink_map) {
        show_status("Prepare original cameras with a matching reviewed rink map first.", true);
        return;
      }
      rink_setup = replay::RinkCameraSetup{
          rink_map, {rink_anticipate->isChecked(), rink_goals->isChecked(), rink_zoom->isChecked()}};
    }
    const std::string name = trial_name->text().trimmed().isEmpty()
        ? trial_prefix.toStdString() + " " + std::to_string(trials.size() + 1)
        : trial_name->text().trimmed().toStdString();
    cancellation = std::make_shared<std::atomic<bool>>(false);
    auto token = cancellation;
    close_preview();
    busy(true);
    show_status("Restoring the same starting state and calculating this trial…");
    work = std::async(std::launch::async, [prepared, parameters, token, name, rink_setup]() {
      WorkResult result;
      if (rink_setup) {
        auto native = prepared->RunTrial(name + " · native", parameters, token.get());
        if (!native.ok()) {
          result.error = native.status().ToString();
          return result;
        }
        result.native_trial = std::move(*native);
      }
      auto trial = prepared->RunTrial(
          rink_setup ? name + " · rink-aware" : name, parameters, token.get(), rink_setup ? &*rink_setup : nullptr);
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
    if (binding.video_origin_pts_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      show_status("Playback start plus Video start offset precedes the selected archive.", true);
      return false;
    }
    const auto media = binding;
    const auto frames = selected;
    const auto width = session->width();
    const auto height = session->height();
    const auto end = session->end_pts_ns();
    const auto window = video->winId(); // QWidget access stays on Qt's thread.
    const bool reopen = !preview_open;
    const bool repeat = loop->isChecked();
    const bool normal_speed = realtime->isChecked();
    const bool players = draw_players->isChecked(), play_drawing = draw_play->isChecked();
    const bool rink = draw_rink->isChecked() && bool(rink_mask);
    const auto mask = rink_mask;
    preview.Submit([media,
                    frames,
                    width,
                    height,
                    end,
                    window,
                    reopen,
                    repeat,
                    normal_speed,
                    play_video,
                    index,
                    players,
                    play_drawing,
                    rink,
                    mask](auto& engine, auto* error) {
      if (reopen &&
          !engine.Open(
              media,
              width,
              height,
              window,
              frames->front().edge_rotation_left,
              frames->front().edge_rotation_right,
              error,
              mask))
        return false;
      engine.SetOverlays(players, play_drawing, rink);
      engine.SetLoop(repeat);
      engine.SetRealtime(normal_speed);
      return engine.SetTrajectory(frames, end, error, play_video, index);
    });
    preview_open = true;
    playing = play_video;
    frame_ready = false;
    preview_loading = true;
    show_status("Loading preview… Every pass uses the prepared historical state.");
    update_controls();
    return true;
  }

  void select_comparison(bool replay_now = false) {
    if (!session || changing_selection)
      return;
    const size_t inspected = !replay_now && preview_open && !playing ? current : 0;
    const int index = comparison->currentIndex();
    if (index == 0)
      selected = std::make_shared<const std::vector<replay::Frame>>(session->original());
    else if (index == 1)
      selected = std::make_shared<const std::vector<replay::Frame>>(session->baseline());
    else if (index >= 2 && static_cast<std::size_t>(index - 2) < trials.size())
      selected = std::make_shared<const std::vector<replay::Frame>>(trials[index - 2].frames);
    else
      return;
    current = selected->empty() ? 0 : std::min(inspected, selected->size() - 1);
    plot->trial = selected;
    plot->frame = current;
    plot->update();
    capture->setEnabled(false);
    timeline->setRange(0, selected->empty() ? 0 : static_cast<int>(selected->size() - 1));
    timeline->setValue(static_cast<int>(current));
    save->setEnabled(index >= 2 && uncropped->isChecked() && !source_dirty && !work.valid());
    update_frame();
    if (!source_dirty && (preview_open || replay_now)) {
      open_preview(replay_now, current);
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
    if (frame.rink_camera) {
      const auto& guide = *frame.rink_camera;
      text += guide.active ? QString("  ·  Rink: %1 m/s · lead %2 px · width ×%3%4")
                                 .arg(guide.speed_mps, 0, 'f', 1)
                                 .arg(guide.lead_pixels, 0, 'f', 0)
                                 .arg(guide.zoom_ratio, 0, 'f', 2)
                                 .arg(guide.goal ? (guide.goal < 0 ? " · left goal" : " · right goal") : "")
                           : "  ·  Rink: waiting for consistent player motion";
    }
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
        rink_mask = std::move(result.rink_mask);
        draw_rink->setToolTip(
            rink_mask ? "Draw the rink mask archived with this recording."
                      : QString::fromStdString(result.rink_mask_reason));
        if (!rink_mask)
          draw_rink->setChecked(false);
        source_dirty = false;
        trials.clear();
        media_size = result.media_size;
        prepared_media = result.media_path;
        prepared_stitching = std::move(result.stitching);
        rink_map = std::move(result.rink_map);
        rink_status->setText(
            rink_map
                ? "Reviewed map matches this recording. Apply creates native and rink-aware views with identical camera overrides."
                : QString::fromStdString(result.rink_map_reason));
        if (!rink_map)
          rink_aware->setChecked(false);
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
        changing_selection = true;
        if (result.native_trial) {
          trials.push_back(std::move(*result.native_trial));
          comparison->addItem(QString::fromStdString(trials.back().name));
        }
        trials.push_back(std::move(*result.trial));
        comparison->addItem(QString::fromStdString(trials.back().name));
        comparison->setCurrentIndex(comparison->count() - 1);
        changing_selection = false;
        select_comparison(true);
        trial_name->setText(trial_prefix + QString(" %1").arg(trials.size() + 1));
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
        if (frame_ready && preview_loading) {
          preview_loading = false;
          show_status("Preview ready. Every pass uses the prepared historical state.");
        }
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
    const std::map<QString, double>& camera_controls,
    qint64 playback_start_ms)
    : QDialog(parent, Qt::Window), impl_(std::make_unique<Impl>(this)) {
  qApp->installEventFilter(this);
  auto& s = *impl_;
  s.initial_controls = camera_controls;
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok())
    throw std::runtime_error(baseline.status().ToString());
  const auto preferences = baseline->values["camera_experiments"];
  if (!preferences || !preferences.IsMap())
    throw std::runtime_error("Missing required camera_experiments baseline preferences");
  setWindowTitle("HStream · Camera experiments");
  setObjectName("cameraExperimentDialog");
  configure_preview_dialog_window(this);
  resize(1440, 980);
  auto* root = new QVBoxLayout(this);
  auto* title = new QLabel("Camera experiments");
  title->setStyleSheet("font-size:24px;font-weight:600;color:#152b43;");
  auto* heading = new QHBoxLayout();
  heading->addWidget(title, 1);
  auto* maximize = new PreviewDialogWindowSizeButton(this);
  maximize->setObjectName("maximizeCameraExperimentWindowButton");
  heading->addWidget(maximize, 0, Qt::AlignTop);
  root->addLayout(heading);
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
    auto* browse = new QPushButton(action_icon(ActionIcon::Open), "Browse…");
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
  const auto media_kind = preferences["media_kind"].as<std::string>();
  if (media_kind != "original-cameras" && media_kind != "panorama")
    throw std::runtime_error("Invalid camera experiment source preference");
  s.source_mode->setCurrentIndex(media_kind == "original-cameras" ? 0 : 1);
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
    auto* value = new ExperimentTimeEdit();
    value->setObjectName(name);
    value->setRange(0, maximum);
    value->setDecimals(3);
    value->setToolTip("Zero or greater: seconds (12.5), MM:SS (90:23), or HH:MM:SS, with optional milliseconds.");
    value->setValue(initial);
    range->addWidget(value);
    return value;
  };
  s.playback_start = time_control("experimentPlaybackStart", "Playback start", playback_start_ms / 1000.0, 1e8);
  s.playback_start->setToolTip("Inherited from the main window. Changes apply only to this experiment.");
  s.in = time_control("experimentIn", "Start offset", preferences["start_seconds"].as<double>(), 1e8);
  s.duration = time_control("experimentDuration", "Duration", preferences["duration_seconds"].as<double>(), 120);
  s.duration->setMinimum(0.1);
  s.video_origin = time_control(
      "experimentVideoOrigin", "Video start offset", preferences["video_offset_seconds"].as<double>(), 1e8);
  range->addStretch();
  source_layout->addRow(range);
  s.uncropped = new QCheckBox();
  s.uncropped->setObjectName("experimentUncropped");
  s.uncropped->setChecked(preferences["source_confirmed"].as<bool>());
  source_layout->addRow("Video source confirmation", s.uncropped);
  root->addWidget(s.settings);

  auto* splitter = s.splitter = new QSplitter(Qt::Horizontal);
  auto* left = new QWidget();
  auto* left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 8, 0);
  s.video = new ExperimentVideoTarget(left);
  s.video->setObjectName("experimentVideoTarget");
  auto* video_frame = new QGroupBox("Program preview");
  auto* video_layout = new QVBoxLayout(video_frame);
  auto* preview_header = new QHBoxLayout();
  s.draw_players = new QCheckBox("Player boxes");
  s.draw_players->setObjectName("experimentDrawPlayers");
  s.draw_players->setChecked(preferences["players"].as<bool>());
  s.draw_players->setToolTip("Draw recorded player boxes; gray IGNORED labels follow the selected trial's filtering.");
  s.draw_play = new QCheckBox("Play tracking");
  s.draw_play->setObjectName("experimentDrawPlayTracking");
  s.draw_play->setChecked(preferences["play"].as<bool>());
  s.draw_play->setToolTip("Draw the selected camera policy's boxes, thresholds, clusters, and movement state.");
  s.draw_rink = new QCheckBox("Rink mask");
  s.draw_rink->setObjectName("experimentDrawRinkMask");
  s.draw_rink->setChecked(preferences["rink"].as<bool>());
  s.draw_rink->setEnabled(false);
  s.draw_rink->setToolTip("Prepare a recording with an archived rink mask to enable this drawing.");
  for (auto* toggle : {s.draw_players, s.draw_play, s.draw_rink}) {
    preview_header->addWidget(toggle);
    connect(toggle, &QCheckBox::toggled, this, [this]() {
      auto& state = *impl_;
      if (!state.preview_open)
        return;
      const bool players = state.draw_players->isChecked(), play = state.draw_play->isChecked();
      const bool rink = state.draw_rink->isChecked() && bool(state.rink_mask);
      state.preview.Submit([players, play, rink](auto& engine, auto*) {
        engine.SetOverlays(players, play, rink);
        return true;
      });
      state.update_controls();
    });
  }
  preview_header->addStretch();
  s.focus_button = new PreviewFocusButton(video_frame);
  s.focus_button->setObjectName("maximizeCameraExperimentPreviewButton");
  preview_header->addWidget(s.focus_button);
  video_layout->addLayout(preview_header);
  video_layout->addWidget(s.video, 1);
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
  auto* previous = s.previous = new QPushButton(action_icon(ActionIcon::Previous), "◀ Frame");
  previous->setObjectName("experimentPrevious");
  auto* next = s.next = new QPushButton(action_icon(ActionIcon::Next), "Frame ▶");
  next->setObjectName("experimentNext");
  s.play = new QPushButton(action_icon(ActionIcon::Play), "Play preview");
  s.play->setObjectName("experimentPlay");
  s.play->setEnabled(false);
  s.stop_preview = new QPushButton(action_icon(ActionIcon::Stop), "Stop");
  s.stop_preview->setObjectName("experimentStopPreview");
  s.loop = new QCheckBox("Repeat range");
  s.loop->setObjectName("experimentLoop");
  s.loop->setChecked(preferences["loop"].as<bool>());
  s.realtime = new QCheckBox("Play at normal speed");
  s.realtime->setObjectName("experimentRealtimePlayback");
  s.realtime->setChecked(preferences["realtime"].as<bool>());
  s.realtime->setToolTip(
      "Use video timestamps to play at normal speed, retaining late frames when processing is slow. "
      "Pause or Stop to change this; it applies when you next press Play preview. "
      "Camera Experiments previews video only.");
  s.comparison = new QComboBox();
  s.comparison->setObjectName("experimentComparison");
  s.comparison->setMinimumWidth(180);
  transport->addWidget(previous);
  transport->addWidget(s.play);
  transport->addWidget(s.stop_preview);
  transport->addWidget(next);
  transport->addWidget(s.loop);
  transport->addWidget(s.realtime);
  transport->addStretch();
  transport->addWidget(new QLabel("View"));
  transport->addWidget(s.comparison);
  left_layout->addLayout(transport);
  auto* plot_panel = s.plot_panel = new QWidget(left);
  plot_panel->setObjectName("experimentCameraPlotPanel");
  auto* plot_layout = new QVBoxLayout(plot_panel);
  plot_layout->setContentsMargins(0, 0, 0, 0);
  plot_layout->setSpacing(2);
  auto* plot_header = new QHBoxLayout();
  plot_header->setContentsMargins(14, 0, 14, 0);
  s.plot = new CameraPathPlot(plot_panel, preferences);
  s.plot->setObjectName("experimentCameraPlot");
  for (int layer = 0; layer < kPlotLayerCount; ++layer) {
    const PlotLayerSpec& spec = kPlotLayers[layer];
    auto* toggle = new QCheckBox(QString::fromLatin1(spec.label));
    toggle->setObjectName(QString("experimentPlotLayer_") + QString::fromLatin1(spec.id));
    toggle->setToolTip(QString::fromLatin1(spec.tip));
    toggle->setStyleSheet(QString("color:%1;").arg(QString::fromLatin1(spec.legend)));
    toggle->setChecked(preferences["plot_layers"][spec.id].as<bool>());
    connect(toggle, &QCheckBox::toggled, this, [this, layer](bool enabled) {
      impl_->plot->layers[layer] = enabled;
      impl_->plot->update();
    });
    plot_header->addWidget(toggle);
  }
  plot_header->addStretch();
  plot_layout->addLayout(plot_header);
  plot_layout->addWidget(s.plot);
  left_layout->addWidget(plot_panel);
  splitter->addWidget(left);

  auto* right = new QWidget();
  s.right_panel = right;
  right->setObjectName("experimentControlsPanel");
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
  s.fast->setChecked(preferences["apply_to_fast_box"].as<bool>());
  s.follower = new QCheckBox("Program / follower box");
  s.follower->setObjectName("experimentFollowerBox");
  s.follower->setChecked(preferences["apply_to_follower_box"].as<bool>());

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
  const auto defaults = [&preferences](const QString& id) {
    if (id == "Apply_To_Fast_Box")
      return int(preferences["apply_to_fast_box"].as<bool>());
    if (id == "Apply_To_Follower_Box")
      return int(preferences["apply_to_follower_box"].as<bool>());
    return preferences["controls"][id.toStdString()].as<int>();
  };
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
    tabs->addTab(scroll, tab_icon(label), label);
  };
  control_tab("Tracking", tracking_control_specs(defaults));
  control_tab("Motion", motion_control_specs(defaults));
  control_tab("Players", player_control_specs(defaults));
  auto* rink_tab = new QWidget();
  auto* rink_layout = new QVBoxLayout(rink_tab);
  s.rink_aware = new QCheckBox("Experimental rink-aware framing");
  s.rink_aware->setObjectName("experimentRinkAware");
  s.rink_aware->setChecked(preferences["rink_aware"].as<bool>());
  s.rink_aware->setEnabled(false);
  rink_layout->addWidget(s.rink_aware);
  s.rink_anticipate = new QCheckBox("Lead the direction of play");
  s.rink_anticipate->setObjectName("experimentRinkAnticipate");
  s.rink_anticipate->setChecked(preferences["anticipate"].as<bool>());
  s.rink_goals = new QCheckBox("Include the nearby goal during end-zone play");
  s.rink_goals->setObjectName("experimentRinkGoals");
  s.rink_goals->setChecked(preferences["frame_goals"].as<bool>());
  s.rink_zoom = new QCheckBox("Widen for rushes; tighten when play settles");
  s.rink_zoom->setObjectName("experimentRinkZoom");
  s.rink_zoom->setChecked(preferences["adaptive_zoom"].as<bool>());
  for (auto* option : {s.rink_anticipate, s.rink_goals, s.rink_zoom}) {
    option->setEnabled(false);
    rink_layout->addWidget(option);
  }
  s.rink_status = new QLabel("Prepare original cameras with a reviewed rink map to try this option.");
  s.rink_status->setObjectName("experimentRinkStatus");
  s.rink_status->setWordWrap(true);
  rink_layout->addWidget(s.rink_status);
  auto* rink_help = new QLabel(
      "Uses collective player motion, without puck or team identification. "
      "The adjustment is limited and smoothed, and falls back to the native camera when motion is uncertain. "
      "Trials affect this experiment only. Ice-mask filtering is unchanged.");
  rink_help->setWordWrap(true);
  rink_layout->addWidget(rink_help);
  rink_layout->addStretch();
  tabs->addTab(rink_tab, tab_icon("Rink framing"), "Rink framing");
  connect(s.rink_aware, &QCheckBox::toggled, this, [this] { impl_->update_controls(); });
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
  tabs->addTab(legacy, tab_icon("Legacy"), "Legacy");
  right_layout->addWidget(tabs, 1);
  s.trial_prefix = QString::fromStdString(preferences["trial_name_prefix"].as<std::string>());
  s.trial_name = new QLineEdit(s.trial_prefix + " 1");
  s.trial_name->setObjectName("experimentTrialName");
  s.trial_name->setMaxLength(100);
  right_layout->addWidget(s.trial_name);
  s.apply = new QPushButton(action_icon(ActionIcon::Refresh), "Apply && replay");
  s.apply->setObjectName("experimentApply");
  s.apply->setEnabled(false);
  s.apply->setStyleSheet(
      "QPushButton{background:#117866;color:white;padding:10px;font-weight:600;} QPushButton:disabled{background:#a5bdb7;}");
  right_layout->addWidget(s.apply);
  auto* outputs = new QHBoxLayout();
  s.save = new QPushButton(action_icon(ActionIcon::Save), "Save trial…");
  s.save->setObjectName("experimentSave");
  s.save->setEnabled(false);
  s.capture = new QPushButton(action_icon(ActionIcon::Camera), "Screenshot…");
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
  s.prepare = new QPushButton(action_icon(ActionIcon::Prepare), "Prepare historical start");
  s.prepare->setObjectName("experimentPrepare");
  s.cancel = new QPushButton(action_icon(ActionIcon::Cancel), "Cancel preparation");
  s.cancel->setObjectName("experimentCancel");
  s.cancel->setEnabled(false);
  auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close");
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
  for (QDoubleSpinBox* source : {s.in, s.duration, s.playback_start})
    connect(source, &QDoubleSpinBox::valueChanged, this, [this]() { impl_->invalidate_source(); });
  connect(s.video_origin, &QDoubleSpinBox::valueChanged, this, [this]() { impl_->close_preview(); });
  connect(s.uncropped, &QCheckBox::toggled, this, [this](bool checked) {
    if (!checked)
      impl_->close_preview();
    impl_->update_controls();
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
      const bool realtime = state.realtime->isChecked();
      state.preview.Submit([index, realtime](auto& engine, auto* error) {
        engine.SetRealtime(realtime);
        return engine.Seek(index, true, error);
      });
      state.playing = true;
      state.update_controls();
    } else {
      state.open_preview(true, state.current);
    }
  });
  connect(s.stop_preview, &QPushButton::clicked, this, [this]() { impl_->close_preview(); });
  s.video->toggleFocus = [&s]() { s.set_preview_focus(!s.preview_focused); };
  s.video->requestRedraw = [&s]() { s.preview.RequestRedraw(); };
  s.plot->selectFrame = [&s](std::size_t index) {
    if (!s.session || s.source_dirty || s.calculating || s.closing_result)
      return;
    if (s.preview_open)
      s.seek(index);
    else
      s.open_preview(false, index);
  };
  connect(s.focus_button, &QToolButton::clicked, this, s.video->toggleFocus);
  connect(s.save, &QPushButton::clicked, this, [this]() {
    auto& state = *impl_;
    const int index = state.comparison->currentIndex() - 2;
    if (!state.session || state.source_dirty || state.work.valid() || !state.uncropped->isChecked() || index < 0 ||
        static_cast<std::size_t>(index) >= state.trials.size())
      return;
    const QString path =
        QFileDialog::getSaveFileName(this, "Save camera experiment", "camera-trial.yaml", "YAML (*.yaml)");
    if (path.isEmpty())
      return;
    const auto media = state.media_binding();
    if (media.video_origin_pts_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      state.show_status("Playback start plus Video start offset precedes the selected archive.", true);
      return;
    }
    const auto status = state.session->SaveTrial(path.toStdString(), state.trials[index], media);
    state.show_status(
        status.ok() ? (media.path.empty() ? "Trial saved without a video binding."
                                          : "Trial and its source/video binding saved.")
                    : QString::fromStdString(status.ToString()),
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
  qApp->removeEventFilter(this);
  impl_->video->requestRedraw = {};
  if (impl_->cancellation)
    impl_->cancellation->store(true);
  if (impl_->work.valid())
    impl_->work.wait();
  // Fence the renderer while its Qt native child is still alive.
  impl_->preview.Close();
}

bool CameraExperimentDialog::eventFilter(QObject* watched, QEvent* event) {
  return route_value_control_wheel(this, watched, event) || QDialog::eventFilter(watched, event);
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

void CameraExperimentDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && impl_->preview_focused) {
    impl_->set_preview_focus(false);
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
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
