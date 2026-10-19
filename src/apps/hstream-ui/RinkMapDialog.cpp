#include "src/apps/hstream-ui/RinkMapDialog.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QShortcut>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGraphicsEllipseItem>
#include <QtWidgets/QGraphicsItemGroup>
#include <QtWidgets/QGraphicsPixmapItem>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsSceneMouseEvent>
#include <QtWidgets/QGraphicsTextItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

namespace {
namespace rink = hm::stitching;
bool sameInputs(const rink::RinkMapDocument& a, const rink::RinkMapDocument& b) {
  const auto dimensions = [](const rink::RinkLayout& d) {
    return std::make_tuple(d.length, d.width, d.blue_line_distance, d.goal_inset, d.corner_radius);
  };
  if (dimensions(a.layout) != dimensions(b.layout) || a.observations.size() != b.observations.size())
    return false;
  for (size_t i = 0; i < a.observations.size(); ++i) {
    const auto& x = a.observations[i];
    const auto& y = b.observations[i];
    if (std::tie(
            x.feature, x.label, x.camera, x.line, x.automatic, x.enabled, x.confidence, x.pixels, x.mask_revision) !=
        std::tie(y.feature, y.label, y.camera, y.line, y.automatic, y.enabled, y.confidence, y.pixels, y.mask_revision))
      return false;
  }
  return true;
}
QColor landmarkColor(std::string_view label) {
  // Keep a stable identity colour across model contours and reviewed marks.
  static const std::pair<std::string_view, QColor> colors[]{
      {"Center Line", QColor(255, 80, 80)},
      {"Blue Line", QColor(55, 140, 255)},
      {"Goal Line", QColor(255, 151, 60)},
      {"Center Ice Circle", QColor(255, 210, 70)},
      {"Zone Circle", QColor(160, 150, 255)},
      {"Crease", QColor(65, 225, 255)},
      {"Faceoff Dot", QColor(255, 125, 185)},
      {"Goal", QColor(230, 110, 255)},
      {"Slot Box", QColor(170, 230, 65)},
      {"Trapezoid", QColor(60, 225, 165)},
      {"Field", QColor(200, 210, 220)}};
  for (const auto& [name, color] : colors)
    if (label == name)
      return color;
  return QColor(200, 210, 220);
}
QColor observationColor(const rink::RinkMapObservation& observation) {
  if (!observation.enabled)
    return QColor(140, 140, 140);
  const auto& feature = observation.feature;
  if (feature.rfind("center", 0) == 0)
    return landmarkColor("Center Line");
  if (feature.rfind("blue-", 0) == 0)
    return landmarkColor("Blue Line");
  if (feature == "goal-left" || feature == "goal-right")
    return landmarkColor("Goal");
  if (feature.rfind("goal-", 0) == 0)
    return landmarkColor("Goal Line");
  if (feature == "near-boards" || feature == "far-boards")
    return QColor(73, 214, 234);
  return landmarkColor(observation.label);
}
QColor layoutColor(size_t index) {
  // RinkLayoutDrawing: boards, goal, blue, centre, blue, goal lines.
  if (index == 1 || index == 5)
    return landmarkColor("Goal Line");
  if (index == 2 || index == 4)
    return landmarkColor("Blue Line");
  if (index == 3)
    return landmarkColor("Center Line");
  return QColor(73, 214, 234); // Boards and estimated goal-mouth crosses.
}
QString featureName(const rink::RinkMapObservation& observation) {
  for (const auto& feature : rink::RinkMapFeatures())
    if (feature.id == observation.feature)
      return QString::fromUtf8(feature.name);
  return QString::fromStdString(observation.label) + " — choose identity";
}
bool hasGoalLineEnds(const rink::RinkMapObservation& observation) {
  return observation.line && observation.pixels.size() >= 2 &&
      (observation.feature == "goal-left-line" || observation.feature == "goal-right-line");
}
class Anchor : public QGraphicsEllipseItem {
 public:
  Anchor(std::function<void(QPointF)> moved, std::function<void(QPointF)> preview, QGraphicsItem* parent)
      : QGraphicsEllipseItem(-5, -5, 10, 10, parent), moved_(std::move(moved)), preview_(std::move(preview)) {
    setFlags(ItemIsMovable | ItemIgnoresTransformations);
    setBrush(QColor(68, 224, 166));
    setPen(QPen(Qt::black, 1));
    setCursor(Qt::SizeAllCursor);
    setZValue(20);
  }

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
    original_ = pos();
    press_ = event->scenePos();
    event->accept();
  }
  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
    QPointF point = original_ + event->scenePos() - press_;
    const QRectF bounds = scene()->sceneRect().adjusted(0, 0, -1, -1);
    point.setX(std::clamp(point.x(), bounds.left(), bounds.right()));
    point.setY(std::clamp(point.y(), bounds.top(), bounds.bottom()));
    setPos(point);
    preview_(point);
    event->accept();
  }
  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
    event->accept();
    if (pos() != original_)
      moved_(pos());
  }

 private:
  std::function<void(QPointF)> moved_;
  std::function<void(QPointF)> preview_;
  QPointF original_;
  QPointF press_;
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
  void pendingPoint(std::optional<QPointF> point) {
    delete pending_;
    pending_ = nullptr;
    if (point) {
      pending_ = scene()->addEllipse(-6, -6, 12, 12, QPen(Qt::black, 2), QColor(255, 210, 70));
      pending_->setFlag(QGraphicsItem::ItemIgnoresTransformations);
      pending_->setPos(*point);
      pending_->setZValue(10);
      pending_->setAcceptedMouseButtons(Qt::NoButton);
      pending_->setToolTip("First line point — click a second point, or press Escape to cancel");
      pending_->setData(0, "rinkPendingLinePoint");
    }
  }
  void overlays(const RinkMapWorkResult& state, int camera, int selected, bool editing) {
    delete overlays_;
    overlays_ = new QGraphicsItemGroup();
    overlays_->setHandlesChildEvents(false);
    scene()->addItem(overlays_);
    auto path = [&](const std::vector<cv::Point2d>& points,
                    QColor color,
                    const QString& tooltip,
                    qreal width,
                    bool split_long_segments = true) {
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
        if (first || (split_long_segments && QLineF(previous, point).length() > original_.width() / 3.0))
          drawing.moveTo(point);
        else
          drawing.lineTo(point);
        previous = point;
        first = false;
      }
      // A solid black underlay fills the dash gaps. Cosmetic flat-cap strokes
      // retain visible, alternating colour/black segments at every zoom level.
      std::vector<QGraphicsPathItem*> paths;
      for (bool foreground : {false, true}) {
        auto* item = new QGraphicsPathItem(drawing, overlays_);
        QPen pen(foreground ? color : QColor(Qt::black), width);
        pen.setCapStyle(Qt::FlatCap);
        pen.setCosmetic(true);
        if (foreground)
          pen.setDashPattern({4, 4});
        item->setPen(pen);
        item->setToolTip(tooltip);
        item->setAcceptedMouseButtons(Qt::NoButton);
        paths.push_back(item);
      }
      return paths;
    };
    for (size_t i = 0; i < state.projected_layout[camera].size(); ++i)
      path(state.projected_layout[camera][i], layoutColor(i), "Estimated rink geometry", 2);
    for (const auto& guide : state.mask_intersections) {
      if (guide.camera != camera || guide.pixels.empty())
        continue;
      auto* ring = new QGraphicsEllipseItem(-7, -7, 14, 14, overlays_);
      ring->setFlag(QGraphicsItem::ItemIgnoresTransformations);
      ring->setPos(guide.pixels.front().x, guide.pixels.front().y);
      ring->setPen(QPen(observationColor(guide), 2));
      ring->setBrush(Qt::NoBrush);
      ring->setAcceptedMouseButtons(Qt::NoButton);
      ring->setToolTip(featureName(guide) + " — mask intersection guide, not an additional measurement");
      ring->setData(0, "rinkMaskIntersectionGuide");
    }
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
      path(outline, landmarkColor(detection.label), QString::fromStdString(detection.label), 3);
    }
    for (size_t index = 0; index < state.document.observations.size(); ++index) {
      const auto& observation = state.document.observations[index];
      if (observation.camera != camera || observation.pixels.empty())
        continue;
      const QColor color = observationColor(observation);
      const auto observation_paths = path(observation.pixels, color, featureName(observation), 3, false);
      for (auto* item : observation_paths) {
        item->setData(0, "rinkMapObservationPath");
        item->setData(1, int(index));
      }
      auto preview_pixels = std::make_shared<std::vector<cv::Point2d>>(observation.pixels);
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
              [preview_pixels,
               sample,
               observation_paths,
               text,
               automatic = observation.automatic,
               line = observation.line](QPointF point) {
                (*preview_pixels)[sample] = {point.x(), point.y()};
                QPainterPath drawing;
                const auto& first = preview_pixels->front();
                drawing.moveTo(first.x, first.y);
                if (automatic && line) {
                  const auto& last = preview_pixels->back();
                  drawing.lineTo(last.x, last.y);
                } else {
                  for (size_t i = 1; i < preview_pixels->size(); ++i)
                    drawing.lineTo((*preview_pixels)[i].x, (*preview_pixels)[i].y);
                }
                for (auto* item : observation_paths)
                  item->setPath(drawing);
                text->setPos(first.x, first.y);
              },
              overlays_);
          anchor->setData(0, "rinkMapAnchor");
          anchor->setData(1, int(index));
          anchor->setData(2, int(sample));
          anchor->setPos(observation.pixels[sample].x, observation.pixels[sample].y);
          anchor->setEnabled(editing);
          if (int(index) == selected && hasGoalLineEnds(observation) &&
              (sample == 0 || sample + 1 == observation.pixels.size())) {
            auto* label = new QGraphicsTextItem(sample == 0 ? "End 1" : "End 2", anchor);
            label->setPos(8, -26);
            label->setDefaultTextColor(Qt::black);
            label->setAcceptedMouseButtons(Qt::NoButton);
            auto* background = new QGraphicsRectItem(label->boundingRect(), label);
            background->setBrush(Qt::white);
            background->setPen(QPen(Qt::black, 1));
            background->setFlag(QGraphicsItem::ItemStacksBehindParent);
            background->setAcceptedMouseButtons(Qt::NoButton);
          }
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
    const double delta = event->pixelDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y() * 3;
    const double multiplier = std::pow(1.0015, std::clamp(delta, -1200.0, 1200.0));
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
  QGraphicsEllipseItem* pending_{nullptr};
  QPointF press_;
  bool fitted_{true};
  bool anchor_pressed_{false};
};

