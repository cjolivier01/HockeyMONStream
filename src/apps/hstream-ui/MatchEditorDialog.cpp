#include "src/apps/hstream-ui/MatchEditorDialog.h"

#include "src/apps/hstream-ui/ActionIcons.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include <QtCore/QFileInfo>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSignalBlocker>
#include <QtGui/QImageReader>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtGui/QShortcut>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGraphicsPixmapItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

namespace {
using hm::stitching::CalibrationMatchSet;
using hm::stitching::FeatureMatch;
using Matches = std::vector<FeatureMatch>;

QPointF endpoint(const FeatureMatch& match, int camera) {
  const auto& p = camera ? match.right : match.left;
  return {p.x, p.y};
}

// These are retained offline stills, not video surfaces. Only the current pair
// is decoded. Retain native pixels for accurate editing, with explicit bounds
// on encoded bytes and decoded dimensions; Qt also enforces its allocation cap.
constexpr qint64 kMaximumImageBytes = 512LL * 1024 * 1024;
constexpr qint64 kMaximumImagePixels = 64LL * 1024 * 1024;

class MatchView : public QGraphicsView {
 public:
  explicit MatchView(QWidget* parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    image_ = scene()->addPixmap({});
    image_->setOffset(-0.5, -0.5); // Integer coordinates denote camera pixel centers.
    setBackgroundBrush(QColor("#141820"));
    setMinimumSize(180, 180);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setDragMode(ScrollHandDrag);
    setTransformationAnchor(NoAnchor);
    setResizeAnchor(AnchorViewCenter);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setToolTip("Select a marker in either camera; drag it to adjust. Drag empty space to pan. Wheel to zoom.");
  }

  QString load(const std::filesystem::path& path, cv::Size expected) {
    image_->setPixmap({});
    ready_ = false;
    const QString filename = QString::fromStdString(path.string());
    QFileInfo info(filename);
    if (!info.isFile() || info.isSymLink() || info.size() > kMaximumImageBytes)
      return "Camera image is missing or exceeds the editor's file limit.";
    QImageReader reader(filename, "png");
    const QSize size = reader.size();
    if (size != QSize(expected.width, expected.height) || size.width() <= 0 || size.height() <= 0 ||
        size.width() > 32768 || size.height() > 32768 || qint64(size.width()) * size.height() > kMaximumImagePixels)
      return "Camera image dimensions differ from the retained match set or exceed the editor's limit.";
    QImage decoded = reader.read();
    if (decoded.isNull())
      return "Cannot decode camera image: " + reader.errorString();
    // QImageReader supports RGB/RGBA 16-bit PNG. Conversion is only for display;
    // saved images and floating-point match coordinates are never resampled.
    image_->setPixmap(QPixmap::fromImage(decoded.convertToFormat(QImage::Format_RGB32)));
    ready_ = !image_->pixmap().isNull();
    fit();
    return ready_ ? QString() : QString("Cannot allocate camera image for display.");
  }

  void points(const Matches* matches, int camera, int selected, std::optional<QPointF> pending = {}) {
    matches_ = matches;
    camera_ = camera;
    selected_ = selected;
    pending_ = pending;
    viewport()->update();
  }
  void clearImage() {
    image_->setPixmap({});
    ready_ = false;
  }
  bool ready() const {
    return ready_;
  }
  void fit() {
    fitted_ = true;
    if (!ready_)
      return;
    setSceneRect(image_->boundingRect());
    fitInView(image_, Qt::KeepAspectRatio);
  }
  void actualSize() {
    if (!ready_)
      return;
    fitted_ = false;
    const QPointF center = mapToScene(viewport()->rect().center());
    resetTransform();
    scale(1.0 / devicePixelRatioF(), 1.0 / devicePixelRatioF());
    margins();
    centerOn(center);
  }
  void zoom(double amount) {
    zoomAt(amount, viewport()->rect().center());
  }
  void reveal(QPointF point) {
    if (ready_)
      centerOn(point);
  }
  std::function<void(int)> select;
  std::function<void()> begin_drag;
  std::function<void(QPointF)> move_point;
  std::function<void()> finish_drag;
  std::function<void(QPointF)> add_point;
  bool adding{false};

