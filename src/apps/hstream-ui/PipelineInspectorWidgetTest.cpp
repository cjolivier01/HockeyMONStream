#include "src/apps/hstream-ui/PipelineInspectorWidget.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

template <typename Widget>
Widget* require_child(PipelineInspectorWidget* inspector, const char* name) {
  Widget* child = inspector->findChild<Widget*>(name);
  if (!child) {
    std::cerr << "FAIL: missing widget " << name << '\n';
  }
  return child;
}

QGraphicsItem* top_level_item_at(QGraphicsView* graph_view, const QPoint& point) {
  QGraphicsItem* item = graph_view->itemAt(point);
  while (item && item->parentItem()) {
    item = item->parentItem();
  }
  return item;
}

QPoint find_bin_background_point(
    QGraphicsView* graph_view,
    QGraphicsRectItem* bin_item,
    const QList<QGraphicsRectItem*>& child_items) {
  graph_view->centerOn(bin_item);
  QApplication::processEvents();
  const QRectF bin_rect = bin_item->sceneBoundingRect();
  QList<QRectF> child_rects;
  for (QGraphicsRectItem* child : child_items) {
    if (child) {
      child_rects.push_back(child->sceneBoundingRect().adjusted(-3.0, -3.0, 3.0, 3.0));
    }
  }
  for (qreal y = bin_rect.top() + 8.0; y < bin_rect.bottom() - 8.0; y += 12.0) {
    for (qreal x = bin_rect.left() + 8.0; x < bin_rect.right() - 8.0; x += 12.0) {
      const QPointF scene_point(x, y);
      bool overlaps_child = false;
      for (const QRectF& child_rect : child_rects) {
        overlaps_child = overlaps_child || child_rect.contains(scene_point);
      }
      const QPoint view_point = graph_view->mapFromScene(scene_point);
      if (!overlaps_child && graph_view->viewport()->rect().contains(view_point) &&
          top_level_item_at(graph_view, view_point) == bin_item) {
        return view_point;
      }
    }
  }
  return QPoint(-1, -1);
}

void send_mouse(
    QGraphicsView* view,
    QEvent::Type type,
    const QPoint& point,
    Qt::MouseButton button,
    Qt::MouseButtons buttons) {
  QMouseEvent event(type, point, view->viewport()->mapToGlobal(point), button, buttons, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &event);
}

void click_graph(QGraphicsView* view, const QPoint& point) {
  send_mouse(view, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
  send_mouse(view, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
}

void send_wheel(QGraphicsView* view, const QPoint& point, int angle, int pixels = 0) {
  QWheelEvent event(
      point,
      view->viewport()->mapToGlobal(point),
      QPoint(0, pixels),
      QPoint(0, angle),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::NoScrollPhase,
      false);
  QApplication::sendEvent(view->viewport(), &event);
  QApplication::processEvents();
}

void send_key(QWidget* widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);
  QApplication::sendEvent(widget, &event);
  QApplication::processEvents();
}

int selection_pixel_count(QGraphicsView* view) {
  const QImage image = view->viewport()->grab().toImage();
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.red() < 85 && pixel.green() > 155 && pixel.blue() > 220) {
        ++count;
      }
    }
  }
  return count;
}

