#include "src/apps/hstream-ui/PipelineInspectorWidget.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGraphicsRectItem>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

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
  bool ok = expect(commands.size() == 1 && commands.back() == "@inspect-pipeline 1\n", "refresh command mismatch");

  const QString graph = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"graph\",\"requestId\":1,\"status\":\"ok\","
      "\"stage\":0,\"nodes\":["
      "{\"id\":\"0:pipeline\",\"appIndex\":0,\"path\":\"pipeline\",\"parentId\":\"\","
      "\"name\":\"pipeline\",\"factory\":\"pipeline\",\"type\":\"GstPipeline\",\"state\":\"PLAYING\","
      "\"bin\":true},"
      "{\"id\":\"0:pipeline.source\",\"appIndex\":0,\"path\":\"pipeline.source\","
      "\"parentId\":\"0:pipeline\",\"name\":\"source\",\"factory\":\"videotestsrc\","
      "\"type\":\"GstVideoTestSrc\",\"state\":\"PLAYING\",\"bin\":false}],"
      "\"edges\":[{\"source\":\"0:pipeline.source\",\"sourcePad\":\"src\",\"sink\":\"0:pipeline\","
      "\"sinkPad\":\"sink\",\"caps\":\"video/x-raw\"}]}");
  ok &= expect(inspector.handleBackendLine(graph), "graph response must be consumed");
  ok &= expect(inspector.nodeCount() == 2 && inspector.edgeCount() == 1, "graph counts mismatch");

  auto* view = require_child<QGraphicsView>(&inspector, "pipelineInspectorGraphView");
  QGraphicsRectItem* source_item = nullptr;
  if (view) {
    for (QGraphicsItem* item : view->scene()->items()) {
      auto* rectangle = dynamic_cast<QGraphicsRectItem*>(item);
      if (rectangle && rectangle->data(1).toString() == "0:pipeline.source") {
        source_item = rectangle;
        break;
      }
    }
  }
  ok &= expect(source_item != nullptr, "source graph node must be selectable");
  if (source_item) {
    source_item->setSelected(true);
    QApplication::processEvents();
  }
  ok &= expect(inspector.selectedNodeId() == "0:pipeline.source", "selected node id mismatch");
  ok &= expect(
      commands.size() == 2 && commands.back() == "@inspect-properties 2 0 cGlwZWxpbmUuc291cmNl\n",
      "property request must carry canonical base64 path");

  const QString properties = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"properties\",\"requestId\":2,"
      "\"status\":\"ok\",\"nodeId\":\"0:pipeline.source\",\"appIndex\":0,"
      "\"path\":\"pipeline.source\",\"properties\":["
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
          commands.back() == "@inspect-set-property 3 0 cGlwZWxpbmUuc291cmNl aXMtbGl2ZQ== dHJ1ZQ==\n",
      "live property command must encode path, name, and value");

  const QString set_result = QStringLiteral(
      "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":3,"
      "\"status\":\"ok\",\"nodeId\":\"0:pipeline.source\",\"appIndex\":0,"
      "\"path\":\"pipeline.source\",\"property\":\"is-live\"}");
  ok &= expect(inspector.handleBackendLine(set_result), "set result must be consumed");
  ok &= expect(
      commands.size() == 4 && commands.back() == "@inspect-properties 4 0 cGlwZWxpbmUuc291cmNl\n",
      "successful mutation must read the value back");

  auto* status = require_child<QLabel>(&inspector, "pipelineInspectorStatus");
  ok &= expect(
      inspector.handleBackendLine("HSTREAM_PIPELINE_INSPECTOR {not-json") && status &&
          status->text().contains("Malformed"),
      "malformed protocol lines must be contained and reported");
  inspector.setPipelineRunning(false);
  ok &= expect(inspector.nodeCount() == 0, "stopped inspector must clear stale live topology");
  return ok ? 0 : 1;
}
