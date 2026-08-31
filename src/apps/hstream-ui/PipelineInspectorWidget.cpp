#include "src/apps/hstream-ui/PipelineInspectorWidget.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QBrush>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QGraphicsPolygonItem>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyleOptionGraphicsItem>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>

namespace {

constexpr char kProtocolPrefix[] = "HSTREAM_PIPELINE_INSPECTOR ";
constexpr int kNodeIdRole = 1;
constexpr int kNodeBinRole = 2;

struct NodeVisual {
  QString name;
  QString factory;
  QString type;
  QString state;
  QString path;
  QStringList source_pads;
  QStringList sink_pads;
  bool bin{false};
};

class PipelineGraphView : public QGraphicsView {
 public:
  explicit PipelineGraphView(QWidget* parent = nullptr) : QGraphicsView(parent) {
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setCursor(Qt::ArrowCursor);
  }

  void zoomBy(qreal factor) {
    const qreal current = transform().m11();
    const qreal requested = current * factor;
    if (requested < 0.08 || requested > 8.0) {
      return;
    }
    scale(factor, factor);
  }

  void setOverlayButton(QToolButton* button) {
    overlay_button_ = button;
    positionOverlayButton();
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QGraphicsView::resizeEvent(event);
    positionOverlayButton();
  }

  void mousePressEvent(QMouseEvent* event) override {
    const HitTarget target = hitTargetAt(event->pos());
    if (event->button() == Qt::LeftButton && target == HitTarget::kEmpty) {
      if (scene()) {
        scene()->clearSelection();
      }
      panning_ = true;
      last_pan_pos_ = event->pos();
      setCursor(Qt::ClosedHandCursor);
      event->accept();
      return;
    }
    if (event->button() == Qt::LeftButton && target == HitTarget::kBin) {
      pending_bin_pan_ = true;
      bin_pan_press_pos_ = event->pos();
      last_pan_pos_ = event->pos();
      QGraphicsItem* item = nodeItemAt(event->pos());
      pending_bin_node_id_ = item ? item->data(kNodeIdRole).toString() : QString{};
      event->accept();
      return;
    }
    QGraphicsView::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (pending_bin_pan_ && !panning_ &&
        (event->pos() - bin_pan_press_pos_).manhattanLength() >= QApplication::startDragDistance()) {
      pending_bin_pan_ = false;
      panning_ = true;
      pending_bin_node_id_.clear();
      last_pan_pos_ = bin_pan_press_pos_;
      if (scene()) {
        scene()->clearSelection();
      }
      setCursor(Qt::ClosedHandCursor);
    }
    if (panning_) {
      const QPoint delta = event->pos() - last_pan_pos_;
      last_pan_pos_ = event->pos();
      horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
      verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
      event->accept();
      return;
    }
    QGraphicsView::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && panning_) {
      panning_ = false;
      pending_bin_pan_ = false;
      pending_bin_node_id_.clear();
      setCursor(Qt::ArrowCursor);
      event->accept();
      return;
    }
    if (event->button() == Qt::LeftButton && pending_bin_pan_) {
      pending_bin_pan_ = false;
      QGraphicsItem* pending_item = findNodeItem(pending_bin_node_id_);
      if (pending_item) {
        if (scene()) {
          scene()->clearSelection();
        }
        pending_item->setSelected(true);
      }
      pending_bin_node_id_.clear();
      event->accept();
      return;
    }
    if (event->button() == Qt::LeftButton) {
      pending_bin_node_id_.clear();
    }
    QGraphicsView::mouseReleaseEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override {
    const qreal steps = static_cast<qreal>(event->angleDelta().y()) / 120.0;
    if (steps == 0.0) {
      QGraphicsView::wheelEvent(event);
      return;
    }
    zoomBy(std::pow(1.18, steps));
    event->accept();
  }

 private:
  enum class HitTarget {
    kEmpty,
    kBin,
    kNode,
  };

  void positionOverlayButton() {
    if (!overlay_button_) {
      return;
    }
    constexpr int kMargin = 6;
    overlay_button_->move(width() - overlay_button_->width() - kMargin, kMargin);
    overlay_button_->raise();
  }

  HitTarget hitTargetAt(const QPoint& view_pos) const {
    QGraphicsItem* item = nodeItemAt(view_pos);
    return item ? itemTarget(item) : HitTarget::kEmpty;
  }

  QGraphicsItem* nodeItemAt(const QPoint& view_pos) const {
    return nodeItemAt(view_pos, /*bin=*/nullptr);
  }

  QGraphicsItem* nodeItemAt(const QPoint& view_pos, bool* bin) const {
    QGraphicsItem* item = itemAt(view_pos);
    while (item) {
      if (!item->data(kNodeIdRole).toString().isEmpty()) {
        if (bin) {
          *bin = item->data(kNodeBinRole).toBool();
        }
        return item;
      }
      item = item->parentItem();
    }
    return nullptr;
  }

  HitTarget itemTarget(QGraphicsItem* item) const {
    return item->data(kNodeBinRole).toBool() ? HitTarget::kBin : HitTarget::kNode;
  }

  QGraphicsItem* findNodeItem(const QString& node_id) const {
    if (!scene() || node_id.isEmpty()) {
      return nullptr;
    }
    for (QGraphicsItem* item : scene()->items()) {
      if (item->data(kNodeIdRole).toString() == node_id) {
        return item;
      }
    }
    return nullptr;
  }

