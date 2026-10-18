#include "src/apps/hstream-ui/RinkMapDialog.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGraphicsEllipseItem>
#include <QtWidgets/QGraphicsItemGroup>
#include <QtWidgets/QGraphicsPixmapItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsTextItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"

namespace {
namespace rink = hm::stitching;
QString featureName(const rink::RinkMapObservation& observation) {
  for (const auto& feature : rink::RinkMapFeatures())
    if (feature.id == observation.feature)
      return QString::fromUtf8(feature.name);
  return QString::fromStdString(observation.label) + " — choose identity";
}
class Anchor : public QGraphicsEllipseItem {
 public:
  explicit Anchor(std::function<void(QPointF)> moved, QGraphicsItem* parent)
      : QGraphicsEllipseItem(-5, -5, 10, 10, parent), moved_(std::move(moved)) {
    setFlags(ItemIsMovable | ItemIgnoresTransformations);
    setBrush(QColor(68, 224, 166));
    setPen(QPen(Qt::black, 1));
    setCursor(Qt::SizeAllCursor);
  }

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
    original_ = pos();
    QGraphicsEllipseItem::mousePressEvent(event);
  }
  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
    QGraphicsEllipseItem::mouseReleaseEvent(event);
    if (pos() != original_)
      moved_(pos());
  }

 private:
  std::function<void(QPointF)> moved_;
  QPointF original_;
};
} // namespace

class RinkMapCanvas : public QGraphicsView {
 public:
  explicit RinkMapCanvas(QWidget* parent = nullptr) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setBackgroundBrush(QColor(19, 25, 32));
    setMinimumSize(300, 220);
  }
  void setImage(const QImage& image, QSize original) {
    original_ = original;
    auto* item = scene()->addPixmap(QPixmap::fromImage(image));
    item->setTransform(
        QTransform::fromScale(double(original.width()) / image.width(), double(original.height()) / image.height()));
    scene()->setSceneRect(QRectF(QPointF(), original));
    fit();
  }
  void fit() {
    fitInView(sceneRect(), Qt::KeepAspectRatio);
    fitted_ = true;
  }
  std::function<void(QPointF)> clicked;
  std::function<void(size_t, size_t, QPointF)> moved;
  void overlays(const RinkMapWorkResult& state, int camera, int selected, bool editing) {
    delete overlays_;
    overlays_ = new QGraphicsItemGroup();
    overlays_->setHandlesChildEvents(false);
    scene()->addItem(overlays_);
    auto path = [&](const std::vector<cv::Point2d>& points, QColor color, bool dashed) {
      QPainterPath drawing;
      bool first = true;
      QPointF previous;
      for (const auto p : points) {
        const bool valid = std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 &&
            p.x < original_.width() && p.y < original_.height();
        if (!valid) {
          first = true;
          continue;
        }
        const QPointF point(p.x, p.y);
        if (first || QLineF(previous, point).length() > original_.width() / 3.0)
          drawing.moveTo(point);
        else
          drawing.lineTo(point);
        previous = point;
        first = false;
      }
      auto* item = new QGraphicsPathItem(drawing, overlays_);
      QPen pen(color, 2, dashed ? Qt::DashLine : Qt::SolidLine);
      pen.setCosmetic(true);
      item->setPen(pen);
    };
    for (const auto& line : state.projected_layout[camera])
      path(line, QColor(73, 214, 234), true);
    if (state.projected_layout[camera].size() == 10) {
      for (int end = 0; end < 2; ++end) {
        const auto& cross = state.projected_layout[camera][6 + 2 * end];
        const auto centre = cross[cross.size() / 2];
        if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || !sceneRect().contains(centre.x, centre.y))
          continue;
        auto* text = new QGraphicsTextItem(end ? "Estimated right goal" : "Estimated left goal", overlays_);
        text->setDefaultTextColor(QColor(73, 214, 234));
        text->setFlag(QGraphicsItem::ItemIgnoresTransformations);
        text->setPos(centre.x, centre.y);
        text->setTransform(QTransform::fromTranslate(0, -24));
      }
    }
    for (const auto& detection : state.detections[camera]) {
      auto outline = detection.outline;
      if (!outline.empty())
        outline.push_back(outline.front());
      path(outline, QColor(230, 168, 65, 170), true);
    }
    for (size_t index = 0; index < state.document.observations.size(); ++index) {
      const auto& observation = state.document.observations[index];
      if (observation.camera != camera || observation.pixels.empty())
        continue;
      const QColor color = !observation.enabled ? QColor(140, 140, 140)
          : observation.automatic               ? QColor(255, 195, 78)
                                                : QColor(68, 224, 166);
      path(observation.pixels, color, false);
      const auto position = observation.pixels.front();
      auto* text = new QGraphicsTextItem(QString::number(index + 1) + " " + featureName(observation), overlays_);
      text->setDefaultTextColor(color);
      text->setFlag(QGraphicsItem::ItemIgnoresTransformations);
      text->setPos(position.x, position.y);
      if (int(index) == selected || !observation.automatic) {
        for (size_t sample = 0; sample < observation.pixels.size(); ++sample) {
          if (observation.automatic && sample != 0 && sample + 1 != observation.pixels.size())
            continue;
          auto* anchor = new Anchor(
              [this, index, sample](QPointF position) {
                QTimer::singleShot(0, this, [this, index, sample, position] {
                  if (moved)
                    moved(index, sample, position);
                });
              },
              overlays_);
          anchor->setPos(observation.pixels[sample].x, observation.pixels[sample].y);
          anchor->setEnabled(editing);
        }
      }
    }
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QGraphicsView::resizeEvent(event);
    if (fitted_)
      fit();
  }
  void wheelEvent(QWheelEvent* event) override {
    fitted_ = false;
    const double multiplier = std::pow(1.0015, event->angleDelta().y());
    if (transform().m11() * multiplier > 0.005 && transform().m11() * multiplier < 10)
      scale(multiplier, multiplier);
    event->accept();
  }
  void mousePressEvent(QMouseEvent* event) override {
    press_ = event->position();
    anchor_pressed_ = dynamic_cast<Anchor*>(itemAt(event->position().toPoint())) != nullptr;
    QGraphicsView::mousePressEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    QGraphicsView::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && !anchor_pressed_ && QLineF(press_, event->position()).length() < 4 &&
        clicked) {
      const auto point = mapToScene(event->position().toPoint());
      if (sceneRect().contains(point))
        clicked(point);
    }
  }

 private:
  QSize original_;
  QGraphicsItemGroup* overlays_{nullptr};
  QPointF press_;
  bool fitted_{true};
  bool anchor_pressed_{false};
};