class RinkMapSchematic : public QWidget {
 public:
  rink::RinkLayout layout;
  bool ready{false};
  explicit RinkMapSchematic(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumHeight(140);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(20, 30, 41));
    if (!rink::ValidateRinkLayout(layout).ok()) {
      painter.setPen(QColor(215, 225, 235));
      painter.drawText(rect(), Qt::AlignCenter, "Enter valid rink dimensions");
      return;
    }
    const double scale = std::min((width() - 28) / layout.length, (height() - 54) / layout.width);
    const QPointF origin(width() / 2.0, height() / 2.0 + 5);
    auto screen = [&](cv::Point2d p) { return origin + QPointF(p.x * scale, -p.y * scale); };
    const auto drawing = rink::RinkLayoutDrawing(layout);
    for (size_t i = 0; i < drawing.size(); ++i) {
      QPainterPath path;
      path.moveTo(screen(drawing[i].front()));
      for (const auto p : drawing[i])
        path.lineTo(screen(p));
      painter.setPen(QPen(layoutColor(i), 2));
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
  setWindowTitle("Rink map and goal centres[*]");
  resize(1380, 920);
  configure_preview_dialog_window(this);
  qApp->installEventFilter(this);
  auto* root = new QVBoxLayout(this);
  auto* toolbar = new QHBoxLayout();
  detect_ = new QPushButton(action_icon(ActionIcon::Inspect), "Find landmarks");
  detect_->setObjectName("detectRinkLandmarksButton");
  stop_ = new QPushButton(action_icon(ActionIcon::Stop), "Stop");
  stop_->setObjectName("stopRinkMapButton");
  toolbar->addWidget(detect_);
  mask_ = new QPushButton(action_icon(ActionIcon::Inspect), "Use rink mask");
  mask_->setObjectName("suggestRinkMaskButton");
  mask_->setToolTip(
      "Suggest the far straight boards from the saved ice mask. Review before checking Use. The near edge may follow the tops of boards and is excluded.");
  toolbar->addWidget(mask_);
  connect(mask_, &QPushButton::clicked, this, [this] { update(false, true); });
  toolbar->addWidget(stop_);
  auto* fit = new QPushButton("Fit images");
  toolbar->addWidget(fit);
  undo_ = new QPushButton(action_icon(ActionIcon::Undo), "Undo");
  undo_->setObjectName("rinkMapUndoButton");
  redo_ = new QPushButton(action_icon(ActionIcon::Next), "Redo");
  redo_->setObjectName("rinkMapRedoButton");
  toolbar->addWidget(undo_);
  toolbar->addWidget(redo_);
  connect(undo_, &QPushButton::clicked, this, [this] { travelHistory(false); });
  connect(redo_, &QPushButton::clicked, this, [this] { travelHistory(true); });
  const auto shortcut = [this](QKeySequence key, QPushButton* button) {
    auto* action = new QShortcut(key, this);
    connect(action, &QShortcut::activated, button, &QPushButton::click);
  };
  shortcut(QKeySequence::Undo, undo_);
  shortcut(QKeySequence::Redo, redo_);
  detect_->setToolTip("Replaces earlier model detections; manual marks are kept. Undo restores the previous result.");
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
      recordEdit();
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
      observation.mask_revision.clear();
      observation.confidence = 1;
      update();
    };
  }
  preview_layout->addWidget(tabs_, 1);
  splitter->addWidget(preview);
  auto* sidebar_scroll = new QScrollArea();
  sidebar_scroll->setWidgetResizable(true);
  sidebar_scroll->setFrameShape(QFrame::NoFrame);
  sidebar_ = sidebar_scroll;
  sidebar_->setMinimumWidth(360);
  auto* sidebar_content = new QWidget();
  sidebar_scroll->setWidget(sidebar_content);
  auto* side = new QVBoxLayout(sidebar_content);
  side->setContentsMargins(0, 0, 0, 0);
  auto* heading = new QLabel("Observed landmarks");
  side->addWidget(heading);
  minimap_readiness_ = new QLabel();
  minimap_readiness_->setObjectName("rinkMapMinimapReadiness");
  framing_readiness_ = new QLabel();
  framing_readiness_->setObjectName("rinkMapFramingReadiness");
  for (auto* label : {minimap_readiness_, framing_readiness_}) {
    label->setWordWrap(true);
    side->addWidget(label);
  }
  guidance_ = new QLabel();
  guidance_->setObjectName("rinkMapGuidanceLabel");
  guidance_->setWordWrap(true);
  guidance_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  side->addWidget(guidance_);
  auto* board_actions = new QHBoxLayout();
  for (int i = 0; i < 2; ++i) {
    auto* button = new QPushButton(action_icon(ActionIcon::Add), i ? "Mark far boards" : "Mark near boards");
    board_shortcuts_[i] = button;
    button->setObjectName(i ? "rinkMapMarkFarBoardsButton" : "rinkMapMarkNearBoardsButton");
    button->setToolTip("Click two separated points on the straight boards at ice level, away from rounded corners.");
    board_actions->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, i] {
      feature_->setCurrentIndex(feature_->findData(i ? "far-boards" : "near-boards"));
    });
  }
  side->addLayout(board_actions);
  table_ = new QTableWidget(0, 3);
  table_->setObjectName("rinkMapObservationsTable");
  table_->setMinimumHeight(140);
  table_->setAlternatingRowColors(true);
  table_->setStyleSheet("QTableWidget { background: white; alternate-background-color: #f0f0f0; color: #161616; }");
  table_->setHorizontalHeaderLabels({"Use", "Identity", "Source"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  side->addWidget(table_, 1);
  remove_ = new QPushButton(action_icon(ActionIcon::Remove), "Remove selected mark");
  remove_->setObjectName("removeRinkMapObservationButton");
  goal_line_ends_ = new QPushButton(action_icon(ActionIcon::Apply), "Use goal-line ends at boards…");
  goal_line_ends_->setObjectName("rinkMapGoalLineEndsButton");
  goal_line_ends_->setToolTip(
      "Select a goal-line row. Reuse its existing endpoints as near/far board intersections after identifying the near end.");
  auto* row_actions = new QHBoxLayout();
  row_actions->addWidget(goal_line_ends_);
  row_actions->addWidget(remove_);
  side->addLayout(row_actions);
  connect(goal_line_ends_, &QPushButton::clicked, this, &RinkMapDialog::useGoalLineEnds);
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
        if (!pending_dimension_edit_)
          pending_dimension_edit_ = captureEdit();
        state_.document.layout = dimensionValues();
        resolved_ = false;
        state_.fit = {};
        state_.projected_layout = {};
        refresh();
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
  edited_ = new QLabel();
  edited_->setObjectName("rinkMapEditedLabel");
  root->addWidget(edited_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  save_ = buttons->addButton("Save partial map", QDialogButtonBox::ActionRole);
  leveling_ = buttons->addButton("Save and use for leveling…", QDialogButtonBox::ActionRole);
  leveling_->setObjectName("rinkMapLevelingButton");
  leveling_->setIcon(action_icon(ActionIcon::Apply));
  leveling_->setToolTip(
      "Save these marks, then review optional leveling from the same source evidence. Partial maps can contain usable rectangle corners.");
  connect(leveling_, &QPushButton::clicked, this, [this] {
    if (save()) {
      leveling_requested_ = true;
      accept();
    }
  });
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
  connect(feature_, &QComboBox::currentIndexChanged, this, [this] { cancelLine(); });
  connect(tabs_, &QTabWidget::currentChanged, this, [this] { cancelLine(); });
  connect(focus_, &QToolButton::clicked, this, [this] {
    const bool focused = !sidebar_->isHidden();
    sidebar_->setVisible(!focused);
    focus_->setFocused(focused);
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    const int row = table_->currentRow();
    if (row >= 0 && row < int(state_.document.observations.size())) {
      recordEdit();
      state_.document.observations.erase(state_.document.observations.begin() + row);
      update();
    }
  });
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    if (!rebuilding_) {
      const int row = table_->currentRow();
      if (row >= 0 && row < int(state_.document.observations.size()))
        tabs_->setCurrentIndex(state_.document.observations[row].camera);
      refresh();
    }
  });
  connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (rebuilding_ || busy_ || item->column() != 0 || item->row() < 0 ||
        size_t(item->row()) >= state_.document.observations.size())
      return;
    recordEdit();
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
  state_.document.layout = dimensionValues();
  clean_document_ = state_.document;
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
  qApp->removeEventFilter(this);
  if (job_)
    job_->cancelled = true;
  if (worker_) {
    worker_->wait();
    delete worker_;
  }
}