  bool panning_{false};
  bool pending_bin_pan_{false};
  QPoint last_pan_pos_;
  QPoint bin_pan_press_pos_;
  QString pending_bin_node_id_;
  QToolButton* overlay_button_{nullptr};
};

QIcon graph_focus_icon(bool focused) {
  QPixmap pixmap(16, 16);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
  const int outer = focused ? 3 : 2;
  const int inner = focused ? 6 : 5;
  const int far = focused ? 12 : 13;
  const int arm = inner - outer;
  painter.drawLine(outer, inner, outer, outer);
  painter.drawLine(outer, outer, inner, outer);
  painter.drawLine(far, inner, far, outer);
  painter.drawLine(far, outer, far - arm, outer);
  painter.drawLine(outer, far - arm, outer, far);
  painter.drawLine(outer, far, inner, far);
  painter.drawLine(far, far - arm, far, far);
  painter.drawLine(far, far, far - arm, far);
  return QIcon(pixmap);
}

QString elidedLabel(QPainter* painter, const QString& value, qreal maximum_width) {
  return painter->fontMetrics().elidedText(value, Qt::ElideRight, static_cast<int>(maximum_width));
}

QString normalizedKind(const NodeVisual& node) {
  if (node.bin) {
    return "bin";
  }
  const QString value = QString("%1 %2").arg(node.factory, node.type).toLower();
  if (value.contains("sink")) {
    return "sink";
  }
  if (value.contains("src") || value.contains("source") || value.contains("decode")) {
    return "source";
  }
  if (value.contains("infer") || value.contains("gie") || value.contains("yolo")) {
    return "inference";
  }
  if (value.contains("track")) {
    return "tracking";
  }
  if (value.contains("streammux") || value.contains("mux")) {
    return "mux";
  }
  if (value.contains("osd") || value.contains("overlay") || value.contains("draw")) {
    return "overlay";
  }
  if (value.contains("convert") || value.contains("scale") || value.contains("caps") || value.contains("filter")) {
    return "transform";
  }
  if (value.contains("queue") || value.contains("tee")) {
    return "routing";
  }
  return "element";
}

QColor nodeColor(const NodeVisual& node) {
  const QString kind = normalizedKind(node);
  if (kind == "bin") {
    return QColor("#26364f");
  }
  if (kind == "source") {
    return QColor("#1f6f8b");
  }
  if (kind == "sink") {
    return QColor("#7a4f1d");
  }
  if (kind == "inference") {
    return QColor("#7c3aed");
  }
  if (kind == "tracking") {
    return QColor("#b83280");
  }
  if (kind == "mux") {
    return QColor("#0f766e");
  }
  if (kind == "overlay") {
    return QColor("#b7791f");
  }
  if (kind == "transform") {
    return QColor("#4263a3");
  }
  if (kind == "routing") {
    return QColor("#4c566a");
  }
  return QColor("#394150");
}

QColor stateColor(const QString& state) {
  if (state.compare("PLAYING", Qt::CaseInsensitive) == 0) {
    return QColor("#8ce99a");
  }
  if (state.compare("PAUSED", Qt::CaseInsensitive) == 0) {
    return QColor("#ffd166");
  }
  return QColor("#cbd5e1");
}

class PipelineNodeItem : public QGraphicsRectItem {
 public:
  PipelineNodeItem(const QRectF& rect, NodeVisual visual, QGraphicsItem* parent = nullptr)
      : QGraphicsRectItem(rect, parent), visual_(std::move(visual)) {}

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override {
    (void)widget;
    const QRectF bounds = rect();
    const qreal lod = option->levelOfDetailFromTransform(painter->worldTransform());
    QColor fill = nodeColor(visual_);
    if (visual_.bin) {
      fill.setAlphaF(lod < 0.55 ? 0.24 : 0.34);
    }
    painter->setPen(QPen(isSelected() ? QColor("#f8fafc") : QColor("#91a4bf"), isSelected() ? 2.0 : 1.2));
    painter->setBrush(fill);
    painter->drawRoundedRect(bounds, visual_.bin ? 8.0 : 5.0, visual_.bin ? 8.0 : 5.0);

    if (lod < 0.34) {
      return;
    }

    const qreal inset = visual_.bin ? 12.0 : 8.0;
    const qreal text_width = std::max<qreal>(24.0, bounds.width() - inset * 2.0);
    QFont title_font = painter->font();
    title_font.setBold(true);
    title_font.setPointSizeF(visual_.bin ? title_font.pointSizeF() + 0.5 : title_font.pointSizeF());
    painter->setFont(title_font);
    painter->setPen(Qt::white);
    painter->drawText(
        QPointF(bounds.left() + inset, bounds.top() + (visual_.bin ? 20.0 : 18.0)),
        elidedLabel(painter, visual_.name, text_width));

    if (lod < 0.68) {
      return;
    }

    QFont detail_font = painter->font();
    detail_font.setBold(false);
    detail_font.setPointSizeF(std::max(7.0, detail_font.pointSizeF() - 1.0));
    painter->setFont(detail_font);
    painter->setPen(QColor("#d9e2ef"));
    const QString detail = visual_.factory.isEmpty() ? visual_.type : visual_.factory;
    painter->drawText(
        QPointF(bounds.left() + inset, bounds.top() + (visual_.bin ? 39.0 : 36.0)),
        elidedLabel(painter, detail, text_width));
    painter->setPen(stateColor(visual_.state));
    painter->drawText(
        QPointF(bounds.left() + inset, bounds.top() + (visual_.bin ? 57.0 : 53.0)),
        elidedLabel(painter, visual_.state, text_width));

    if (lod < 1.25 || visual_.bin) {
      return;
    }

    painter->setPen(QColor("#b8c2d4"));
    painter->drawText(
        QPointF(bounds.left() + inset, bounds.top() + 70.0), elidedLabel(painter, visual_.type, text_width));
    if (lod < 1.85) {
      return;
    }
    painter->setPen(QColor("#94a3b8"));
    painter->drawText(
        QPointF(bounds.left() + inset, bounds.top() + 87.0), elidedLabel(painter, visual_.path, text_width));
    const QString pads = QString("src: %1   sink: %2")
                             .arg(
                                 visual_.source_pads.isEmpty() ? "-" : visual_.source_pads.join(", "),
                                 visual_.sink_pads.isEmpty() ? "-" : visual_.sink_pads.join(", "));
    painter->drawText(QPointF(bounds.left() + inset, bounds.top() + 104.0), elidedLabel(painter, pads, text_width));
  }