 protected:
  void drawForeground(QPainter* painter, const QRectF&) override {
    if (!ready_)
      return;
    painter->save();
    // Draw fixed-size markers in viewport coordinates so zooming retains a
    // precise, unobscured image location and an easy-to-hit marker.
    painter->resetTransform();
    if (matches_) {
      for (size_t i = 0; i < matches_->size(); ++i) {
        const bool selected = int(i) == selected_;
        const QPoint point = mapFromScene(endpoint((*matches_)[i], camera_));
        if (!viewport()->rect().adjusted(-15, -15, 15, 15).contains(point))
          continue;
        const QColor color = selected ? QColor("#fff176") : QColor::fromHsv((i * 137) % 360, 220, 255);
        painter->setPen(QPen(Qt::black, selected ? 4 : 3));
        drawCross(painter, point, selected ? 8 : 5);
        painter->setPen(QPen(color, selected ? 2 : 1));
        drawCross(painter, point, selected ? 8 : 5);
        if (selected) {
          painter->drawEllipse(point, 10, 10);
          painter->drawText(point + QPoint(12, -12), QString::number(i + 1));
        }
      }
    }
    if (pending_) {
      painter->setPen(QPen(QColor("#fff176"), 2));
      drawCross(painter, mapFromScene(*pending_), 10);
    }
    painter->restore();
  }
  void resizeEvent(QResizeEvent* event) override {
    QGraphicsView::resizeEvent(event);
    if (fitted_)
      fit();
    else if (ready_)
      margins();
  }
  void wheelEvent(QWheelEvent* event) override {
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->pixelDelta().y();
    if (!ready_ || !delta) {
      event->ignore();
      return;
    }
    zoomAt(std::pow(1.25, std::clamp(delta / 120.0, -4.0, 4.0)), event->position().toPoint());
    event->accept();
  }
  void mousePressEvent(QMouseEvent* event) override {
    if (ready_ && event->button() == Qt::LeftButton) {
      const QPoint pos = event->position().toPoint();
      const QPointF scene_point = mapToScene(pos);
      if (adding) {
        if (scene_point.x() >= 0 && scene_point.y() >= 0 && scene_point.x() < image_->pixmap().width() &&
            scene_point.y() < image_->pixmap().height() && add_point)
          add_point(scene_point);
        event->accept();
        return;
      }
      int hit = -1;
      double distance = 12.0;
      if (matches_)
        for (size_t i = 0; i < matches_->size(); ++i) {
          const double candidate = QLineF(pos, mapFromScene(endpoint((*matches_)[i], camera_))).length();
          if (candidate <= distance) {
            distance = candidate;
            hit = int(i);
          }
        }
      if (hit >= 0) {
        drag_ = true;
        moved_ = false;
        press_ = pos;
        offset_ = endpoint((*matches_)[hit], camera_) - scene_point;
        if (select)
          select(hit);
        event->accept();
        return;
      }
    }
    QGraphicsView::mousePressEvent(event);
  }
  void mouseMoveEvent(QMouseEvent* event) override {
    if (drag_) {
      if (!moved_ && (event->position().toPoint() - press_).manhattanLength() >= 3) {
        moved_ = true;
        if (begin_drag)
          begin_drag();
      }
      if (moved_ && move_point)
        move_point(mapToScene(event->position().toPoint()) + offset_);
      event->accept();
      return;
    }
    QGraphicsView::mouseMoveEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (drag_ && event->button() == Qt::LeftButton) {
      drag_ = false;
      if (moved_ && finish_drag)
        finish_drag();
      event->accept();
      return;
    }
    QGraphicsView::mouseReleaseEvent(event);
  }
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && !adding) {
      fit();
      event->accept();
    } else
      QGraphicsView::mouseDoubleClickEvent(event);
  }

 private:
  static void drawCross(QPainter* p, QPoint point, int radius) {
    p->drawLine(point + QPoint(-radius, 0), point + QPoint(radius, 0));
    p->drawLine(point + QPoint(0, -radius), point + QPoint(0, radius));
  }
  void margins() {
    const double x = viewport()->width() / transform().m11();
    const double y = viewport()->height() / transform().m22();
    setSceneRect(image_->boundingRect().adjusted(-x, -y, x, y));
  }
  void zoomAt(double factor, QPoint position) {
    if (!ready_)
      return;
    fitted_ = false;
    const QPointF before = mapToScene(position);
    const double current = transform().m11();
    const double next = std::clamp(current * factor, 0.005, 64.0);
    scale(next / current, next / current);
    margins();
    const QPointF after = mapToScene(position);
    translate(after.x() - before.x(), after.y() - before.y());
  }
  QGraphicsPixmapItem* image_{};
  const Matches* matches_{};
  int camera_{0}, selected_{-1};
  bool ready_{false}, fitted_{true}, drag_{false}, moved_{false};
  QPoint press_;
  QPointF offset_;
  std::optional<QPointF> pending_;
};
} // namespace