bool RinkMapDialog::eventFilter(QObject* watched, QEvent* event) {
  return route_value_control_wheel(this, watched, event) || QDialog::eventFilter(watched, event);
}

rink::RinkLayout RinkMapDialog::dimensionValues() const {
  return {
      dimensions_[0]->value(),
      dimensions_[1]->value(),
      dimensions_[2]->value(),
      dimensions_[3]->value(),
      dimensions_[4]->value()};
}

RinkMapDialog::EditState RinkMapDialog::captureEdit() const {
  auto document = state_.document;
  for (auto& observation : document.observations)
    observation.rays.clear(); // Recomputed from original pixels after restoration.
  return {std::move(document), history_detections_};
}

bool RinkMapDialog::hasUnsavedEdits() const {
  return clean_document_ && !sameInputs(*clean_document_, state_.document);
}

void RinkMapDialog::flushDimensionEdit() {
  if (!pending_dimension_edit_)
    return;
  if (!sameInputs(pending_dimension_edit_->document, state_.document)) {
    undo_history_.push_back(std::move(*pending_dimension_edit_));
    if (undo_history_.size() > 32)
      undo_history_.pop_front();
    redo_history_.clear();
  }
  pending_dimension_edit_.reset();
}

void RinkMapDialog::recordEdit() {
  flushDimensionEdit();
  undo_history_.push_back(captureEdit());
  if (undo_history_.size() > 32)
    undo_history_.pop_front();
  redo_history_.clear();
}