class RinkMapSchematic : public QWidget {
 public:
  rink::RinkLayout layout;
  bool ready{false};
  explicit RinkMapSchematic(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumHeight(170);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(20, 30, 41));
    const double scale = std::min((width() - 28) / layout.length, (height() - 54) / layout.width);
    const QPointF origin(width() / 2.0, height() / 2.0 + 5);
    auto screen = [&](cv::Point2d p) { return origin + QPointF(p.x * scale, -p.y * scale); };
    const auto drawing = rink::RinkLayoutDrawing(layout);
    for (size_t i = 0; i < drawing.size(); ++i) {
      QPainterPath path;
      path.moveTo(screen(drawing[i].front()));
      for (const auto p : drawing[i])
        path.lineTo(screen(p));
      painter.setPen(QPen(
          i == 0                   ? QColor(172, 192, 209)
              : (i == 2 || i == 4) ? QColor(64, 143, 255)
                                   : QColor(230, 92, 103),
          2));
      painter.drawPath(path);
    }
    painter.setPen(QColor(215, 225, 235));
    painter.drawText(
        QRect(8, 4, width() - 16, 24),
        Qt::AlignCenter,
        ready ? "Rink map · estimated from selected dimensions" : "Layout reference · camera map incomplete");
    for (double x : {-layout.length / 2 + layout.goal_inset, layout.length / 2 - layout.goal_inset}) {
      const auto centre = screen({x, 0});
      painter.setPen(
          QPen(ready ? QColor(64, 220, 177) : QColor(125, 135, 145), 2, ready ? Qt::SolidLine : Qt::DashLine));
      painter.drawEllipse(centre, 5, 5);
    }
    painter.setPen(QColor(155, 175, 190));
    painter.drawText(
        QRect(6, height() - 22, width() - 12, 20),
        Qt::AlignCenter,
        QString("%1 × %2 m  ·  near boards below").arg(layout.length, 0, 'f', 2).arg(layout.width, 0, 'f', 2));
  }
};