struct MatchEditorDialog::Impl {
  MatchEditorDialog* dialog;
  CalibrationMatchSet set, automatic;
  std::vector<Matches> initial_matches;
  bool close_prompt_active{false};
  bool saving{false}, invoking_save{false};
  std::function<void()> save_handler;
  QComboBox* pairs{};
  QComboBox* selection{};
  std::array<MatchView*, 2> views{};
  std::array<QDoubleSpinBox*, 4> coordinates{};
  QPushButton *add{}, *remove{}, *undo{}, *redo{}, *save{}, *maximize{}, *reset{};
  QLabel *status{}, *identity{};
  int pair{0}, selected{-1};
  bool adding{false}, automatic_compatible{false};
  std::array<std::optional<QPointF>, 2> pending;
  QString image_error;
  struct Change {
    int pair;
    Matches before, after;
    int before_selection, after_selection;
  };
  struct Operation {
    std::vector<Change> changes;
  };
  std::vector<Operation> history;
  size_t cursor{0};
  std::optional<Matches> drag_before;

  Impl(MatchEditorDialog* owner, CalibrationMatchSet input, CalibrationMatchSet original)
      : dialog(owner), set(std::move(input)), automatic(std::move(original)) {
    initial_matches.reserve(set.frames.size());
    for (const auto& frame : set.frames)
      initial_matches.push_back(frame.matches);
  }
  Matches& matches() {
    return set.frames[pair].matches;
  }
  bool validPair() const {
    return pair >= 0 && pair < int(set.frames.size());
  }
  bool editable() const {
    return !saving && validPair() && views[0]->ready() && views[1]->ready();
  }
  static bool equal(const Matches& a, const Matches& b) {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i].left != b[i].left || a[i].right != b[i].right || a[i].score != b[i].score ||
          a[i].left_index != b[i].left_index || a[i].right_index != b[i].right_index)
        return false;
    return true;
  }
  bool changed() const {
    if (set.frames.size() != initial_matches.size())
      return true;
    for (size_t i = 0; i < initial_matches.size(); ++i)
      if (!equal(set.frames[i].matches, initial_matches[i]))
        return true;
    return false;
  }
  size_t totalMatches() const {
    size_t total = 0;
    for (const auto& frame : set.frames)
      total += frame.matches.size();
    return total;
  }
  void record(Operation operation) {
    operation.changes.erase(
        std::remove_if(
            operation.changes.begin(),
            operation.changes.end(),
            [](const Change& c) { return equal(c.before, c.after); }),
        operation.changes.end());
    if (operation.changes.empty()) {
      refresh();
      return;
    }
    history.resize(cursor);
    history.push_back(std::move(operation));
    // Bound history memory independently of the number of input frame pairs.
    size_t points = 0;
    for (const auto& op : history)
      for (const auto& change : op.changes)
        points += change.before.size() + change.after.size();
    while (history.size() > 1 && (history.size() > 100 || points > 1000000)) {
      for (const auto& change : history.front().changes)
        points -= change.before.size() + change.after.size();
      history.erase(history.begin());
    }
    cursor = history.size();
    refresh();
  }
  void cancelAdd() {
    adding = false;
    pending = {};
    for (auto* view : views)
      view->adding = false;
    refresh();
  }
  void refresh() {
    const bool active = editable();
    const bool have_selection = validPair() && selected >= 0 && selected < int(matches().size());
    {
      QSignalBlocker block(selection);
      const int count = validPair() ? int(matches().size()) : 0;
      // Drag updates must not rebuild thousands of combo-box rows.
      if (selection->count() != count) {
        selection->clear();
        for (int i = 0; i < count; ++i)
          selection->addItem(QString("Match %1").arg(i + 1));
      }
      selection->setCurrentIndex(have_selection ? selected : -1);
    }
    selection->setEnabled(active && !adding);
    add->setText(adding ? "Cancel adding" : "Add match");
    add->setIcon(action_icon(adding ? ActionIcon::Cancel : ActionIcon::Add));
    add->setEnabled(active && (adding || matches().size() < hm::stitching::kMaximumEditableMatchesPerPair));
    remove->setEnabled(active && have_selection && !adding);
    undo->setEnabled(cursor > 0 && !adding);
    redo->setEnabled(cursor < history.size() && !adding);
    reset->setEnabled(automatic_compatible && !adding);
    save->setEnabled(active && !adding);
    for (int camera = 0; camera < 2; ++camera) {
      views[camera]->points(validPair() ? &matches() : nullptr, camera, selected, pending[camera]);
      for (int axis = 0; axis < 2; ++axis) {
        auto* field = coordinates[camera * 2 + axis];
        QSignalBlocker block(field);
        field->setEnabled(active && have_selection && !adding);
        if (validPair()) {
          const auto size = set.frames[pair].sizes[camera];
          field->setRange(0, std::nextafter(float(axis ? size.height : size.width), 0.0f));
          const auto p = have_selection ? endpoint(matches()[selected], camera) : QPointF();
          field->setValue(axis ? p.y() : p.x());
        }
      }
    }
    if (saving)
      status->setText("Saving edited matches…");
    else if (!image_error.isEmpty())
      status->setText(image_error);
    else if (adding)
      status->setText(
          pending[0]       ? "Click the corresponding point in the right camera."
              : pending[1] ? "Click the corresponding point in the left camera."
                           : "Click a point in each camera to add a match. Escape cancels.");
    else
      status->setText(
          validPair()
              ? QString(
                    "%1 matches in this pair; %2 total across %3 pairs. Select a marker in either image; drag to adjust or enter exact pixel coordinates below.")
                    .arg(matches().size())
                    .arg(totalMatches())
                    .arg(set.frames.size())
              : "No retained frame pairs are available.");
  }
  void loadPair(int index) {
    adding = false;
    pending = {};
    drag_before.reset();
    for (auto* view : views) {
      view->adding = false;
      view->clearImage();
    }
    pair = index;
    selected = -1;
    image_error.clear();
    if (validPair()) {
      const auto& frame = set.frames[pair];
      for (int camera = 0; camera < 2; ++camera) {
        const QString error = views[camera]->load(frame.images[camera], frame.sizes[camera]);
        if (!error.isEmpty())
          image_error += QString("%1: %2 ").arg(camera ? "Right" : "Left", error);
      }
      identity->setText(QString("Left: %1 s  •  Right: %2 s  •  Coordinates are original camera pixels.")
                            .arg(frame.source_seconds[0], 0, 'f', 6)
                            .arg(frame.source_seconds[1], 0, 'f', 6));
      selected = matches().empty() ? -1 : 0;
    }
    refresh();
  }
  void select(int index) {
    if (!editable() || adding)
      return;
    selected = index;
    refresh();
  }
  void move(int camera, QPointF p) {
    if (!editable() || selected < 0 || selected >= int(matches().size()))
      return;
    const auto size = set.frames[pair].sizes[camera];
    auto& value = camera ? matches()[selected].right : matches()[selected].left;
    value = {
        std::clamp(float(p.x()), 0.0f, std::nextafter(float(size.width), 0.0f)),
        std::clamp(float(p.y()), 0.0f, std::nextafter(float(size.height), 0.0f))};
    refresh();
  }
  void addPoint(int camera, QPointF point) {
    if (!adding || !editable())
      return;
    const auto size = set.frames[pair].sizes[camera];
    pending[camera] = QPointF(
        std::clamp(float(point.x()), 0.0f, std::nextafter(float(size.width), 0.0f)),
        std::clamp(float(point.y()), 0.0f, std::nextafter(float(size.height), 0.0f)));
    if (pending[0] && pending[1]) {
      const Matches before = matches();
      const int previous = selected;
      FeatureMatch match;
      match.left = {float(pending[0]->x()), float(pending[0]->y())};
      match.right = {float(pending[1]->x()), float(pending[1]->y())};
      match.score = 1.0f;
      matches().push_back(match);
      selected = int(matches().size()) - 1;
      adding = false;
      pending = {};
      for (auto* view : views)
        view->adding = false;
      record({{{pair, before, matches(), previous, selected}}});
    } else
      refresh();
  }
  void travel(bool forward) {
    if (adding || (forward ? cursor == history.size() : cursor == 0))
      return;
    const auto& operation = history[forward ? cursor++ : --cursor];
    int selection_after = selected;
    for (const auto& change : operation.changes) {
      set.frames[change.pair].matches = forward ? change.after : change.before;
      if (change.pair == pair)
        selection_after = forward ? change.after_selection : change.before_selection;
    }
    if (operation.changes.size() == 1 && operation.changes[0].pair != pair) {
      const auto& change = operation.changes[0];
      pairs->setCurrentIndex(change.pair);
      selection_after = forward ? change.after_selection : change.before_selection;
    }
    selected = selection_after;
    refresh();
  }
  void resetAutomatic() {
    if (!automatic_compatible)
      return;
    Operation operation;
    for (size_t i = 0; i < set.frames.size(); ++i) {
      operation.changes.push_back(
          {int(i),
           set.frames[i].matches,
           automatic.frames[i].matches,
           i == size_t(pair) ? selected : -1,
           automatic.frames[i].matches.empty() ? -1 : 0});
      set.frames[i].matches = automatic.frames[i].matches;
    }
    selected = matches().empty() ? -1 : 0;
    record(std::move(operation));
  }
};