void RinkMapDialog::travelHistory(bool redo) {
  if (busy_ || !snapshot_)
    return;
  flushDimensionEdit();
  auto& from = redo ? redo_history_ : undo_history_;
  auto& to = redo ? undo_history_ : redo_history_;
  if (from.empty())
    return;
  to.push_back(captureEdit());
  auto restored = std::move(from.back());
  from.pop_back();
  state_.document = std::move(restored.document);
  history_detections_ = std::move(restored.detections);
  state_.detections = *history_detections_;
  const auto& layout = state_.document.layout;
  const double values[]{
      layout.length, layout.width, layout.blue_line_distance, layout.goal_inset, layout.corner_radius};
  rebuilding_ = true;
  for (size_t i = 0; i < dimensions_.size(); ++i)
    dimensions_[i]->setValue(values[i]);
  rebuilding_ = false;
  update();
}

void RinkMapDialog::refreshEditControls() {
  undo_->setEnabled(!busy_ && (!undo_history_.empty() || pending_dimension_edit_.has_value()));
  redo_->setEnabled(!busy_ && redo_history_.size() && !pending_dimension_edit_);
  const int selected = table_->currentRow();
  goal_line_ends_->setEnabled(
      snapshot_ && !busy_ && !refresh_timer_.isActive() && state_.document.observations.size() < 128 && selected >= 0 &&
      size_t(selected) < state_.document.observations.size() &&
      hasGoalLineEnds(state_.document.observations[selected]));
  const bool dirty = hasUnsavedEdits();
  setWindowModified(dirty);
  edited_->setText(dirty ? "Unsaved changes — save this map before playback." : "No unsaved changes.");
}