bool test_large_graph_navigation() {
  PipelineInspectorWidget inspector;
  inspector.resize(1300, 800);
  inspector.show();
  inspector.activateWindow();
  QApplication::processEvents();
  QByteArray last_command;
  inspector.setCommandWriter([&last_command](const QByteArray& command) {
    last_command = command;
    return true;
  });
  inspector.setPipelineRunning(true);
  inspector.handleBackendLine(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,"
      "\"status\":\"ok\",\"stage\":0,\"generation\":1}");
  QJsonArray nodes;
  nodes.append(QJsonObject{{"id", "bin"}, {"path", "bin"}, {"name", "pipeline"}, {"bin", true}});
  for (int index = 0; index < 150; ++index) {
    const QString name = QString("node%1").arg(index, 3, 10, QChar('0'));
    nodes.append(
        QJsonObject{
            {"id", name},
            {"path", name},
            {"parentId", "bin"},
            {"name", name},
            {"factory", "identity"},
            {"bin", false}});
  }
  auto graph_response = [&]() {
    const QJsonObject response{
        {"version", 1},
        {"kind", "graph"},
        {"requestId", last_command.split(' ').at(1).trimmed().toInt()},
        {"status", "ok"},
        {"stage", 0},
        {"generation", 1},
        {"nodes", nodes}};
    return inspector.handleBackendLine(
        "HSTREAM_PIPELINE_INSPECTOR " + QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)));
  };
  bool ok = expect(graph_response(), "large graph must load");
  auto* view = require_child<QGraphicsView>(&inspector, "pipelineInspectorGraphView");
  auto* fit = require_child<QPushButton>(&inspector, "pipelineInspectorFitButton");
  auto* zoom_in = require_child<QPushButton>(&inspector, "pipelineInspectorZoomInButton");
  auto* actual = require_child<QPushButton>(&inspector, "pipelineInspectorActualSizeButton");
  auto* focus = require_child<QPushButton>(&inspector, "pipelineInspectorFocusSelectionButton");
  auto* zoom_label = require_child<QLabel>(&inspector, "pipelineInspectorZoomLabel");
  auto* selected_label = require_child<QLabel>(&inspector, "pipelineInspectorSelectedNode");
  auto* search = require_child<QLineEdit>(&inspector, "pipelineInspectorNodeSearch");
  if (!view || !fit || !zoom_in || !actual || !focus || !zoom_label || !selected_label || !search) {
    return false;
  }
  const qreal overview = view->transform().m11();
  ok &= expect(overview < 0.08, "large graph must reproduce a fit below the old 8% zoom limit");
  ok &= expect(!focus->isEnabled(), "focus must be disabled without a selection");
  const QPoint center = view->viewport()->rect().center();
  send_wheel(view, center, 120);
  ok &= expect(
      view->transform().m11() > overview && view->transform().m11() < 0.08,
      "wheel must zoom incrementally out of a very small fitted overview");
  send_wheel(view, center, -120);
  ok &= expect(std::abs(view->transform().m11() - overview) < 0.001, "reverse wheel must return to overview");
  zoom_in->click();
  ok &= expect(view->transform().m11() > overview, "toolbar zoom must escape the old minimum too");
  const qreal before_pixels = view->transform().m11();
  send_wheel(view, center, 0, 30);
  ok &= expect(view->transform().m11() > before_pixels, "pixel-only trackpad wheel must zoom");
  send_wheel(view, center, 12000);
  ok &= expect(std::abs(view->transform().m11() - 8.0) < 0.001, "large wheel deltas must clamp to maximum");
  send_wheel(view, center, -120);
  ok &= expect(view->transform().m11() < 8.0, "zoom must reverse from the maximum");
  actual->click();
  ok &= expect(
      std::abs(view->transform().m11() - 1.0) < 0.001 && zoom_label->text() == "100%",
      "actual size must reset the transform and zoom readout");

  QGraphicsItem* node = nullptr;
  QGraphicsItem* bin = nullptr;
  for (QGraphicsItem* item : view->scene()->items()) {
    if (item->data(1).toString() == "node075")
      node = item;
    if (item->data(1).toString() == "bin")
      bin = item;
  }
  if (!node || !bin)
    return false;
  view->centerOn(node);
  QApplication::processEvents();
  const QPoint anchor = view->viewport()->rect().center() + QPoint(0, 100);
  const QPointF before_anchor = view->mapToScene(anchor);
  send_wheel(view, anchor, 120);
  ok &= expect(
      QLineF(view->mapFromScene(before_anchor), anchor).length() <= 3.0,
      "wheel zoom must keep the scene point under the actual event position");
  click_graph(view, view->mapFromScene(node->sceneBoundingRect().center()));
  ok &= expect(
      inspector.selectedNodeId() == "node075" && selected_label->text().contains("node075") && focus->isEnabled(),
      "click must immediately identify the node before properties arrive");
  focus->click();
  ok &= expect(view->transform().m11() >= 1.0, "focus selected must make the node readable");
  const QPoint drag_start = view->mapFromScene(node->sceneBoundingRect().center());
  const int before_pan = view->verticalScrollBar()->value();
  send_mouse(view, QEvent::MouseButtonPress, drag_start, Qt::MiddleButton, Qt::MiddleButton);
  send_mouse(view, QEvent::MouseMove, drag_start + QPoint(0, 50), Qt::NoButton, Qt::MiddleButton);
  send_mouse(view, QEvent::MouseButtonRelease, drag_start + QPoint(0, 50), Qt::MiddleButton, Qt::NoButton);
  ok &= expect(
      view->verticalScrollBar()->value() != before_pan && inspector.selectedNodeId() == "node075",
      "middle-drag over a node must pan while preserving selection");
  const qreal refresh_zoom = view->transform().m11();
  const QPointF refresh_center = view->mapToScene(view->viewport()->rect().center());
  inspector.requestRefresh();
  ok &= expect(graph_response(), "refresh must load");
  ok &= expect(
      inspector.selectedNodeId() == "node075" && view->transform().m11() == refresh_zoom &&
          QLineF(refresh_center, view->mapToScene(view->viewport()->rect().center())).length() < 2.0,
      "same-session refresh must preserve selection, zoom, and position");
  fit->click();
  ok &= expect(selection_pixel_count(view) >= 6, "selected node must have visible colored pixels at overview scale");
  send_key(view, Qt::Key_Escape);
  ok &= expect(inspector.selectedNodeId().isEmpty() && !focus->isEnabled(), "Escape must clear selection");
  // Reacquire items after the graph refresh.
  for (QGraphicsItem* item : view->scene()->items()) {
    if (item->data(1).toString() == "bin")
      bin = item;
  }
  send_wheel(view, view->viewport()->rect().center(), 600);
  view->centerOn(bin->sceneBoundingRect().topLeft());
  QApplication::processEvents();
  const QPoint bin_point = view->mapFromScene(bin->sceneBoundingRect().topLeft() + QPointF(120, 20));
  click_graph(view, bin_point);
  ok &= expect(
      inspector.selectedNodeId() == "bin" && selection_pixel_count(view) > 300,
      "selected bin must have a prominent outline at overview scale");
  search->setText("node075");
  send_key(search, Qt::Key_Return);
  ok &= expect(
      inspector.selectedNodeId() == "node075" && view->transform().m11() >= 1.0,
      "search must select and zoom to a readable match");
  send_key(view, Qt::Key_F);
  ok &= expect(view->transform().m11() < 0.08, "F must restore whole-graph overview");
  send_key(view, Qt::Key_S);
  ok &= expect(view->transform().m11() >= 1.0, "S must focus the selection");
  send_key(view, Qt::Key_1);
  ok &= expect(zoom_label->text() == "100%", "1 must reset zoom");
  send_key(view, Qt::Key_Plus);
  ok &= expect(view->transform().m11() > 1.0, "keyboard plus must zoom in");
  send_key(view, Qt::Key_Home);
  for (QGraphicsItem* item : view->scene()->items()) {
    if (item->data(1).toString() == "node075")
      node = item;
  }
  send_mouse(
      view,
      QEvent::MouseButtonDblClick,
      view->mapFromScene(node->sceneBoundingRect().center()),
      Qt::LeftButton,
      Qt::LeftButton);
  send_mouse(view, QEvent::MouseButtonRelease, view->viewport()->rect().center(), Qt::LeftButton, Qt::NoButton);
  ok &= expect(view->transform().m11() >= 1.0, "double-click must focus a node");
  view->setFocus();
  send_key(view, Qt::Key_F, Qt::ControlModifier);
  ok &= expect(search->hasFocus(), "Ctrl+F must focus node search");
  return ok;
}

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  PipelineInspectorWidget inspector;
  inspector.resize(1300, 800);
  inspector.show();
  QApplication::processEvents();
  std::vector<QByteArray> commands;
  inspector.setCommandWriter([&commands](const QByteArray& command) {
    commands.push_back(command);
    return true;
  });
  inspector.setPipelineRunning(true);
  inspector.requestRefresh();
  bool ok = expect(commands.empty(), "inspector must not issue unbound requests before the session is announced");

  const QString initial_session = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
      "\"stage\":-1,\"generation\":7}");
  ok &= expect(inspector.handleBackendLine(initial_session), "session response must be consumed");
  ok &= expect(
      commands.size() == 1 && commands.back() == "@inspect-pipeline 1 -1 7\n",
      "session-bound refresh command mismatch");

  const QString graph = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"graph\",\"requestId\":1,\"status\":\"ok\","
      "\"stage\":-1,\"generation\":7,\"nodes\":["
      "{\"id\":\"0:8:pipeline\",\"appIndex\":0,\"path\":\"8:pipeline\",\"parentId\":\"\","
      "\"name\":\"pipeline\",\"factory\":\"pipeline\",\"type\":\"GstPipeline\",\"state\":\"PLAYING\","
      "\"bin\":true},"
      "{\"id\":\"0:8:pipeline/6:source\",\"appIndex\":0,\"path\":\"8:pipeline/6:source\","
      "\"parentId\":\"0:8:pipeline\",\"name\":\"source\",\"factory\":\"videotestsrc\","
      "\"type\":\"GstVideoTestSrc\",\"state\":\"PLAYING\",\"bin\":false},"
      "{\"id\":\"0:8:pipeline/6:filter\",\"appIndex\":0,\"path\":\"8:pipeline/6:filter\","
      "\"parentId\":\"0:8:pipeline\",\"name\":\"filter\",\"factory\":\"capsfilter\","
      "\"type\":\"GstCapsFilter\",\"state\":\"PLAYING\",\"bin\":false},"
      "{\"id\":\"0:8:pipeline/6:middle\",\"appIndex\":0,\"path\":\"8:pipeline/6:middle\","
      "\"parentId\":\"0:8:pipeline\",\"name\":\"middle\",\"factory\":\"queue\","
      "\"type\":\"GstQueue\",\"state\":\"PLAYING\",\"bin\":false},"
      "{\"id\":\"0:7:outside\",\"appIndex\":0,\"path\":\"7:outside\",\"parentId\":\"\","
      "\"name\":\"outside\",\"factory\":\"queue\",\"type\":\"GstQueue\",\"state\":\"PLAYING\",\"bin\":false},"
      "{\"id\":\"1:8:pipeline\",\"appIndex\":1,\"path\":\"8:pipeline\",\"parentId\":\"\","
      "\"name\":\"pipeline\",\"factory\":\"pipeline\",\"type\":\"GstPipeline\",\"state\":\"PLAYING\","
      "\"bin\":true},"
      "{\"id\":\"1:8:pipeline/6:source\",\"appIndex\":1,\"path\":\"8:pipeline/6:source\","
      "\"parentId\":\"1:8:pipeline\",\"name\":\"source\",\"factory\":\"videotestsrc\","
      "\"type\":\"GstVideoTestSrc\",\"state\":\"PLAYING\",\"bin\":false}],"
      "\"edges\":[{\"source\":\"0:8:pipeline/6:source\",\"sourcePad\":\"src\","
      "\"sink\":\"0:8:pipeline\",\"sinkPad\":\"sink\"},"
      "{\"source\":\"0:8:pipeline/6:filter\",\"sourcePad\":\"src2\","
      "\"sink\":\"0:8:pipeline\",\"sinkPad\":\"sink2\"},"
      "{\"source\":\"0:8:pipeline/6:middle\",\"sourcePad\":\"src3\","
      "\"sink\":\"0:8:pipeline\",\"sinkPad\":\"sink3\"},"
      "{\"source\":\"1:8:pipeline/6:source\",\"sourcePad\":\"src\","
      "\"sink\":\"1:8:pipeline\",\"sinkPad\":\"sink\"}]}");
  ok &= expect(inspector.handleBackendLine(graph), "graph response must be consumed");
  ok &= expect(inspector.nodeCount() == 7 && inspector.edgeCount() == 4, "graph counts mismatch");

  auto* view = require_child<QGraphicsView>(&inspector, "pipelineInspectorGraphView");
  auto* splitter = require_child<QSplitter>(&inspector, "pipelineInspectorSplitter");
  auto* properties_pane = require_child<QWidget>(&inspector, "pipelineInspectorPropertiesPane");
  auto* maximize = require_child<QToolButton>(&inspector, "pipelineInspectorMaximizeButton");
  auto* status = require_child<QLabel>(&inspector, "pipelineInspectorStatus");
  QGraphicsRectItem* source_item = nullptr;
  QGraphicsRectItem* filter_item = nullptr;
  QGraphicsRectItem* middle_item = nullptr;
  QGraphicsRectItem* outside_item = nullptr;
  QGraphicsRectItem* pipeline_item = nullptr;
  QGraphicsRectItem* second_app_source_item = nullptr;
  QGraphicsRectItem* second_app_pipeline_item = nullptr;
  QGraphicsLineItem* source_to_pipeline_edge = nullptr;
  QGraphicsLineItem* filter_to_pipeline_edge = nullptr;
  QGraphicsLineItem* middle_to_pipeline_edge = nullptr;
  if (view) {
    for (QGraphicsItem* item : view->scene()->items()) {
      auto* rectangle = dynamic_cast<QGraphicsRectItem*>(item);
      if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:source") {
        source_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:filter") {
        filter_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:middle") {
        middle_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:7:outside") {
        outside_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline") {
        pipeline_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "1:8:pipeline/6:source") {
        second_app_source_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "1:8:pipeline") {
        second_app_pipeline_item = rectangle;
      }
      auto* line = dynamic_cast<QGraphicsLineItem*>(item);
      if (line && line->toolTip() == "src → sink") {
        source_to_pipeline_edge = line;
      } else if (line && line->toolTip() == "src2 → sink2") {
        filter_to_pipeline_edge = line;
      } else if (line && line->toolTip() == "src3 → sink3") {
        middle_to_pipeline_edge = line;
      }
    }
  }
  if (view) {
    const qreal zoom_before = view->transform().m11();
    QWheelEvent wheel_in(
        view->viewport()->rect().center(),
        view->viewport()->mapToGlobal(view->viewport()->rect().center()),
        QPoint(),
        QPoint(0, 120),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(view->viewport(), &wheel_in);
    ok &= expect(view->transform().m11() > zoom_before, "mouse wheel must zoom the graph view in around the pointer");
  }
  if (splitter && properties_pane && maximize) {
    const QList<int> normal_sizes = splitter->sizes();
    maximize->click();
    QApplication::processEvents();
    const QList<int> maximized_sizes = splitter->sizes();
    ok &= expect(inspector.graphMaximized(), "maximize button must enter graph-maximized mode");
    ok &= expect(
        properties_pane->isVisible() && properties_pane->width() >= properties_pane->minimumWidth(),
        "graph-maximized mode must keep the property pane visible and usable");
    ok &= expect(
        maximized_sizes.size() == 2 && normal_sizes.size() == 2 && maximized_sizes[0] > normal_sizes[0],
        "graph-maximized mode must allocate more space to the graph pane");
    maximize->click();
    QApplication::processEvents();
    ok &= expect(!inspector.graphMaximized(), "restore button must leave graph-maximized mode");
  }
  ok &= expect(source_item != nullptr, "source graph node must be selectable");
  ok &= expect(filter_item != nullptr, "second child graph node must be rendered");
  ok &= expect(middle_item != nullptr, "middle child graph node must be rendered");
  ok &= expect(outside_item != nullptr, "unrelated graph node must be rendered");
  ok &= expect(pipeline_item != nullptr, "pipeline bin must be rendered as a selectable container");
  ok &= expect(second_app_source_item != nullptr, "second app child graph node must be rendered");
  ok &= expect(second_app_pipeline_item != nullptr, "second app pipeline bin must be rendered");
  ok &= expect(
      source_item && pipeline_item && pipeline_item->rect().contains(source_item->rect()),
      "pipeline bin container must surround the source child node");
  ok &= expect(
      filter_item && pipeline_item && pipeline_item->rect().contains(filter_item->rect()),
      "pipeline bin container must surround the filter child node");
  ok &= expect(
      middle_item && pipeline_item && pipeline_item->rect().contains(middle_item->rect()),
      "pipeline bin container must surround the middle child node");
  ok &= expect(
      outside_item && pipeline_item && !pipeline_item->rect().intersects(outside_item->rect()),
      "pipeline bin container must not surround unrelated nodes");
  ok &= expect(
      second_app_source_item && pipeline_item && !pipeline_item->rect().intersects(second_app_source_item->rect()),
      "pipeline bin container must not surround repeated-topology nodes from another app");
  ok &= expect(
      second_app_pipeline_item && source_item && !second_app_pipeline_item->rect().intersects(source_item->rect()),
      "second app bin container must not surround same-path nodes from the first app");
  ok &= expect(source_to_pipeline_edge != nullptr, "source-to-bin pad edge must be rendered");
  ok &= expect(filter_to_pipeline_edge != nullptr, "second child-to-bin pad edge must be rendered");
  ok &= expect(middle_to_pipeline_edge != nullptr, "middle child-to-bin pad edge must be rendered");
  if (source_item && filter_item && middle_item && pipeline_item && source_to_pipeline_edge &&
      filter_to_pipeline_edge && middle_to_pipeline_edge) {
    auto expect_contained_edge = [&](const QLineF& edge_line, const QGraphicsRectItem* child) {
      ok &= expect(
          std::abs(edge_line.y1() - child->rect().center().y()) < 0.01 &&
              std::abs(edge_line.y2() - child->rect().center().y()) < 0.01,
          "contained-bin edge must route horizontally through the child row");
      ok &= expect(
          pipeline_item->rect().contains(edge_line.p2()) &&
              (edge_line.x2() < child->rect().left() || edge_line.x2() > child->rect().right()),
          "contained-bin edge must terminate at a side lane outside the child node");
    };
    expect_contained_edge(source_to_pipeline_edge->line(), source_item);
    expect_contained_edge(filter_to_pipeline_edge->line(), filter_item);
    expect_contained_edge(middle_to_pipeline_edge->line(), middle_item);
    ok &= expect(
        view->scene()->itemAt(source_item->rect().center(), QTransform()) == source_item,
        "child node must remain the top hit-test target inside its containing bin");
  }
  if (source_item) {
    source_item->setSelected(true);
    QApplication::processEvents();
  }
  ok &= expect(inspector.selectedNodeId() == "0:8:pipeline/6:source", "selected node id mismatch");
  ok &= expect(
      commands.size() == 2 && commands.back() == "@inspect-properties 2 -1 7 0 ODpwaXBlbGluZS82OnNvdXJjZQ==\n",
      "property request must carry canonical base64 path");

  const QString properties = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"properties\",\"requestId\":2,"
      "\"status\":\"ok\",\"stage\":-1,\"generation\":7,\"nodeId\":\"0:8:pipeline/6:source\","
      "\"appIndex\":0,\"path\":\"8:pipeline/6:source\",\"properties\":["
      "{\"name\":\"is-live\",\"label\":\"Is live\",\"description\":\"Act as a live source\","
      "\"type\":\"gboolean\",\"kind\":\"toggle\",\"applyMode\":\"playing\",\"value\":\"false\","
      "\"default\":\"false\",\"minimum\":\"\",\"maximum\":\"\",\"readable\":true,"
      "\"writable\":true,\"editable\":true,\"secret\":false,\"editReason\":\"Live edit\",\"choices\":[]},"
      "{\"name\":\"location\",\"label\":\"Location\",\"description\":\"Sensitive URI\","
      "\"type\":\"gchararray\",\"kind\":\"text\",\"applyMode\":\"playing\",\"value\":\"[redacted]\","
      "\"default\":\"\",\"minimum\":\"\",\"maximum\":\"\",\"readable\":true,\"writable\":true,"
      "\"editable\":false,\"secret\":true,\"editReason\":\"Sensitive value is read-only\",\"choices\":[]}]}");
  ok &= expect(inspector.handleBackendLine(properties), "property response must be consumed");
  auto* table = require_child<QTableWidget>(&inspector, "pipelineInspectorPropertyTable");
  auto* editor = require_child<QComboBox>(&inspector, "pipelineInspectorPropertyEditor");
  auto* apply = require_child<QPushButton>(&inspector, "pipelineInspectorApplyButton");
  ok &= expect(table && table->rowCount() == 2, "selected element properties must populate the table");
  if (table) {
    table->selectRow(0);
    QApplication::processEvents();
  }
  ok &= expect(editor && editor->isEnabled() && apply && apply->isEnabled(), "live boolean must enable safe editor");
  if (editor) {
    editor->setCurrentText("true");
  }
  if (apply) {
    apply->click();
  }
  ok &= expect(
      commands.size() == 3 &&
          commands.back() == "@inspect-set-property 3 -1 7 0 ODpwaXBlbGluZS82OnNvdXJjZQ== aXMtbGl2ZQ== dHJ1ZQ==\n",
      "live property command must encode path, name, and value");

  const QString set_result = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":3,"
      "\"status\":\"ok\",\"stage\":-1,\"generation\":7,\"nodeId\":\"0:8:pipeline/6:source\","
      "\"appIndex\":0,\"path\":\"8:pipeline/6:source\",\"property\":\"is-live\"}");
  ok &= expect(inspector.handleBackendLine(set_result), "set result must be consumed");
  ok &= expect(
      commands.size() == 4 && commands.back() == "@inspect-properties 4 -1 7 0 ODpwaXBlbGluZS82OnNvdXJjZQ==\n",
      "successful mutation must read the value back");

  const QString next_session = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
      "\"stage\":0,\"generation\":8}");
  ok &= expect(inspector.handleBackendLine(next_session), "new stage session must be consumed");
  ok &= expect(
      inspector.nodeCount() == 0 && inspector.selectedNodeId().isEmpty(),
      "a stage/generation change must clear stale graph and selection state");
  ok &= expect(
      commands.size() == 5 && commands.back() == "@inspect-pipeline 5 0 8\n",
      "a stage/generation change must refresh with the new binding");
  ok &= expect(
      inspector.handleBackendLine(properties) && inspector.nodeCount() == 0,
      "a delayed property response from the previous stage must remain ignored");
  const QString stale_error = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"properties\",\"requestId\":4,\"status\":\"error\","
      "\"stage\":-1,\"generation\":7,\"message\":\"Stale pipeline inspector stage/generation\"}");
  ok &= expect(inspector.handleBackendLine(stale_error), "old-session errors must be consumed");
  QString current_graph = graph;
  current_graph.replace("\"requestId\":1", "\"requestId\":5");
  current_graph.replace("\"stage\":-1,\"generation\":7", "\"stage\":0,\"generation\":8");
  ok &= expect(
      inspector.handleBackendLine(current_graph) && inspector.nodeCount() == 7,
      "an old-session error must not cancel the current bound graph refresh");
  QGraphicsRectItem* current_source_item = nullptr;
  QGraphicsRectItem* current_filter_item = nullptr;
  QGraphicsRectItem* current_middle_item = nullptr;
  QGraphicsRectItem* current_pipeline_item = nullptr;
  if (view) {
    for (QGraphicsItem* item : view->scene()->items()) {
      auto* rectangle = dynamic_cast<QGraphicsRectItem*>(item);
      if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:source") {
        current_source_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:filter") {
        current_filter_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline/6:middle") {
        current_middle_item = rectangle;
      } else if (rectangle && rectangle->data(1).toString() == "0:8:pipeline") {
        current_pipeline_item = rectangle;
      }
    }
  }
  if (view && current_source_item) {
    current_source_item->setSelected(true);
    QApplication::processEvents();
    ok &= expect(inspector.selectedNodeId() == "0:8:pipeline/6:source", "reselected node id mismatch");
    QPoint empty_view_point(8, view->viewport()->height() - 8);
    for (int y = view->viewport()->height() - 8; y >= 8; y -= 20) {
      for (int x = 8; x < view->viewport()->width(); x += 20) {
        const QPoint candidate(x, y);
        if (!view->itemAt(candidate)) {
          empty_view_point = candidate;
          break;
        }
      }
      if (!view->itemAt(empty_view_point)) {
        break;
      }
    }
    QMouseEvent empty_press(
        QEvent::MouseButtonPress,
        empty_view_point,
        view->viewport()->mapToGlobal(empty_view_point),
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &empty_press);
    QMouseEvent empty_release(
        QEvent::MouseButtonRelease,
        empty_view_point,
        view->viewport()->mapToGlobal(empty_view_point),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &empty_release);
    ok &= expect(inspector.selectedNodeId().isEmpty(), "empty-space pan/click must clear selected node state");
    QString delayed_current_properties = properties;
    delayed_current_properties.replace("\"requestId\":2", "\"requestId\":6");
    delayed_current_properties.replace("\"stage\":-1,\"generation\":7", "\"stage\":0,\"generation\":8");
    ok &= expect(
        inspector.handleBackendLine(delayed_current_properties),
        "delayed cleared-selection properties must be consumed");
    ok &= expect(table && table->rowCount() == 0, "delayed cleared-selection properties must not repopulate the table");
    ok &= expect(
        editor && !editor->isEnabled() && apply && !apply->isEnabled(),
        "cleared selection must keep the property editor disabled after delayed responses");
    const QString delayed_current_error = QStringLiteral(
        "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"properties\",\"requestId\":6,\"status\":\"error\","
        "\"stage\":0,\"generation\":8,\"message\":\"Synthetic stale same-session property error\"}");
    ok &= expect(
        inspector.handleBackendLine(delayed_current_error),
        "delayed cleared-selection property errors must be consumed");
    ok &= expect(
        status && !status->text().contains("Synthetic stale same-session property error"),
        "delayed cleared-selection property errors must not overwrite current UI status");
    const QString delayed_set_error = QStringLiteral(
        "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":7,\"status\":\"error\","
        "\"stage\":0,\"generation\":8,\"message\":\"Synthetic stale same-session set error\"}");
    ok &=
        expect(inspector.handleBackendLine(delayed_set_error), "delayed cleared-selection set errors must be consumed");
    ok &= expect(
        status && !status->text().contains("Synthetic stale same-session set error"),
        "delayed cleared-selection set errors must not overwrite current UI status");
  }
  if (view && current_pipeline_item && current_source_item && current_filter_item && current_middle_item) {
    while (view->horizontalScrollBar()->maximum() == view->horizontalScrollBar()->minimum() &&
           view->transform().m11() < 6.0) {
      view->scale(1.5, 1.5);
      QApplication::processEvents();
    }
    const QPoint bin_point = find_bin_background_point(
        view, current_pipeline_item, {current_source_item, current_filter_item, current_middle_item});
    ok &= expect(bin_point.x() >= 0, "test must find a visible bin background point");
    if (bin_point.x() >= 0) {
      const size_t commands_before_drag = commands.size();
      const int horizontal_before_drag = view->horizontalScrollBar()->value();
      const int drag_x = view->horizontalScrollBar()->value() < view->horizontalScrollBar()->maximum()
          ? -(QApplication::startDragDistance() + 30)
          : QApplication::startDragDistance() + 30;
      QMouseEvent bin_drag_press(
          QEvent::MouseButtonPress,
          bin_point,
          view->viewport()->mapToGlobal(bin_point),
          Qt::LeftButton,
          Qt::LeftButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &bin_drag_press);
      const QPoint drag_point = bin_point + QPoint(drag_x, 0);
      QMouseEvent bin_drag_move(
          QEvent::MouseMove,
          drag_point,
          view->viewport()->mapToGlobal(drag_point),
          Qt::NoButton,
          Qt::LeftButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &bin_drag_move);
      QMouseEvent bin_drag_release(
          QEvent::MouseButtonRelease,
          drag_point,
          view->viewport()->mapToGlobal(drag_point),
          Qt::LeftButton,
          Qt::NoButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &bin_drag_release);
      ok &= expect(commands.size() == commands_before_drag, "bin background drag must not request bin properties");
      ok &= expect(
          view->horizontalScrollBar()->value() != horizontal_before_drag,
          "bin background drag must apply the threshold-crossing pan delta");
      ok &= expect(
          inspector.selectedNodeId().isEmpty(), "bin background drag must pan instead of leaving the bin selected");

      const QPoint click_bin_point = find_bin_background_point(
          view, current_pipeline_item, {current_source_item, current_filter_item, current_middle_item});
      ok &= expect(click_bin_point.x() >= 0, "test must find a visible bin background point after panning");
      QMouseEvent bin_press(
          QEvent::MouseButtonPress,
          click_bin_point,
          view->viewport()->mapToGlobal(click_bin_point),
          Qt::LeftButton,
          Qt::LeftButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &bin_press);
      QMouseEvent bin_release(
          QEvent::MouseButtonRelease,
          click_bin_point,
          view->viewport()->mapToGlobal(click_bin_point),
          Qt::LeftButton,
          Qt::NoButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &bin_release);
      ok &= expect(inspector.selectedNodeId() == "0:8:pipeline", "bin background click must select the bin");

      const QPoint refresh_press_point = find_bin_background_point(
          view, current_pipeline_item, {current_source_item, current_filter_item, current_middle_item});
      ok &= expect(refresh_press_point.x() >= 0, "test must find a bin point before refresh-during-click");
      QMouseEvent refresh_bin_press(
          QEvent::MouseButtonPress,
          refresh_press_point,
          view->viewport()->mapToGlobal(refresh_press_point),
          Qt::LeftButton,
          Qt::LeftButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &refresh_bin_press);
      inspector.requestRefresh();
      ok &= expect(
          !commands.empty() && commands.back() == "@inspect-pipeline 8 0 8\n",
          "explicit refresh during pending bin click must issue the expected graph request");
      QString refreshed_graph = current_graph;
      refreshed_graph.replace("\"requestId\":5", "\"requestId\":8");
      ok &= expect(
          inspector.handleBackendLine(refreshed_graph), "graph refresh during pending bin click must be consumed");
      QMouseEvent refresh_bin_release(
          QEvent::MouseButtonRelease,
          refresh_press_point,
          view->viewport()->mapToGlobal(refresh_press_point),
          Qt::LeftButton,
          Qt::NoButton,
          Qt::NoModifier);
      QApplication::sendEvent(view->viewport(), &refresh_bin_release);
      ok &= expect(
          inspector.selectedNodeId() == "0:8:pipeline",
          "bin click release after graph refresh must resolve the current scene item");
    }
  }

  ok &= expect(
      inspector.handleBackendLine("HSTREAM_PIPELINE_INSPECTOR {not-json") && status &&
          status->text().contains("Malformed"),
      "malformed protocol lines must be contained and reported");
  inspector.setPipelineRunning(false);
  ok &= expect(inspector.nodeCount() == 0, "stopped inspector must clear stale live topology");
  ok &= test_large_graph_navigation();
  return ok ? 0 : 1;
}