MatchEditorDialog::MatchEditorDialog(CalibrationMatchSet matches, CalibrationMatchSet automatic, QWidget* parent)
    : QDialog(parent, Qt::Window), impl_(std::make_unique<Impl>(this, std::move(matches), std::move(automatic))) {
  setObjectName("matchEditorDialog");
  setWindowTitle("Modify calibration matches");
  setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setSizeGripEnabled(true);
  if (screen())
    resize(screen()->availableGeometry().size() * 0.9);
  auto& s = *impl_;
  auto* root = new QVBoxLayout(this);
  auto* top = new QHBoxLayout();
  top->addWidget(new QLabel("Frame pair"));
  s.pairs = new QComboBox();
  s.pairs->setObjectName("matchEditorPair");
  for (size_t i = 0; i < s.set.frames.size(); ++i)
    s.pairs->addItem(QString("Pair %1").arg(i + 1));
  top->addWidget(s.pairs);
  s.identity = new QLabel();
  s.identity->setWordWrap(true);
  top->addWidget(s.identity, 1);
  s.maximize = new QPushButton();
  s.maximize->setObjectName("matchEditorMaximize");
  top->addWidget(s.maximize);
  root->addLayout(top);
  auto* split = new QSplitter();
  root->addWidget(split, 1);
  for (int camera = 0; camera < 2; ++camera) {
    auto* group = new QGroupBox(camera ? "Right camera" : "Left camera");
    auto* layout = new QVBoxLayout(group);
    auto* view = new MatchView(group);
    s.views[camera] = view;
    view->setObjectName(camera ? "matchEditorRight" : "matchEditorLeft");
    layout->addWidget(view, 1);
    auto* nav = new QHBoxLayout();
    const auto button = [&](const QString& text, ActionIcon icon, std::function<void()> action) {
      auto* control = new QPushButton(action_icon(icon), text);
      control->setAutoDefault(false);
      connect(control, &QPushButton::clicked, this, std::move(action));
      nav->addWidget(control);
    };
    button("Zoom out", ActionIcon::ZoomOut, [view] { view->zoom(0.8); });
    button("Zoom in", ActionIcon::ZoomIn, [view] { view->zoom(1.25); });
    button("1:1", ActionIcon::ActualSize, [view] { view->actualSize(); });
    button("Fit", ActionIcon::Fit, [view] { view->fit(); });
    button("Find selected", ActionIcon::Inspect, [&s, camera] {
      if (s.validPair() && s.selected >= 0 && s.selected < int(s.matches().size()))
        s.views[camera]->reveal(endpoint(s.matches()[s.selected], camera));
    });
    layout->addLayout(nav);
    split->addWidget(group);
    view->select = [&s](int index) { s.select(index); };
    view->begin_drag = [&s] { s.drag_before = s.matches(); };
    view->move_point = [&s, camera](QPointF point) { s.move(camera, point); };
    view->finish_drag = [&s] {
      if (s.drag_before) {
        s.record({{{s.pair, *s.drag_before, s.matches(), s.selected, s.selected}}});
        s.drag_before.reset();
      }
    };
    view->add_point = [&s, camera](QPointF point) { s.addPoint(camera, point); };
  }
  s.status = new QLabel();
  s.status->setWordWrap(true);
  s.status->setObjectName("matchEditorStatus");
  root->addWidget(s.status);
  auto* values = new QHBoxLayout();
  s.selection = new QComboBox();
  s.selection->setObjectName("matchEditorSelection");
  values->addWidget(s.selection);
  for (int i = 0; i < 4; ++i) {
    values->addWidget(new QLabel(QString(i < 2 ? "Left %1" : "Right %1").arg(i % 2 ? "Y" : "X")));
    auto* field = new QDoubleSpinBox();
    s.coordinates[i] = field;
    field->setObjectName(QString("matchEditorCoordinate%1").arg(i));
    field->setDecimals(9);
    field->setSingleStep(0.25);
    field->setKeyboardTracking(false);
    values->addWidget(field);
    connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [&s, i](double value) {
      if (!s.editable() || s.selected < 0 || s.selected >= int(s.matches().size()))
        return;
      const Matches before = s.matches();
      QPointF point = endpoint(s.matches()[s.selected], i / 2);
      if (i % 2)
        point.setY(value);
      else
        point.setX(value);
      s.move(i / 2, point);
      s.record({{{s.pair, before, s.matches(), s.selected, s.selected}}});
    });
  }
  root->addLayout(values);
  auto* actions = new QHBoxLayout();
  const auto action = [&](const QString& text, const QString& name, ActionIcon icon, std::function<void()> callback) {
    auto* button = new QPushButton(action_icon(icon), text);
    button->setObjectName(name);
    button->setAutoDefault(false);
    connect(button, &QPushButton::clicked, this, std::move(callback));
    actions->addWidget(button);
    return button;
  };
  s.add = action("Add match", "matchEditorAdd", ActionIcon::Add, [&s] {
    if (s.adding) {
      s.cancelAdd();
      return;
    }
    s.adding = true;
    s.pending = {};
    for (auto* view : s.views)
      view->adding = true;
    s.refresh();
  });
  s.remove = action("Delete", "matchEditorDelete", ActionIcon::Delete, [&s] {
    if (!s.editable() || s.selected < 0 || s.selected >= int(s.matches().size()))
      return;
    const Matches before = s.matches();
    const int previous = s.selected;
    s.matches().erase(s.matches().begin() + s.selected);
    s.selected = std::min(s.selected, int(s.matches().size()) - 1);
    s.record({{{s.pair, before, s.matches(), previous, s.selected}}});
  });
  s.undo = action("Undo", "matchEditorUndo", ActionIcon::Undo, [&s] { s.travel(false); });
  s.redo = action("Redo", "matchEditorRedo", ActionIcon::Next, [&s] { s.travel(true); });
  s.reset = action("Reset all to automatic", "matchEditorReset", ActionIcon::Reset, [&s] { s.resetAutomatic(); });
  s.automatic_compatible = !s.set.frames.empty() && s.automatic.frames.size() == s.set.frames.size() &&
      s.automatic.input_fingerprint == s.set.input_fingerprint;
  if (s.automatic_compatible)
    for (size_t i = 0; i < s.set.frames.size(); ++i)
      s.automatic_compatible &= s.automatic.frames[i].sizes == s.set.frames[i].sizes &&
          s.automatic.frames[i].source_paths == s.set.frames[i].source_paths &&
          s.automatic.frames[i].source_seconds == s.set.frames[i].source_seconds &&
          (!s.set.input_fingerprint.empty() || s.automatic.frames[i].images == s.set.frames[i].images);
  actions->addStretch();
  action("Cancel", "matchEditorCancel", ActionIcon::Cancel, [this] { reject(); });
  s.save = action("Save & recalibrate", "matchEditorSave", ActionIcon::Save, [this] {
    auto& state = *impl_;
    if (!state.editable() || state.adding || state.invoking_save)
      return;
    QScopedValueRollback<bool> guard(state.invoking_save, true);
    for (auto* field : state.coordinates)
      field->interpretText();
    if (state.save_handler)
      state.save_handler();
    else
      accept();
  });
  root->addLayout(actions);
  connect(s.pairs, qOverload<int>(&QComboBox::currentIndexChanged), this, [&s](int index) { s.loadPair(index); });
  connect(s.selection, qOverload<int>(&QComboBox::currentIndexChanged), this, [&s](int index) { s.select(index); });
  connect(s.maximize, &QPushButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
  s.maximize->setAutoDefault(false);
  const auto shortcut = [&](const QKeySequence& keys, QPushButton* button) {
    auto* key = new QShortcut(keys, this);
    connect(key, &QShortcut::activated, button, &QPushButton::click);
  };
  shortcut(QKeySequence::Undo, s.undo);
  shortcut(QKeySequence::Redo, s.redo);
  s.maximize->setIcon(action_icon(ActionIcon::Expand));
  s.maximize->setText("Maximize");
  s.loadPair(s.set.frames.empty() ? -1 : 0);
}