void RinkMapDialog::refreshGuidance() {
  std::set<std::string> identities;
  size_t enabled = 0, unassigned = 0;
  bool cross = false, points = false;
  for (const auto& observation : state_.document.observations) {
    if (!observation.enabled)
      continue;
    ++enabled;
    if (observation.feature.empty()) {
      ++unassigned;
      continue;
    }
    identities.insert(observation.feature);
    points |= !observation.line;
    cross |= observation.line && observation.feature != "near-boards" && observation.feature != "far-boards";
  }
  QString readiness, help, color = "#925b00";
  const auto dimensions = rink::ValidateRinkLayout(dimensionValues());
  if (!snapshot_) {
    readiness = "Unavailable — calibration not loaded";
    help = "Complete NONA stitching calibration, then reopen this dialog. The map needs the calibrated camera views.";
  } else if (!dimensions.ok()) {
    readiness = "Not ready — invalid rink dimensions";
    help = QString::fromStdString(std::string(dimensions.message()));
  } else if (busy_ || refresh_timer_.isActive()) {
    readiness = "Checking current landmarks…";
    help = "Wait for the geometry check. Changes must be checked and saved before playback uses them.";
  } else if (!resolved_) {
    readiness = "Not ready — geometry check incomplete";
    help =
        "The current marks have not passed the geometry check. See the message below, correct the problem and retry.";
  } else if (state_.fit.state == rink::RinkMapState::kReady) {
    readiness = hasUnsavedEdits() ? "Geometry ready — review and save" : "Ready — saved for this calibration";
    color = hasUnsavedEdits() ? "#925b00" : "#237a3b";
    help =
        "Enough landmark data for both uses. You do not need every rink item or visible goals. "
        "Review the outline in both cameras, then Save reviewed map after changes. "
        "Goal estimates depend on the marks and rink dimensions.\n"
        "Enable the mini-map for playback. Program framing is off by default; enable it under Program → Rink framing.";
  } else if (state_.fit.state == rink::RinkMapState::kInconsistent) {
    readiness = "Not ready — marks need correction";
    color = "#b23b32";
    help =
        "The marks cannot form a reliable map with these rink dimensions and camera calibration. "
        "Check left/right identities, endpoints and dimensions; disable a suspect mark or Undo to see its effect. "
        "Adding more marks is not necessarily the fix. You can still save partial evidence.";
  } else {
    readiness = "Not ready — more independent landmarks needed";
    const bool only_cross_lines =
        cross && !points && !identities.count("near-boards") && !identities.count("far-boards");
    const bool goal_line_ends =
        only_cross_lines && identities.count("goal-left-line") && identities.count("goal-right-line");
    if (goal_line_ends) {
      help =
          "Both goal lines are marked. To locate goal centres, identify their board intersections. "
          "If your clicks already meet the boards at ice level, select each goal-line row and choose "
          "Use goal-line ends at boards. This reuses your clicks.";
    } else {
      help = only_cross_lines
          ? "Centre, blue and goal lines are parallel. They do not pin down positions across the rink, so players and play cannot be placed reliably."
          : "The assigned marks do not yet locate positions both along and across the ice. Repeated marks of one feature are not enough.";
    }
    // Suggestions are alternatives, never a second readiness gate. Only the
    // calibrated fit can accept the marks, including combinations not listed.
    const std::vector<std::vector<std::string>> alternatives{
        {"blue-left-line", "blue-right-line", "far-boards", "center"},
        {"goal-left-line", "goal-right-line", "far-boards", "center"},
        {"goal-left-line", "goal-right-line", "near-boards", "far-boards"},
        {"center-line", "blue-left-line", "near-boards", "far-boards"},
        {"center-line", "blue-right-line", "near-boards", "far-boards"},
        {"center-near", "center-far", "blue-left-near", "blue-left-far"},
        {"center-near", "center-far", "blue-right-near", "blue-right-far"},
        {"goal-left-near", "goal-left-far", "goal-right-near", "goal-right-far"}};
    std::vector<std::string> missing;
    for (const auto& alternative : alternatives) {
      std::vector<std::string> candidate;
      for (const auto& feature : alternative)
        if (!identities.count(feature))
          candidate.push_back(feature);
      if (missing.empty() || candidate.size() < missing.size())
        missing = std::move(candidate);
      if (missing.empty())
        break;
    }
    if (missing.empty()) {
      help +=
          "\nThese identities could be sufficient, but their measured positions are too ambiguous. Spread endpoints along the actual marking and check that each identity is correct.";
    } else {
      QStringList names;
      for (const auto& id : missing)
        for (const auto& feature : rink::RinkMapFeatures())
          if (id == feature.id)
            names << feature.name;
      help +=
          (goal_line_ends ? "\nOr add or enable: " : "\nTo complete the map, add or enable: ") + names.join(", ") + ".";
    }
    help += "\nUse ice-level marks, never board tops. Save partial map keeps unfinished work.";
  }
  minimap_readiness_->setText("Playback mini-map: " + readiness);
  framing_readiness_->setText("Program framing: " + readiness);
  for (auto* label : {minimap_readiness_, framing_readiness_})
    label->setStyleSheet("font-weight: bold; color: " + color);
  if (unassigned)
    help += QString("\nAssign the %1 unassigned detection(s) in the list to use them.").arg(unassigned);
  guidance_->setText(QString("%1 enabled marks · %2 distinct identities · %3 unassigned\n%4")
                         .arg(enabled)
                         .arg(identities.size())
                         .arg(unassigned)
                         .arg(help));
}