 private:
  NodeVisual visual_;
};

QPointF edgeAnchor(const QRectF& rect, const QPointF& other) {
  const QPointF center = rect.center();
  const qreal dx = other.x() - center.x();
  const qreal dy = other.y() - center.y();
  if (std::abs(dx) * rect.height() > std::abs(dy) * rect.width()) {
    return QPointF(dx >= 0.0 ? rect.right() : rect.left(), center.y() + dy * (rect.width() / 2.0) / std::abs(dx));
  }
  if (std::abs(dy) < 0.001) {
    return QPointF(dx >= 0.0 ? rect.right() : rect.left(), center.y());
  }
  return QPointF(center.x() + dx * (rect.height() / 2.0) / std::abs(dy), dy >= 0.0 ? rect.bottom() : rect.top());
}

QLineF childBinEdgeLine(const QRectF& child_rect, const QRectF& bin_rect) {
  const qreal y = std::clamp(child_rect.center().y(), bin_rect.top() + 26.0, bin_rect.bottom() - 12.0);
  const bool use_left = child_rect.center().x() - bin_rect.left() <= bin_rect.right() - child_rect.center().x();
  if (use_left) {
    return QLineF(QPointF(child_rect.left(), y), QPointF(bin_rect.left() + 12.0, y));
  }
  return QLineF(QPointF(child_rect.right(), y), QPointF(bin_rect.right() - 12.0, y));
}

QLineF edgeLine(
    const QRectF& source_rect,
    const QRectF& sink_rect,
    bool source_descends_from_sink,
    bool sink_descends_from_source) {
  if (source_descends_from_sink) {
    return childBinEdgeLine(source_rect, sink_rect);
  }
  if (sink_descends_from_source) {
    const QLineF line = childBinEdgeLine(sink_rect, source_rect);
    return QLineF(line.p2(), line.p1());
  }
  return QLineF(edgeAnchor(source_rect, sink_rect.center()), edgeAnchor(sink_rect, source_rect.center()));
}

QString propertySummary(const QJsonObject& property) {
  const QString minimum = property.value("minimum").toString();
  const QString maximum = property.value("maximum").toString();
  if (!minimum.isEmpty() || !maximum.isEmpty()) {
    return QString("%1; range %2 … %3")
        .arg(
            property.value("editReason").toString(),
            minimum.isEmpty() ? "?" : minimum,
            maximum.isEmpty() ? "?" : maximum);
  }
  return property.value("editReason").toString();
}

} // namespace