MatchEditorDialog::~MatchEditorDialog() = default;
CalibrationMatchSet MatchEditorDialog::editedSet() const {
  auto result = impl_->set;
  result.manual = true;
  result.automatic_fingerprint = impl_->automatic.fingerprint;
  result.fingerprint.clear();
  return result;
}
void MatchEditorDialog::setSaveHandler(std::function<void()> handler) {
  impl_->save_handler = std::move(handler);
}
void MatchEditorDialog::setSaving(bool saving, const QString& error) {
  impl_->saving = saving;
  setEnabled(!saving);
  impl_->refresh();
  if (!saving && !error.isEmpty())
    impl_->status->setText(error);
}
void MatchEditorDialog::reject() {
  auto& s = *impl_;
  if (s.close_prompt_active || s.saving)
    return;
  // The title-bar close path may arrive before a spin box loses focus. Commit
  // any typed coordinate so dirty detection cannot overlook that pending edit.
  for (auto* field : s.coordinates)
    field->interpretText();
  if (s.changed()) {
    s.close_prompt_active = true;
    QMessageBox prompt(
        QMessageBox::Question,
        "Discard match changes?",
        "Your match edits have not been saved. Discard these changes and close the editor?",
        QMessageBox::NoButton,
        this);
    prompt.setObjectName("matchEditorDiscardPrompt");
    auto* discard = prompt.addButton("Discard changes", QMessageBox::DestructiveRole);
    discard->setObjectName("matchEditorDiscardChanges");
    discard->setIcon(action_icon(ActionIcon::Delete));
    auto* keep = prompt.addButton("Keep editing", QMessageBox::RejectRole);
    keep->setObjectName("matchEditorKeepEditing");
    keep->setIcon(action_icon(ActionIcon::Cancel));
    prompt.setDefaultButton(keep);
    prompt.setEscapeButton(keep);
    prompt.exec();
    s.close_prompt_active = false;
    if (prompt.clickedButton() != discard)
      return;
  }
  QDialog::reject();
}
void MatchEditorDialog::keyPressEvent(QKeyEvent* event) {
  if (impl_->saving) {
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape && impl_->adding) {
    impl_->cancelAdd();
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
}
void MatchEditorDialog::changeEvent(QEvent* event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange && impl_ && impl_->maximize) {
    impl_->maximize->setText(isMaximized() ? "Restore window" : "Maximize");
    impl_->maximize->setIcon(action_icon(isMaximized() ? ActionIcon::Restore : ActionIcon::Expand));
  }
}
