#include "src/apps/hstream-ui/PipelineInspectorWidget.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

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

} // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  PipelineInspectorWidget inspector;
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

  auto* status = require_child<QLabel>(&inspector, "pipelineInspectorStatus");
  ok &= expect(
      inspector.handleBackendLine("HSTREAM_PIPELINE_INSPECTOR {not-json") && status &&
          status->text().contains("Malformed"),
      "malformed protocol lines must be contained and reported");
  inspector.setPipelineRunning(false);
  ok &= expect(inspector.nodeCount() == 0, "stopped inspector must clear stale live topology");
  return ok ? 0 : 1;
}