PipelineInspectorWidget::PipelineInspectorWidget(QWidget* parent) : QWidget(parent) {
  setObjectName("pipelineInspectorWidget");
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  root->setSpacing(6);

  auto* toolbar = new QHBoxLayout();
  auto* refresh = new QPushButton("Refresh");
  refresh->setObjectName("pipelineInspectorRefreshButton");
  auto* zoom_out = new QPushButton("−");
  zoom_out->setObjectName("pipelineInspectorZoomOutButton");
  auto* zoom_in = new QPushButton("+");
  zoom_in->setObjectName("pipelineInspectorZoomInButton");
  auto* fit = new QPushButton("Fit");
  fit->setObjectName("pipelineInspectorFitButton");
  node_search_ = new QLineEdit();
  node_search_->setObjectName("pipelineInspectorNodeSearch");
  node_search_->setPlaceholderText("Find node by name, factory, or path");
  auto* find_next = new QPushButton("Find next");
  find_next->setObjectName("pipelineInspectorFindNextButton");
  toolbar->addWidget(refresh);
  toolbar->addSpacing(8);
  toolbar->addWidget(zoom_out);
  toolbar->addWidget(zoom_in);
  toolbar->addWidget(fit);
  toolbar->addSpacing(12);
  toolbar->addWidget(node_search_, 1);
  toolbar->addWidget(find_next);
  root->addLayout(toolbar);

  splitter_ = new QSplitter(Qt::Horizontal);
  splitter_->setObjectName("pipelineInspectorSplitter");
  splitter_->setChildrenCollapsible(false);
  graph_scene_ = new QGraphicsScene(this);
  graph_view_ = new PipelineGraphView();
  graph_view_->setObjectName("pipelineInspectorGraphView");
  graph_view_->setScene(graph_scene_);
  graph_view_->setBackgroundBrush(QColor("#11151b"));
  graph_view_->setToolTip("Drag empty space to pan. Use the mouse wheel or +/− buttons to zoom.");
  graph_maximize_button_ = new QToolButton(graph_view_);
  graph_maximize_button_->setObjectName("pipelineInspectorMaximizeButton");
  graph_maximize_button_->setFixedSize(24, 24);
  graph_maximize_button_->setIconSize(QSize(14, 14));
  graph_maximize_button_->setIcon(graph_focus_icon(false));
  graph_maximize_button_->setAccessibleName("Maximize pipeline graph");
  graph_maximize_button_->setToolTip("Maximize the pipeline graph while keeping properties visible.");
  graph_maximize_button_->setStyleSheet(
      "QToolButton { background: rgba(15, 23, 42, 210); border: 1px solid rgba(255, 255, 255, 100); "
      "border-radius: 3px; color: white; padding: 0; }"
      "QToolButton:hover { background: rgba(30, 64, 175, 235); }");
  graph_maximize_button_->raise();
  static_cast<PipelineGraphView*>(graph_view_)->setOverlayButton(graph_maximize_button_);
  splitter_->addWidget(graph_view_);

  auto* properties = new QWidget();
  properties->setObjectName("pipelineInspectorPropertiesPane");
  properties->setMinimumWidth(280);
  auto* properties_layout = new QVBoxLayout(properties);
  properties_layout->setContentsMargins(0, 0, 0, 0);
  selected_node_label_ = new QLabel("Select a pipeline node");
  selected_node_label_->setObjectName("pipelineInspectorSelectedNode");
  selected_node_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  selected_node_label_->setWordWrap(true);
  property_filter_ = new QLineEdit();
  property_filter_->setObjectName("pipelineInspectorPropertyFilter");
  property_filter_->setPlaceholderText("Filter properties");
  property_table_ = new QTableWidget(0, 4);
  property_table_->setObjectName("pipelineInspectorPropertyTable");
  property_table_->setHorizontalHeaderLabels({"Property", "Current value", "Type", "Runtime access"});
  property_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  property_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  property_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  property_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  property_table_->verticalHeader()->hide();
  property_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  property_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  property_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

  auto* editor_row = new QHBoxLayout();
  property_editor_ = new QComboBox();
  property_editor_->setObjectName("pipelineInspectorPropertyEditor");
  property_editor_->setEditable(true);
  property_editor_->setEnabled(false);
  apply_button_ = new QPushButton("Apply live value");
  apply_button_->setObjectName("pipelineInspectorApplyButton");
  apply_button_->setEnabled(false);
  editor_row->addWidget(property_editor_, 1);
  editor_row->addWidget(apply_button_);
  properties_layout->addWidget(selected_node_label_);
  properties_layout->addWidget(property_filter_);
  properties_layout->addWidget(property_table_, 1);
  properties_layout->addLayout(editor_row);
  splitter_->addWidget(properties);
  splitter_->setStretchFactor(0, 3);
  splitter_->setStretchFactor(1, 2);
  splitter_->setSizes({850, 470});
  normal_splitter_sizes_ = splitter_->sizes();
  root->addWidget(splitter_, 1);

  status_label_ = new QLabel("Start the pipeline, then refresh to inspect its live GStreamer graph.");
  status_label_->setObjectName("pipelineInspectorStatus");
  status_label_->setWordWrap(true);
  root->addWidget(status_label_);

  connect(refresh, &QPushButton::clicked, this, [this]() { requestRefresh(); });
  connect(zoom_out, &QPushButton::clicked, this, [this]() {
    static_cast<PipelineGraphView*>(graph_view_)->zoomBy(1.0 / 1.25);
  });
  connect(
      zoom_in, &QPushButton::clicked, this, [this]() { static_cast<PipelineGraphView*>(graph_view_)->zoomBy(1.25); });
  connect(graph_maximize_button_, &QToolButton::clicked, this, [this]() { setGraphMaximized(!graph_maximized_); });
  connect(fit, &QPushButton::clicked, this, [this]() {
    if (!graph_scene_->items().empty()) {
      graph_view_->fitInView(graph_scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    }
  });
  connect(find_next, &QPushButton::clicked, this, [this]() { selectNextSearchMatch(); });
  connect(node_search_, &QLineEdit::returnPressed, this, [this]() { selectNextSearchMatch(); });
  connect(property_filter_, &QLineEdit::textChanged, this, [this](const QString& filter) {
    for (int row = 0; row < property_table_->rowCount(); ++row) {
      const bool matches = filter.trimmed().isEmpty() ||
          property_table_->item(row, 0)->text().contains(filter, Qt::CaseInsensitive) ||
          property_table_->item(row, 1)->text().contains(filter, Qt::CaseInsensitive);
      property_table_->setRowHidden(row, !matches);
    }
  });
  connect(property_table_, &QTableWidget::itemSelectionChanged, this, [this]() { updatePropertyEditor(); });
  connect(apply_button_, &QPushButton::clicked, this, [this]() { applySelectedProperty(); });
  connect(graph_scene_, &QGraphicsScene::selectionChanged, this, [this]() {
    const QList<QGraphicsItem*> selected = graph_scene_->selectedItems();
    if (selected.isEmpty()) {
      pending_property_request_ = 0;
      pending_set_request_ = 0;
      selected_node_id_.clear();
      property_table_->setRowCount(0);
      displayed_properties_.clear();
      selected_node_label_->setText("Select a pipeline node");
      updatePropertyEditor();
      return;
    }
    const QString node_id = selected.front()->data(kNodeIdRole).toString();
    if (!node_id.isEmpty() && node_id != selected_node_id_) {
      selectNode(node_id);
    }
  });
}

void PipelineInspectorWidget::setCommandWriter(CommandWriter writer) {
  command_writer_ = std::move(writer);
}

void PipelineInspectorWidget::setPipelineRunning(bool running) {
  pipeline_running_ = running;
  if (!running) {
    have_session_ = false;
    session_stage_ = 0;
    session_generation_ = 0;
    clearInspectionState();
    updateStatus("Start the pipeline, then refresh to inspect its live GStreamer graph.");
  } else if (!have_session_) {
    updateStatus("Waiting for the live pipeline inspector session…");
  }
}

void PipelineInspectorWidget::clearInspectionState() {
  pending_graph_request_ = 0;
  pending_property_request_ = 0;
  pending_set_request_ = 0;
  nodes_.clear();
  edges_.clear();
  selected_node_id_.clear();
  graph_scene_->clear();
  node_items_.clear();
  property_table_->setRowCount(0);
  displayed_properties_.clear();
  selected_node_label_->setText("Select a pipeline node");
  updatePropertyEditor();
}

bool PipelineInspectorWidget::writeCommand(const QByteArray& command) {
  if (!pipeline_running_) {
    updateStatus("The live pipeline is not running.", true);
    return false;
  }
  if (!command_writer_ || !command_writer_(command)) {
    updateStatus("Could not write the inspector request to the pipeline process.", true);
    return false;
  }
  return true;
}

uint64_t PipelineInspectorWidget::nextRequestId() {
  if (next_request_id_ == std::numeric_limits<uint64_t>::max()) {
    next_request_id_ = 0;
  }
  return ++next_request_id_;
}

QByteArray PipelineInspectorWidget::encodeToken(const QString& value) {
  return value.toUtf8().toBase64(QByteArray::Base64Encoding);
}

void PipelineInspectorWidget::requestRefresh() {
  if (!pipeline_running_) {
    updateStatus("The live pipeline is not running.", true);
    return;
  }
  if (!have_session_ || session_generation_ == 0) {
    updateStatus("Waiting for the live pipeline inspector session…");
    return;
  }
  const uint64_t request_id = nextRequestId();
  if (writeCommand(QString("@inspect-pipeline %1 %2 %3\n")
                       .arg(request_id)
                       .arg(session_stage_)
                       .arg(session_generation_)
                       .toUtf8())) {
    pending_graph_request_ = request_id;
    updateStatus("Reading live GStreamer topology…");
  }
}

void PipelineInspectorWidget::requestProperties(const NodeData& node) {
  const uint64_t request_id = nextRequestId();
  const QByteArray command = QString("@inspect-properties %1 %2 %3 %4 %5\n")
                                 .arg(request_id)
                                 .arg(session_stage_)
                                 .arg(session_generation_)
                                 .arg(node.app_index)
                                 .arg(QString::fromLatin1(encodeToken(node.path)))
                                 .toUtf8();
  if (writeCommand(command)) {
    pending_property_request_ = request_id;
    selected_node_label_->setText(QString("%1\n%2\nLoading properties…").arg(node.name, node.path));
  }
}

bool PipelineInspectorWidget::handleBackendLine(const QString& line) {
  if (!line.startsWith(QLatin1String(kProtocolPrefix))) {
    return false;
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(
      line.mid(static_cast<int>(std::char_traits<char>::length(kProtocolPrefix))).toUtf8(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    updateStatus(QString("Malformed pipeline inspector response: %1").arg(parse_error.errorString()), true);
    return true;
  }
  const QJsonObject response = document.object();
  if (response.value("version").toInt() != 1) {
    updateStatus("Unsupported pipeline inspector protocol version.", true);
    return true;
  }
  const QString kind = response.value("kind").toString();
  const uint64_t request_id = static_cast<uint64_t>(response.value("requestId").toInteger());
  if (kind == "session") {
    const qint64 stage = response.value("stage").toInteger(std::numeric_limits<qint64>::min());
    const qint64 generation = response.value("generation").toInteger(0);
    if (response.value("status").toString() != "ok" || stage == std::numeric_limits<qint64>::min() || generation <= 0) {
      clearInspectionState();
      have_session_ = false;
      updateStatus("Invalid live pipeline inspector session.", true);
      return true;
    }
    const uint64_t next_generation = static_cast<uint64_t>(generation);
    const bool changed = !have_session_ || session_stage_ != stage || session_generation_ != next_generation;
    have_session_ = true;
    session_stage_ = stage;
    session_generation_ = next_generation;
    if (changed) {
      clearInspectionState();
      updateStatus(QString("Pipeline inspector session changed to stage %1, generation %2; refreshing…")
                       .arg(session_stage_)
                       .arg(session_generation_));
      if (pipeline_running_) {
        requestRefresh();
      }
    }
    return true;
  }
  if (response.value("status").toString() != "ok") {
    if (response.contains("stage") && response.contains("generation") && !responseMatchesSession(response)) {
      return true;
    }
    if (kind == "graph" && request_id == pending_graph_request_) {
      pending_graph_request_ = 0;
    } else if (kind == "properties" && request_id == pending_property_request_) {
      pending_property_request_ = 0;
    } else if (kind == "set-result" && request_id == pending_set_request_) {
      pending_set_request_ = 0;
      updatePropertyEditor();
    } else if (kind == "graph" || kind == "properties" || kind == "set-result") {
      return true;
    }
    const QString message = response.value("message").toString("Pipeline inspector request failed.");
    if (message.startsWith("Stale pipeline inspector", Qt::CaseInsensitive)) {
      clearInspectionState();
    }
    updateStatus(message, true);
    return true;
  }
  if (kind == "graph") {
    if (request_id != pending_graph_request_) {
      return true;
    }
    if (!responseMatchesSession(response)) {
      clearInspectionState();
      updateStatus("Ignored a graph from a stale pipeline inspector session.", true);
      return true;
    }
    pending_graph_request_ = 0;
    nodes_.clear();
    edges_.clear();
    for (const QJsonValue& value : response.value("nodes").toArray()) {
      const QJsonObject object = value.toObject();
      NodeData node{
          .id = object.value("id").toString(),
          .app_index = object.value("appIndex").toInt(),
          .path = object.value("path").toString(),
          .parent_id = object.value("parentId").toString(),
          .name = object.value("name").toString(),
          .factory = object.value("factory").toString(),
          .type = object.value("type").toString(),
          .state = object.value("state").toString(),
          .bin = object.value("bin").toBool(),
      };
      if (!node.id.isEmpty() && !node.path.isEmpty()) {
        nodes_[node.id] = std::move(node);
      }
    }
    for (const QJsonValue& value : response.value("edges").toArray()) {
      const QJsonObject object = value.toObject();
      EdgeData edge{
          .source = object.value("source").toString(),
          .source_pad = object.value("sourcePad").toString(),
          .sink = object.value("sink").toString(),
          .sink_pad = object.value("sinkPad").toString(),
      };
      if (nodes_.count(edge.source) && nodes_.count(edge.sink)) {
        edges_.push_back(std::move(edge));
      }
    }
    renderGraph();
    updateStatus(QString("Live graph: %1 nodes, %2 pad connections. Drag to pan; wheel to zoom.")
                     .arg(static_cast<qulonglong>(nodes_.size()))
                     .arg(static_cast<qulonglong>(edges_.size())));
    return true;
  }
  if (kind == "properties") {
    if (request_id != pending_property_request_ || !responseMatchesSession(response) ||
        response.value("nodeId").toString() != selected_node_id_) {
      return true;
    }
    pending_property_request_ = 0;
    showProperties(response);
    return true;
  }
  if (kind == "set-result") {
    if (request_id != pending_set_request_ || !responseMatchesSession(response)) {
      return true;
    }
    pending_set_request_ = 0;
    updateStatus(QString("Updated %1. Reading back the live value…").arg(response.value("property").toString()));
    const auto node = nodes_.find(selected_node_id_);
    if (node != nodes_.end()) {
      requestProperties(node->second);
    }
    return true;
  }
  updateStatus(QString("Unknown pipeline inspector response kind: %1").arg(kind), true);
  return true;
}

bool PipelineInspectorWidget::responseMatchesSession(const QJsonObject& response) const {
  return have_session_ && response.value("stage").toInteger(std::numeric_limits<qint64>::min()) == session_stage_ &&
      response.value("generation").toInteger(0) == static_cast<qint64>(session_generation_);
}

void PipelineInspectorWidget::renderGraph() {
  graph_scene_->clear();
  node_items_.clear();
  selected_node_id_.clear();
  property_table_->setRowCount(0);
  displayed_properties_.clear();
  updatePropertyEditor();
  if (nodes_.empty()) {
    return;
  }

  std::map<QString, std::set<QString>> successors;
  std::map<QString, int> indegree;
  for (const auto& [id, node] : nodes_) {
    (void)node;
    indegree[id] = 0;
  }
  auto add_layout_edge = [&](const QString& source, const QString& sink) {
    if (source.isEmpty() || source == sink || !nodes_.count(source) || !nodes_.count(sink)) {
      return;
    }
    if (successors[source].insert(sink).second) {
      ++indegree[sink];
    }
  };
  for (const EdgeData& edge : edges_) {
    add_layout_edge(edge.source, edge.sink);
  }

  std::queue<QString> ready;
  std::map<QString, int> layer;
  for (const auto& [id, degree] : indegree) {
    if (degree == 0) {
      ready.push(id);
    }
  }
  while (!ready.empty()) {
    const QString source = ready.front();
    ready.pop();
    for (const QString& sink : successors[source]) {
      layer[sink] = std::max(layer[sink], layer[source] + 1);
      if (--indegree[sink] == 0) {
        ready.push(sink);
      }
    }
  }
  int fallback_layer = 0;
  for (const auto& [id, degree] : indegree) {
    if (degree > 0) {
      layer[id] = std::max(layer[id], fallback_layer++ % 8);
    }
  }
  std::map<QString, std::vector<QString>> children;
  for (const auto& [id, node] : nodes_) {
    if (!node.parent_id.isEmpty() && nodes_.count(node.parent_id)) {
      children[node.parent_id].push_back(id);
    }
  }

  auto containment_key = [&](const QString& id) {
    QStringList parts;
    std::set<QString> visited;
    QString current = id;
    while (!current.isEmpty() && nodes_.count(current) && !visited.count(current)) {
      visited.insert(current);
      parts.prepend(nodes_.at(current).id);
      current = nodes_.at(current).parent_id;
    }
    return parts.join(QChar(0x001f));
  };

  constexpr qreal kNodeWidth = 190.0;
  constexpr qreal kNodeHeight = 116.0;
  constexpr qreal kLayerSpacing = 265.0;
  constexpr qreal kRowSpacing = 148.0;
  constexpr qreal kBinMargin = 30.0;
  std::vector<QString> ordered_ids;
  ordered_ids.reserve(nodes_.size());
  for (const auto& [id, node] : nodes_) {
    (void)node;
    ordered_ids.push_back(id);
  }
  std::sort(ordered_ids.begin(), ordered_ids.end(), [&](const QString& lhs, const QString& rhs) {
    const QString lhs_key = containment_key(lhs);
    const QString rhs_key = containment_key(rhs);
    if (lhs_key == rhs_key) {
      return layer[lhs] < layer[rhs];
    }
    return lhs_key < rhs_key;
  });

  std::map<QString, QPointF> positions;
  const qreal graph_height = static_cast<qreal>(ordered_ids.size() - 1) * kRowSpacing;
  for (size_t row = 0; row < ordered_ids.size(); ++row) {
    positions[ordered_ids[row]] =
        QPointF(layer[ordered_ids[row]] * kLayerSpacing, row * kRowSpacing - graph_height / 2.0);
  }

  std::map<QString, QRectF> node_rects;
  for (const auto& [id, node] : nodes_) {
    (void)node;
    node_rects[id] = QRectF(positions[id], QSizeF(kNodeWidth, kNodeHeight));
  }
  std::set<QString> computing_bins;
  std::function<QRectF(const QString&)> resolve_rect = [&](const QString& id) -> QRectF {
    auto node = nodes_.find(id);
    if (node == nodes_.end()) {
      return QRectF{};
    }
    if (!node->second.bin || children[id].empty()) {
      return node_rects[id];
    }
    if (computing_bins.count(id)) {
      return node_rects[id];
    }
    computing_bins.insert(id);
    QRectF bounds;
    bool have_child = false;
    for (const QString& child : children[id]) {
      const QRectF child_rect = resolve_rect(child);
      if (!child_rect.isValid()) {
        continue;
      }
      bounds = have_child ? bounds.united(child_rect) : child_rect;
      have_child = true;
    }
    computing_bins.erase(id);
    if (!have_child) {
      return node_rects[id];
    }
    bounds = bounds.adjusted(-kBinMargin, -kBinMargin - 22.0, kBinMargin, kBinMargin);
    if (bounds.width() < kNodeWidth) {
      const qreal extra = (kNodeWidth - bounds.width()) / 2.0;
      bounds.adjust(-extra, 0.0, extra, 0.0);
    }
    if (bounds.height() < kNodeHeight) {
      bounds.setHeight(kNodeHeight);
    }
    node_rects[id] = bounds;
    return bounds;
  };
  for (const auto& [id, node] : nodes_) {
    if (node.bin) {
      resolve_rect(id);
    }
  }

  std::map<QString, QStringList> source_pads_by_node;
  std::map<QString, QStringList> sink_pads_by_node;
  auto append_unique = [](QStringList* values, const QString& value) {
    if (!value.isEmpty() && !values->contains(value)) {
      values->append(value);
    }
  };
  for (const EdgeData& edge : edges_) {
    append_unique(&source_pads_by_node[edge.source], edge.source_pad);
    append_unique(&sink_pads_by_node[edge.sink], edge.sink_pad);
  }

  std::vector<QString> bin_order;
  for (const auto& [id, node] : nodes_) {
    if (node.bin) {
      bin_order.push_back(id);
    }
  }
  std::sort(bin_order.begin(), bin_order.end(), [&](const QString& lhs, const QString& rhs) {
    return node_rects[lhs].width() * node_rects[lhs].height() > node_rects[rhs].width() * node_rects[rhs].height();
  });
  for (size_t index = 0; index < bin_order.size(); ++index) {
    const QString& id = bin_order[index];
    const NodeData& node = nodes_.at(id);
    NodeVisual visual{
        .name = node.name,
        .factory = node.factory,
        .type = node.type,
        .state = node.state,
        .path = node.path,
        .source_pads = source_pads_by_node[id],
        .sink_pads = sink_pads_by_node[id],
        .bin = true,
    };
    auto* rectangle = new PipelineNodeItem(node_rects[id], std::move(visual));
    rectangle->setFlag(QGraphicsItem::ItemIsSelectable);
    rectangle->setData(kNodeIdRole, id);
    rectangle->setData(kNodeBinRole, true);
    rectangle->setToolTip(QString("%1\nBin: %2\nType: %3\nState: %4").arg(node.path, node.name, node.type, node.state));
    rectangle->setZValue(-20.0 + static_cast<qreal>(index) * 0.1);
    graph_scene_->addItem(rectangle);
    node_items_[id] = rectangle;
  }

  QPen edge_pen(QColor("#718096"), 1.4);
  auto descends_from = [&](const QString& descendant, const QString& ancestor) {
    std::set<QString> visited;
    QString current = nodes_.count(descendant) ? nodes_.at(descendant).parent_id : QString{};
    while (!current.isEmpty() && nodes_.count(current) && !visited.count(current)) {
      if (current == ancestor) {
        return true;
      }
      visited.insert(current);
      current = nodes_.at(current).parent_id;
    }
    return false;
  };
  for (const EdgeData& edge : edges_) {
    const QRectF source_rect = node_rects[edge.source];
    const QRectF sink_rect = node_rects[edge.sink];
    auto* line = graph_scene_->addLine(
        edgeLine(source_rect, sink_rect, descends_from(edge.source, edge.sink), descends_from(edge.sink, edge.source)),
        edge_pen);
    line->setZValue(-2.0);
    line->setToolTip(QString("%1 → %2").arg(edge.source_pad, edge.sink_pad));
    const QLineF direction = line->line();
    if (direction.length() > 1.0) {
      const qreal angle = std::atan2(-direction.dy(), direction.dx());
      constexpr qreal kArrow = 8.0;
      QPolygonF arrow;
      const QPointF end = direction.p2();
      arrow << end << end - QPointF(std::cos(angle - 0.45) * kArrow, -std::sin(angle - 0.45) * kArrow)
            << end - QPointF(std::cos(angle + 0.45) * kArrow, -std::sin(angle + 0.45) * kArrow);
      auto* arrow_item = graph_scene_->addPolygon(arrow, QPen(Qt::NoPen), edge_pen.color());
      arrow_item->setZValue(-1.0);
    }
  }

  for (const auto& [id, node] : nodes_) {
    if (node.bin) {
      continue;
    }
    NodeVisual visual{
        .name = node.name,
        .factory = node.factory,
        .type = node.type,
        .state = node.state,
        .path = node.path,
        .source_pads = source_pads_by_node[id],
        .sink_pads = sink_pads_by_node[id],
        .bin = false,
    };
    auto* rectangle = new PipelineNodeItem(node_rects[id], std::move(visual));
    rectangle->setFlag(QGraphicsItem::ItemIsSelectable);
    rectangle->setData(kNodeIdRole, id);
    rectangle->setData(kNodeBinRole, false);
    rectangle->setToolTip(QString("%1\nFactory: %2\nType: %3\nState: %4")
                              .arg(node.path, node.factory.isEmpty() ? "(bin)" : node.factory, node.type, node.state));
    rectangle->setZValue(0.0);
    graph_scene_->addItem(rectangle);
    node_items_[id] = rectangle;
  }
  graph_scene_->setSceneRect(graph_scene_->itemsBoundingRect().adjusted(-50, -50, 50, 50));
  graph_view_->fitInView(graph_scene_->sceneRect(), Qt::KeepAspectRatio);
}

void PipelineInspectorWidget::selectNode(const QString& node_id) {
  const auto node = nodes_.find(node_id);
  if (node == nodes_.end()) {
    return;
  }
  selected_node_id_ = node_id;
  property_table_->setRowCount(0);
  displayed_properties_.clear();
  updatePropertyEditor();
  requestProperties(node->second);
}

void PipelineInspectorWidget::showProperties(const QJsonObject& response) {
  const auto node = nodes_.find(selected_node_id_);
  if (node == nodes_.end()) {
    return;
  }
  selected_node_label_->setText(QString("%1  [%2]\n%3")
                                    .arg(
                                        node->second.name,
                                        node->second.factory.isEmpty() ? node->second.type : node->second.factory,
                                        node->second.path));
  const QSignalBlocker blocker(property_table_);
  property_table_->setRowCount(0);
  displayed_properties_.clear();
  const QJsonArray properties = response.value("properties").toArray();
  displayed_properties_.reserve(properties.size());
  for (const QJsonValue& value : properties) {
    const QJsonObject property = value.toObject();
    const int row = property_table_->rowCount();
    property_table_->insertRow(row);
    displayed_properties_.push_back(property);
    auto* name = new QTableWidgetItem(property.value("name").toString());
    name->setToolTip(property.value("description").toString());
    auto* current = new QTableWidgetItem(property.value("value").toString());
    current->setToolTip(property.value("description").toString());
    auto* type = new QTableWidgetItem(property.value("type").toString());
    auto* access = new QTableWidgetItem(propertySummary(property));
    if (property.value("editable").toBool()) {
      access->setForeground(QColor("#2ea043"));
    } else {
      access->setForeground(QColor("#8b949e"));
    }
    property_table_->setItem(row, 0, name);
    property_table_->setItem(row, 1, current);
    property_table_->setItem(row, 2, type);
    property_table_->setItem(row, 3, access);
  }
  if (property_table_->rowCount() > 0) {
    property_table_->selectRow(0);
  }
  updatePropertyEditor();
  const QString filter = property_filter_->text();
  if (!filter.isEmpty()) {
    property_filter_->setText(QString{});
    property_filter_->setText(filter);
  }
  updateStatus(QString("%1 properties loaded. Live editing is limited to plugin-declared mutable scalar values.")
                   .arg(properties.size()));
}

void PipelineInspectorWidget::updatePropertyEditor() {
  const int row = property_table_->currentRow();
  const bool valid = row >= 0 && row < static_cast<int>(displayed_properties_.size());
  const QJsonObject property = valid ? displayed_properties_[row] : QJsonObject{};
  const bool editable = valid && property.value("editable").toBool() && pending_set_request_ == 0;
  const QSignalBlocker blocker(property_editor_);
  property_editor_->clear();
  property_editor_->setEditable(true);
  if (valid) {
    const QString kind = property.value("kind").toString();
    if (kind == "toggle") {
      property_editor_->addItems({"false", "true"});
      property_editor_->setEditable(false);
    } else if (kind == "enum") {
      for (const QJsonValue& value : property.value("choices").toArray()) {
        const QJsonObject choice = value.toObject();
        property_editor_->addItem(choice.value("nick").toString(), choice.value("value").toInt());
      }
      property_editor_->setEditable(false);
    }
    const QString current = property.value("value").toString();
    const int current_index = property_editor_->findText(current);
    if (current_index >= 0) {
      property_editor_->setCurrentIndex(current_index);
    } else if (property_editor_->isEditable()) {
      property_editor_->setEditText(current);
    }
    property_editor_->setToolTip(propertySummary(property));
  }
  property_editor_->setEnabled(editable);
  apply_button_->setEnabled(editable);
}

void PipelineInspectorWidget::applySelectedProperty() {
  const int row = property_table_->currentRow();
  const auto node = nodes_.find(selected_node_id_);
  if (row < 0 || row >= static_cast<int>(displayed_properties_.size()) || node == nodes_.end()) {
    return;
  }
  const QJsonObject property = displayed_properties_[row];
  if (!property.value("editable").toBool()) {
    updateStatus("That property is read-only in the current pipeline state.", true);
    return;
  }
  const QString value = property_editor_->currentText();
  if (value.isEmpty()) {
    updateStatus("A property value is required.", true);
    return;
  }
  const uint64_t request_id = nextRequestId();
  const QByteArray command = QString("@inspect-set-property %1 %2 %3 %4 %5 %6 %7\n")
                                 .arg(request_id)
                                 .arg(session_stage_)
                                 .arg(session_generation_)
                                 .arg(node->second.app_index)
                                 .arg(QString::fromLatin1(encodeToken(node->second.path)))
                                 .arg(QString::fromLatin1(encodeToken(property.value("name").toString())))
                                 .arg(QString::fromLatin1(encodeToken(value)))
                                 .toUtf8();
  if (writeCommand(command)) {
    pending_set_request_ = request_id;
    updatePropertyEditor();
    updateStatus(QString("Applying %1 live…").arg(property.value("name").toString()));
  }
}

void PipelineInspectorWidget::selectNextSearchMatch() {
  const QString query = node_search_->text().trimmed();
  if (query.isEmpty() || nodes_.empty()) {
    return;
  }
  std::vector<QString> ids;
  for (const auto& [id, node] : nodes_) {
    if (node.name.contains(query, Qt::CaseInsensitive) || node.factory.contains(query, Qt::CaseInsensitive) ||
        node.path.contains(query, Qt::CaseInsensitive)) {
      ids.push_back(id);
    }
  }
  if (ids.empty()) {
    updateStatus(QString("No pipeline nodes match “%1”.").arg(query), true);
    return;
  }
  auto selected = std::find(ids.begin(), ids.end(), selected_node_id_);
  const QString id = selected == ids.end() || ++selected == ids.end() ? ids.front() : *selected;
  const auto item = node_items_.find(id);
  if (item != node_items_.end()) {
    graph_scene_->clearSelection();
    item->second->setSelected(true);
    graph_view_->centerOn(item->second);
  }
}

void PipelineInspectorWidget::setGraphMaximized(bool maximized) {
  if (!splitter_ || graph_maximized_ == maximized) {
    return;
  }
  graph_maximized_ = maximized;
  if (maximized) {
    const QList<int> sizes = splitter_->sizes();
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0) {
      normal_splitter_sizes_ = sizes;
    }
    const int total_width = std::max(1, splitter_->width());
    const int property_width = std::clamp(static_cast<int>(std::round(total_width * 0.28)), 300, 420);
    splitter_->setSizes({std::max(1, total_width - property_width), property_width});
  } else if (normal_splitter_sizes_.size() == 2 && normal_splitter_sizes_[0] > 0 && normal_splitter_sizes_[1] > 0) {
    splitter_->setSizes(normal_splitter_sizes_);
  } else {
    splitter_->setSizes({850, 470});
  }
  graph_maximize_button_->setIcon(graph_focus_icon(maximized));
  graph_maximize_button_->setAccessibleName(maximized ? "Restore pipeline graph" : "Maximize pipeline graph");
  graph_maximize_button_->setToolTip(
      maximized ? "Restore the normal pipeline inspector layout."
                : "Maximize the pipeline graph while keeping properties visible.");
}

void PipelineInspectorWidget::updateStatus(const QString& text, bool error) {
  status_label_->setText(text);
  status_label_->setStyleSheet(error ? "color: #ff7b72;" : "color: #aeb8c5;");
}

QString PipelineInspectorWidget::selectedNodeId() const {
  return selected_node_id_;
}

int PipelineInspectorWidget::nodeCount() const {
  return static_cast<int>(nodes_.size());
}

int PipelineInspectorWidget::edgeCount() const {
  return static_cast<int>(edges_.size());
}

bool PipelineInspectorWidget::graphMaximized() const {
  return graph_maximized_;
}