struct RinkMapDialog::Job {
  std::atomic<bool> cancelled{false};
  std::optional<absl::StatusOr<RinkMapWorkResult>> result;
};

RinkMapDialog::RinkMapDialog(const QString& game_directory, QWidget* parent) : QDialog(parent, Qt::Window) {
  setObjectName("rinkMapDialog");
  setWindowTitle("Rink map and goal centres");
  resize(1380, 920);
  configure_preview_dialog_window(this);
  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  detect_ = new QPushButton(action_icon(ActionIcon::Inspect), "Find landmarks");
  detect_->setObjectName("detectRinkLandmarksButton");
  stop_ = new QPushButton(action_icon(ActionIcon::Stop), "Stop");
  stop_->setObjectName("stopRinkMapButton");
  toolbar->addWidget(detect_);
  toolbar->addWidget(stop_);
  auto* fit = new QPushButton("Fit images");
  toolbar->addWidget(fit);
  toolbar->addStretch();
  toolbar->addWidget(new PreviewDialogWindowSizeButton(this));
  root->addLayout(toolbar);
  auto* instructions = new QLabel(
      "Choose a point or line, then mark it in either camera. Lines need two clicks along the paint or straight boards. "
      "Scroll to zoom, drag to pan, and drag green marks to correct them. Sparse detections can be saved as a partial map.");
  instructions->setWordWrap(true);
  root->addWidget(instructions);
  auto* splitter = new QSplitter(Qt::Horizontal);
  auto* preview = new QWidget();
  auto* preview_layout = new QVBoxLayout(preview);
  preview_layout->setContentsMargins(0, 0, 0, 0);
  auto* preview_header = new QHBoxLayout();
  feature_ = new QComboBox();
  feature_->setObjectName("rinkMapFeatureCombo");
  feature_->addItem("Inspect / pan (no new marks)", "");
  for (const auto& feature : rink::RinkMapFeatures())
    feature_->addItem(QString(feature.line ? "Line: " : "Point: ") + feature.name, feature.id);
  preview_header->addWidget(feature_, 1);
  focus_ = new PreviewFocusButton(preview);
  preview_header->addWidget(focus_);
  preview_layout->addLayout(preview_header);
  tabs_ = new QTabWidget();
  tabs_->setObjectName("rinkMapCameraTabs");
  for (int camera = 0; camera < 2; ++camera) {
    cameras_[camera] = new RinkMapCanvas();
    cameras_[camera]->setObjectName(QString("rinkMapCamera%1").arg(camera));
    tabs_->addTab(cameras_[camera], camera ? "Right camera" : "Left camera");
    cameras_[camera]->clicked = [this, camera](QPointF point) { addPoint(camera, point); };
    cameras_[camera]->moved = [this](size_t index, size_t sample, QPointF point) {
      if (busy_ || index >= state_.document.observations.size())
        return;
      auto& observation = state_.document.observations[index];
      const QSize size = snapshot_->original_sizes[observation.camera];
      point.setX(std::clamp(point.x(), 0.0, double(size.width() - 1)));
      point.setY(std::clamp(point.y(), 0.0, double(size.height() - 1)));
      if (observation.automatic && observation.line) {
        auto first = observation.pixels.front(), last = observation.pixels.back();
        (sample == 0 ? first : last) = cv::Point2d(point.x(), point.y());
        observation.pixels = {first, last};
      } else {
        observation.pixels[sample] = cv::Point2d(point.x(), point.y());
      }
      observation.automatic = false;
      observation.confidence = 1;
      update();
    };
  }
  preview_layout->addWidget(tabs_, 1);
  splitter->addWidget(preview);
  sidebar_ = new QWidget();
  sidebar_->setMinimumWidth(360);
  auto* side = new QVBoxLayout(sidebar_);
  side->setContentsMargins(0, 0, 0, 0);
  auto* heading = new QLabel("Observed landmarks");
  side->addWidget(heading);
  table_ = new QTableWidget(0, 3);
  table_->setObjectName("rinkMapObservationsTable");
  table_->setHorizontalHeaderLabels({"Use", "Identity", "Source"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  side->addWidget(table_, 1);
  remove_ = new QPushButton(action_icon(ActionIcon::Remove), "Remove selected mark");
  side->addWidget(remove_);
  auto* layout_box = new QGroupBox("Rink dimensions (assumptions, metres)");
  auto* form = new QFormLayout(layout_box);
  const char* labels[]{"Length", "Width", "Centre to blue line", "Goal line from end", "Corner radius"};
  const auto& layout = state_.document.layout;
  const double values[]{
      layout.length, layout.width, layout.blue_line_distance, layout.goal_inset, layout.corner_radius};
  for (size_t i = 0; i < dimensions_.size(); ++i) {
    dimensions_[i] = new QDoubleSpinBox();
    dimensions_[i]->setObjectName(QString("rinkMapDimension%1").arg(i));
    dimensions_[i]->setRange(i == 4 ? 0 : 0.1, 80);
    dimensions_[i]->setDecimals(3);
    dimensions_[i]->setValue(values[i]);
    form->addRow(labels[i], dimensions_[i]);
    connect(dimensions_[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
      if (!rebuilding_) {
        resolved_ = false;
        save_->setEnabled(false);
        refresh_timer_.start();
      }
    });
  }
  side->addWidget(layout_box);
  schematic_ = new RinkMapSchematic();
  schematic_->setObjectName("rinkMapSchematic");
  side->addWidget(schematic_);
  splitter->addWidget(sidebar_);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);
  root->addWidget(splitter, 1);
  status_ = new QLabel();
  status_->setObjectName("rinkMapStatus");
  status_->setWordWrap(true);
  status_->setMinimumHeight(42);
  root->addWidget(status_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  save_ = buttons->addButton("Save partial map", QDialogButtonBox::ActionRole);
  save_->setObjectName("saveRinkMapButton");
  save_->setIcon(action_icon(ActionIcon::Save));
  root->addWidget(buttons);
  refresh_timer_.setSingleShot(true);
  refresh_timer_.setInterval(250);
  connect(&refresh_timer_, &QTimer::timeout, this, [this] { update(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(save_, &QPushButton::clicked, this, [this] { save(); });
  connect(detect_, &QPushButton::clicked, this, [this] { update(true); });
  connect(stop_, &QPushButton::clicked, this, [this] {
    if (job_)
      job_->cancelled = true;
    restorePreview();
    setStatus("Stopping rink analysis…");
  });
  connect(fit, &QPushButton::clicked, this, [this] {
    for (auto* camera : cameras_)
      camera->fit();
  });
  connect(feature_, &QComboBox::currentIndexChanged, this, [this] { first_line_point_.reset(); });
  connect(focus_, &QToolButton::clicked, this, [this] {
    const bool focused = !sidebar_->isHidden();
    sidebar_->setVisible(!focused);
    focus_->setFocused(focused);
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    const int row = table_->currentRow();
    if (row >= 0 && row < int(state_.document.observations.size())) {
      state_.document.observations.erase(state_.document.observations.begin() + row);
      update();
    }
  });
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    if (!rebuilding_)
      refresh();
  });
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (rebuilding_ || item->column() != 0)
      return;
    state_.document.observations[item->row()].enabled = item->checkState() == Qt::Checked;
    update();
  });
  auto snapshot = LoadRinkMapSnapshot(game_directory);
  if (!snapshot.ok()) {
    setStatus(QString::fromStdString(snapshot.status().ToString()), true);
    setBusy(false);
    return;
  }
  snapshot_ = std::move(*snapshot);
  state_.document.generation = snapshot_->generation;
  state_.document.calibration_revision = snapshot_->revision.toStdString();
  for (int camera = 0; camera < 2; ++camera)
    cameras_[camera]->setImage(snapshot_->images[camera], snapshot_->original_sizes[camera]);
  const auto& saved = snapshot_->saved_map;
  QString load_notice;
  if (saved) {
    auto document = rink::ParseRinkMap(*saved);
    if (!document.ok()) {
      load_notice = QString::fromStdString(document.status().ToString());
    } else if (
        document->generation != snapshot_->generation ||
        document->calibration_revision != snapshot_->revision.toStdString()) {
      load_notice = "The saved map belongs to an older calibration. New marks will be saved against this calibration.";
    } else {
      state_.document = *document;
      const auto& dims = document->layout;
      const double restored[]{dims.length, dims.width, dims.blue_line_distance, dims.goal_inset, dims.corner_radius};
      rebuilding_ = true;
      for (size_t i = 0; i < dimensions_.size(); ++i)
        dimensions_[i]->setValue(restored[i]);
      rebuilding_ = false;
    }
  }
  setBusy(false);
  update();
  if (!load_notice.isEmpty()) {
    auto* notice = new QLabel(load_notice);
    notice->setWordWrap(true);
    root->insertWidget(2, notice);
  }
  QTimer::singleShot(0, this, [this] {
    for (auto* camera : cameras_)
      camera->fit();
  });
}

RinkMapDialog::~RinkMapDialog() {
  if (job_)
    job_->cancelled = true;
  if (worker_) {
    worker_->wait();
    delete worker_;
  }
}

void RinkMapDialog::setStatus(const QString& message, bool error) {
  status_->setText(message);
  status_->setStyleSheet(error ? "color: #d56565" : "");
}

void RinkMapDialog::restorePreview() {
  sidebar_->show();
  focus_->setFocused(false);
}

void RinkMapDialog::setBusy(bool busy) {
  busy_ = busy;
  const bool editable = snapshot_.has_value() && !busy;
  detect_->setEnabled(editable);
  stop_->setEnabled(busy);
  feature_->setEnabled(editable);
  table_->setEnabled(editable);
  remove_->setEnabled(editable);
  for (auto* dimension : dimensions_)
    dimension->setEnabled(editable);
  save_->setEnabled(
      editable && resolved_ && !state_.document.observations.empty() &&
      state_.fit.state != rink::RinkMapState::kInconsistent);
}

void RinkMapDialog::update(bool detect) {
  if (busy_ || !snapshot_)
    return;
  refresh_timer_.stop();
  first_line_point_.reset();
  state_.document.layout = {
      dimensions_[0]->value(),
      dimensions_[1]->value(),
      dimensions_[2]->value(),
      dimensions_[3]->value(),
      dimensions_[4]->value()};
  auto valid = rink::ValidateRinkLayout(state_.document.layout);
  if (!valid.ok()) {
    setStatus(QString::fromStdString(std::string(valid.message())), true);
    save_->setEnabled(false);
    return;
  }
  if (!detect) {
    resolved_ = false;
    state_.fit = {};
    state_.projected_layout = {};
  }
  setBusy(true);
  refresh();
  setStatus(
      detect ? "Preparing the landmark model (269 MiB on first use), then analysing the saved cameras…"
             : "Updating calibrated rink geometry…");
  job_ = std::make_shared<Job>();
  const auto job = job_;
  const auto snapshot = *snapshot_;
  const auto document = state_.document;
  worker_ = QThread::create([job, snapshot, document, detect] {
    job->result = UpdateRinkMap(snapshot, document, detect, [job] { return job->cancelled.load(); });
  });
  connect(worker_, &QThread::finished, this, [this, job, detect] {
    auto* completed = worker_;
    worker_ = nullptr;
    completed->deleteLater();
    if (job->cancelled) {
      setStatus("Stopped. Previous marks are retained.");
    } else if (!job->result || !job->result->ok()) {
      setStatus(
          job->result ? QString::fromStdString(job->result->status().ToString()) : "Rink analysis did not complete",
          true);
    } else {
      auto result = std::move(**job->result);
      if (!detect)
        result.detections = std::move(state_.detections);
      state_ = std::move(result);
      resolved_ = true;
      setStatus(
          QString::fromStdString(state_.fit.message) +
          (state_.fit.state == rink::RinkMapState::kReady
               ? QString("  Residual to %1 marks: %2° (not a position accuracy).")
                     .arg(state_.fit.constraints)
                     .arg(state_.fit.rms_degrees, 0, 'f', 3)
               : "") +
          (detect
               ? " Assign each blue/goal line to its rink end in the list; unassigned detections do not constrain the map."
               : ""));
    }
    job_.reset();
    setBusy(false);
    rebuildTable();
    refresh();
  });
  worker_->start();
}

void RinkMapDialog::refresh() {
  for (int camera = 0; camera < 2; ++camera)
    cameras_[camera]->overlays(state_, camera, table_->currentRow(), !busy_);
  schematic_->layout = state_.document.layout;
  schematic_->ready = state_.fit.state == rink::RinkMapState::kReady;
  schematic_->update();
  save_->setText(state_.fit.state == rink::RinkMapState::kReady ? "Save reviewed map" : "Save partial map");
}

void RinkMapDialog::rebuildTable() {
  rebuilding_ = true;
  const int selected = table_->currentRow();
  table_->setRowCount(int(state_.document.observations.size()));
  for (size_t i = 0; i < state_.document.observations.size(); ++i) {
    const auto& observation = state_.document.observations[i];
    auto* enabled = new QTableWidgetItem();
    enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    enabled->setCheckState(observation.enabled ? Qt::Checked : Qt::Unchecked);
    table_->setItem(int(i), 0, enabled);
    auto* identity = new QComboBox();
    identity->addItem(QString::fromStdString(observation.label) + " (unassigned)", "");
    for (const auto& feature : rink::RinkMapFeatures())
      if (feature.line == observation.line)
        identity->addItem(feature.name, feature.id);
    identity->setCurrentIndex(std::max(0, identity->findData(QString::fromStdString(observation.feature))));
    table_->setCellWidget(int(i), 1, identity);
    connect(identity, &QComboBox::currentIndexChanged, this, [this, i, identity] {
      if (rebuilding_ || busy_)
        return;
      state_.document.observations[i].feature = identity->currentData().toString().toStdString();
      update();
    });
    auto* source = new QTableWidgetItem(
        QString(observation.camera ? "R · " : "L · ") +
        (observation.automatic ? QString("model %1%").arg(qRound(observation.confidence * 100)) : "manual"));
    source->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    table_->setItem(int(i), 2, source);
  }
  if (selected >= 0 && selected < table_->rowCount())
    table_->selectRow(selected);
  rebuilding_ = false;
}

void RinkMapDialog::addPoint(int camera, QPointF point) {
  if (busy_ || !snapshot_)
    return;
  const auto id = feature_->currentData().toString().toStdString();
  const auto found =
      std::find_if(rink::RinkMapFeatures().begin(), rink::RinkMapFeatures().end(), [&](const auto& feature) {
        return id == feature.id;
      });
  if (found == rink::RinkMapFeatures().end())
    return;
  if (found->line && (!first_line_point_ || first_line_camera_ != camera)) {
    first_line_point_ = point;
    first_line_camera_ = camera;
    setStatus("First line mark selected. Click a second point farther along the same marking.");
    return;
  }
  rink::RinkMapObservation observation;
  observation.feature = id;
  observation.label = found->name;
  observation.camera = camera;
  observation.line = found->line;
  if (found->line)
    observation.pixels.emplace_back(first_line_point_->x(), first_line_point_->y());
  observation.pixels.emplace_back(point.x(), point.y());
  first_line_point_.reset();
  state_.document.observations.push_back(std::move(observation));
  update();
}

void RinkMapDialog::save() {
  if (busy_ || !snapshot_ || !resolved_ || refresh_timer_.isActive())
    return;
  const auto status = SaveRinkMap(*snapshot_, state_.document);
  if (!status.ok()) {
    setStatus(QString::fromStdString(status.ToString()), true);
    return;
  }
  setStatus(
      state_.fit.state == rink::RinkMapState::kReady
          ? "Saved reviewed rink map and estimated goal centres for this calibration."
          : "Saved partial observations. Goal locations remain unavailable until the map is constrained.");
}