void RinkMapDialog::cancelLine() {
  first_line_point_.reset();
  for (auto* camera : cameras_)
    if (camera)
      camera->pendingPoint(std::nullopt);
}

void RinkMapDialog::reject() {
  if (close_prompt_active_)
    return;
  if (first_line_point_) {
    cancelLine();
    setStatus("Unfinished line cancelled. Previous marks are unchanged.");
    return;
  }
  for (auto* dimension : dimensions_)
    dimension->interpretText();
  if (busy_) {
    close_after_worker_ = true;
    if (job_)
      job_->cancelled = true;
    setStatus("Stopping rink analysis before closing…");
    return;
  }
  const bool needs_update = refresh_timer_.isActive();
  refresh_timer_.stop();
  flushDimensionEdit();
  if (hasUnsavedEdits()) {
    close_prompt_active_ = true;
    QMessageBox prompt(
        QMessageBox::Question,
        "Discard rink map changes?",
        "Your rink map edits have not been saved. Discard these changes and close the editor?",
        QMessageBox::NoButton,
        this);
    prompt.setObjectName("rinkMapDiscardPrompt");
    auto* discard = prompt.addButton("Discard changes", QMessageBox::DestructiveRole);
    discard->setObjectName("rinkMapDiscardChanges");
    discard->setIcon(action_icon(ActionIcon::Delete));
    auto* keep = prompt.addButton("Keep editing", QMessageBox::RejectRole);
    keep->setObjectName("rinkMapKeepEditing");
    keep->setIcon(action_icon(ActionIcon::Cancel));
    prompt.setDefaultButton(keep);
    prompt.setEscapeButton(keep);
    prompt.exec();
    close_prompt_active_ = false;
    if (prompt.clickedButton() != discard) {
      if (needs_update || !resolved_)
        update();
      return;
    }
  }
  QDialog::reject();
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
  mask_->setEnabled(editable);
  stop_->setEnabled(busy);
  feature_->setEnabled(editable);
  table_->setEnabled(editable);
  remove_->setEnabled(editable);
  for (auto* button : board_shortcuts_)
    button->setEnabled(editable);
  for (auto* dimension : dimensions_)
    dimension->setEnabled(editable);
  save_->setEnabled(
      editable && resolved_ && (snapshot_->saved_map.has_value() || !state_.document.observations.empty()));
  leveling_->setEnabled(editable && resolved_ && !state_.document.observations.empty());
  refreshEditControls();
  refreshGuidance();
}

