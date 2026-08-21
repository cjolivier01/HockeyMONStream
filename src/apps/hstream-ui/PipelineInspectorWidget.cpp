#include "src/apps/hstream-ui/PipelineInspectorWidget.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QBrush>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QGraphicsPolygonItem>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsSimpleTextItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <utility>

namespace {

constexpr char kProtocolPrefix[] = "HSTREAM_PIPELINE_INSPECTOR ";
constexpr int kNodeIdRole = 1;

class PipelineGraphView : public QGraphicsView {
 public:
  explicit PipelineGraphView(QWidget* parent = nullptr) : QGraphicsView(parent) {
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  }

  void zoomBy(qreal factor) {
    const qreal current = transform().m11();
    const qreal requested = current * factor;
    if (requested < 0.08 || requested > 8.0) {
      return;
    }
    scale(factor, factor);
  }

 protected:
  void wheelEvent(QWheelEvent* event) override {
    const qreal steps = static_cast<qreal>(event->angleDelta().y()) / 120.0;
    if (steps == 0.0) {
      QGraphicsView::wheelEvent(event);
      return;
    }
    zoomBy(std::pow(1.18, steps));
    event->accept();
  }
};

QString elidedLabel(const QString& value, int maximum) {
  if (value.size() <= maximum) {
    return value;
  }
  return value.left(maximum - 1) + QChar(0x2026);
}

QColor nodeColor(bool bin, const QString& state) {
  if (bin) {
    return QColor("#274060");
  }
  if (state.compare("PLAYING", Qt::CaseInsensitive) == 0) {
    return QColor("#175c3a");
  }
  if (state.compare("PAUSED", Qt::CaseInsensitive) == 0) {
    return QColor("#725314");
  }
  return QColor("#343b46");
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

  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName("pipelineInspectorSplitter");
  splitter->setChildrenCollapsible(false);
  graph_scene_ = new QGraphicsScene(this);
  graph_view_ = new PipelineGraphView();
  graph_view_->setObjectName("pipelineInspectorGraphView");
  graph_view_->setScene(graph_scene_);
  graph_view_->setBackgroundBrush(QColor("#11151b"));
  graph_view_->setToolTip("Drag empty space to pan. Use the mouse wheel or +/− buttons to zoom.");
  splitter->addWidget(graph_view_);

  auto* properties = new QWidget();
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
  splitter->addWidget(properties);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);
  splitter->setSizes({850, 470});
  root->addWidget(splitter, 1);

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
  for (const auto& [id, node] : nodes_) {
    add_layout_edge(node.parent_id, id);
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
  std::map<int, std::vector<QString>> layers;
  for (const auto& [id, node] : nodes_) {
    (void)node;
    layers[layer[id]].push_back(id);
  }
  for (auto& [layer_index, ids] : layers) {
    (void)layer_index;
    std::sort(ids.begin(), ids.end(), [&](const QString& lhs, const QString& rhs) {
      return nodes_.at(lhs).path < nodes_.at(rhs).path;
    });
  }

  constexpr qreal kNodeWidth = 190.0;
  constexpr qreal kNodeHeight = 62.0;
  constexpr qreal kLayerSpacing = 265.0;
  constexpr qreal kRowSpacing = 92.0;
  std::map<QString, QPointF> positions;
  for (const auto& [layer_index, ids] : layers) {
    const qreal column_height = static_cast<qreal>(ids.size() - 1) * kRowSpacing;
    for (size_t row = 0; row < ids.size(); ++row) {
      positions[ids[row]] = QPointF(layer_index * kLayerSpacing, row * kRowSpacing - column_height / 2.0);
    }
  }

  QPen edge_pen(QColor("#718096"), 1.4);
  for (const EdgeData& edge : edges_) {
    const QPointF start = positions[edge.source] + QPointF(kNodeWidth, kNodeHeight / 2.0);
    const QPointF end = positions[edge.sink] + QPointF(0.0, kNodeHeight / 2.0);
    auto* line = graph_scene_->addLine(QLineF(start, end), edge_pen);
    line->setZValue(-2.0);
    line->setToolTip(QString("%1 → %2").arg(edge.source_pad, edge.sink_pad));
    const QLineF direction(start, end);
    if (direction.length() > 1.0) {
      const qreal angle = std::atan2(-direction.dy(), direction.dx());
      constexpr qreal kArrow = 8.0;
      QPolygonF arrow;
      arrow << end << end - QPointF(std::cos(angle - 0.45) * kArrow, -std::sin(angle - 0.45) * kArrow)
            << end - QPointF(std::cos(angle + 0.45) * kArrow, -std::sin(angle + 0.45) * kArrow);
      auto* arrow_item = graph_scene_->addPolygon(arrow, QPen(Qt::NoPen), edge_pen.color());
      arrow_item->setZValue(-1.0);
    }
  }
  QPen containment_pen(QColor("#3d4a5c"), 1.0, Qt::DashLine);
  for (const auto& [id, node] : nodes_) {
    if (node.parent_id.isEmpty() || !positions.count(node.parent_id)) {
      continue;
    }
    auto* line = graph_scene_->addLine(
        QLineF(
            positions[node.parent_id] + QPointF(kNodeWidth / 2.0, kNodeHeight),
            positions[id] + QPointF(kNodeWidth / 2.0, 0.0)),
        containment_pen);
    line->setZValue(-3.0);
    line->setToolTip("Bin membership");
  }

  for (const auto& [id, node] : nodes_) {
    auto* rectangle = graph_scene_->addRect(
        QRectF(positions[id], QSizeF(kNodeWidth, kNodeHeight)),
        QPen(QColor("#8ca0b8"), 1.2),
        nodeColor(node.bin, node.state));
    rectangle->setFlag(QGraphicsItem::ItemIsSelectable);
    rectangle->setData(kNodeIdRole, id);
    rectangle->setToolTip(QString("%1\nFactory: %2\nType: %3\nState: %4")
                              .arg(node.path, node.factory.isEmpty() ? "(bin)" : node.factory, node.type, node.state));
    auto* title = new QGraphicsSimpleTextItem(elidedLabel(node.name, 25), rectangle);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    title->setBrush(Qt::white);
    title->setPos(8.0, 6.0);
    title->setAcceptedMouseButtons(Qt::NoButton);
    const QString subtitle_text = node.factory.isEmpty() ? node.type : node.factory;
    auto* subtitle = new QGraphicsSimpleTextItem(elidedLabel(subtitle_text, 27), rectangle);
    subtitle->setBrush(QColor("#d5dde8"));
    subtitle->setPos(8.0, 28.0);
    subtitle->setAcceptedMouseButtons(Qt::NoButton);
    auto* state = new QGraphicsSimpleTextItem(node.state, rectangle);
    state->setBrush(QColor("#b7f7d1"));
    state->setPos(kNodeWidth - state->boundingRect().width() - 7.0, 44.0);
    state->setAcceptedMouseButtons(Qt::NoButton);
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