void RinkMapDialog::update(bool detect, bool use_mask) {
  if (busy_ || !snapshot_)
    return;
  flushDimensionEdit();
  refresh_timer_.stop();
  cancelLine();
  state_.document.layout = dimensionValues();
  auto valid = rink::ValidateRinkLayout(state_.document.layout);
  if ((!detect && !use_mask) || !valid.ok()) {
    resolved_ = false;
    state_.fit = {};
    state_.projected_layout = {};
    state_.mask_intersections.clear();
  }
  if (!valid.ok()) {
    setStatus(QString::fromStdString(std::string(valid.message())), true);
    save_->setEnabled(false);
    table_->setEnabled(false);
    // A combo box may be emitting currentIndexChanged. Rebuild on the next
    // turn so its sender survives the signal, and fence old row indices now.
    QTimer::singleShot(0, this, [this] {
      if (busy_)
        return; // The running update will rebuild from its result.
      rebuildTable();
      refresh();
      setBusy(false);
    });
    return;
  }
  setBusy(true);
  refresh();
  setStatus(
      use_mask     ? "Analysing the saved ice mask; excluding near-board occlusions…"
          : detect ? "Preparing the landmark model (269 MiB on first use), then analysing the saved cameras…"
                   : "Updating calibrated rink geometry…");
  job_ = std::make_shared<Job>();
  const auto job = job_;
  const auto snapshot = *snapshot_;
  const auto document = state_.document;
  const auto before_detection = detect || use_mask ? std::optional<EditState>(captureEdit()) : std::nullopt;
  worker_ = QThread::create([job, snapshot, document, detect, use_mask] {
    job->result = UpdateRinkMap(snapshot, document, detect, [job] { return job->cancelled.load(); }, use_mask);
  });
  connect(worker_, &QThread::finished, this, [this, job, detect, use_mask, before_detection] {
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
      if (!detect) {
        result.detections = std::move(state_.detections);
      }
      if (detect || use_mask) {
        undo_history_.push_back(*before_detection);
        if (undo_history_.size() > 32)
          undo_history_.pop_front();
        redo_history_.clear();
        if (detect)
          history_detections_ = std::make_shared<Detections>(result.detections);
      }
      state_ = std::move(result);
      resolved_ = true;
      setStatus(
          (state_.assistance_message.empty() ? QString() : QString::fromStdString(state_.assistance_message) + "\n") +
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
    if (close_after_worker_) {
      close_after_worker_ = false;
      QTimer::singleShot(0, this, [this] { reject(); });
    }
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
  refreshEditControls();
  refreshGuidance();
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
    identity->setStyleSheet(QString("QComboBox { background: %1; color: #161616; }").arg(i % 2 ? "#f0f0f0" : "white"));
    identity->addItem(QString::fromStdString(observation.label) + " (unassigned)", "");
    for (const auto& feature : rink::RinkMapFeatures())
      if (feature.line == observation.line)
        identity->addItem(feature.name, feature.id);
    identity->setCurrentIndex(std::max(0, identity->findData(QString::fromStdString(observation.feature))));
    table_->setCellWidget(int(i), 1, identity);
    connect(identity, &QComboBox::currentIndexChanged, this, [this, i, identity] {
      if (rebuilding_ || busy_ || i >= state_.document.observations.size())
        return;
      recordEdit();
      state_.document.observations[i].feature = identity->currentData().toString().toStdString();
      update();
    });
    auto* source = new QTableWidgetItem(
        QString(observation.camera ? "R · " : "L · ") +
        (!observation.mask_revision.empty() ? "mask · review"
             : observation.automatic        ? QString("model %1%").arg(qRound(observation.confidence * 100))
                                            : "manual"));
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
  if (state_.document.observations.size() >= 128) {
    setStatus("The map already has 128 marks. Remove an unused mark before adding another.", true);
    return;
  }
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
    cameras_[camera]->pendingPoint(point);
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
  recordEdit();
  cancelLine();
  state_.document.observations.push_back(std::move(observation));
  rebuildTable();
  table_->selectRow(int(state_.document.observations.size()) - 1);
  update();
}

void RinkMapDialog::useGoalLineEnds() {
  const int row = table_->currentRow();
  if (busy_ || refresh_timer_.isActive() || !snapshot_ || close_prompt_active_ || row < 0 ||
      size_t(row) >= state_.document.observations.size() || state_.document.observations.size() >= 128)
    return;
  const auto original = state_.document.observations[row];
  if (!hasGoalLineEnds(original))
    return;
  tabs_->setCurrentIndex(original.camera);
  QMessageBox prompt(
      QMessageBox::Question,
      "Identify the goal-line ends",
      QString(
          "%1: use the two numbered ends already marked in the camera image.\n\n"
          "Both ends must be where this goal line meets the boards at ice level. "
          "Which end meets the near boards (closest to the camera)?\n\n"
          "If either end stops short of the boards or sits on a board top, cancel and adjust it first.")
          .arg(featureName(original)),
      QMessageBox::NoButton,
      this);
  prompt.setObjectName("rinkMapGoalLineEndsPrompt");
  auto* first_near = prompt.addButton("End 1 is near", QMessageBox::AcceptRole);
  auto* last_near = prompt.addButton("End 2 is near", QMessageBox::AcceptRole);
  auto* cancel = prompt.addButton(QMessageBox::Cancel);
  first_near->setObjectName("rinkMapEnd1Near");
  last_near->setObjectName("rinkMapEnd2Near");
  cancel->setObjectName("rinkMapCancelEnds");
  first_near->setIcon(action_icon(ActionIcon::Apply));
  last_near->setIcon(action_icon(ActionIcon::Apply));
  prompt.setDefaultButton(cancel);
  prompt.setEscapeButton(cancel);
  close_prompt_active_ = true;
  prompt.exec();
  close_prompt_active_ = false;
  if (prompt.clickedButton() != first_near && prompt.clickedButton() != last_near)
    return;
  const bool first_is_near = prompt.clickedButton() == first_near;
  const std::string end = original.feature == "goal-left-line" ? "goal-left" : "goal-right";
  auto point = [&](bool near) {
    auto mark = original;
    mark.feature = end + (near ? "-near" : "-far");
    mark.label = featureName(mark).toStdString();
    mark.line = false;
    mark.automatic = false; // Explicitly confirmed points survive future model searches.
    mark.confidence = 1;
    mark.mask_revision.clear();
    mark.rays.clear();
    mark.pixels = {near == first_is_near ? original.pixels.front() : original.pixels.back()};
    return mark;
  };
  recordEdit();
  state_.document.observations[row] = point(true);
  state_.document.observations.insert(state_.document.observations.begin() + row + 1, point(false));
  update();
}

bool RinkMapDialog::save() {
  if (busy_ || !snapshot_ || !resolved_ || refresh_timer_.isActive())
    return false;
  const auto status = SaveRinkMap(*snapshot_, state_.document);
  if (!status.ok()) {
    setStatus(QString::fromStdString(status.ToString()), true);
    return false;
  }
  clean_document_ = state_.document;
  refreshEditControls();
  refreshGuidance();
  if (state_.document.observations.empty()) {
    setStatus("Saved an empty rink map. Previous observations and goal estimates are cleared.");
    return true;
  }
  setStatus(
      state_.fit.state == rink::RinkMapState::kReady
          ? "Saved reviewed rink map and estimated goal centres for this calibration."
          : state_.fit.state == rink::RinkMapState::kInconsistent
          ? "Saved all marks. Review the marked positions, identities and rink dimensions before using this map."
          : "Saved all marks. To locate goal centres and players, add the missing landmarks listed above.");
  return true;
}
