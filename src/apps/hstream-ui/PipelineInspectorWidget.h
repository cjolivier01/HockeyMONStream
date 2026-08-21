#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

class QComboBox;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class PipelineInspectorWidget : public QWidget {
 public:
  using CommandWriter = std::function<bool(const QByteArray&)>;

  explicit PipelineInspectorWidget(QWidget* parent = nullptr);

  void setCommandWriter(CommandWriter writer);
  void setPipelineRunning(bool running);
  void requestRefresh();

  // Returns true for every inspector protocol line, including malformed
  // payloads, so large structured messages never spill into the ordinary log.
  bool handleBackendLine(const QString& line);

  QString selectedNodeId() const;
  int nodeCount() const;
  int edgeCount() const;

 private:
  struct NodeData {
    QString id;
    int app_index{0};
    QString path;
    QString parent_id;
    QString name;
    QString factory;
    QString type;
    QString state;
    bool bin{false};
  };

  struct EdgeData {
    QString source;
    QString source_pad;
    QString sink;
    QString sink_pad;
  };

  bool writeCommand(const QByteArray& command);
  uint64_t nextRequestId();
  void clearInspectionState();
  bool responseMatchesSession(const QJsonObject& response) const;
  void requestProperties(const NodeData& node);
  void applySelectedProperty();
  void renderGraph();
  void showProperties(const QJsonObject& response);
  void selectNode(const QString& node_id);
  void selectNextSearchMatch();
  void updatePropertyEditor();
  void updateStatus(const QString& text, bool error = false);
  static QByteArray encodeToken(const QString& value);

  CommandWriter command_writer_;
  bool pipeline_running_{false};
  bool have_session_{false};
  qint64 session_stage_{0};
  uint64_t session_generation_{0};
  uint64_t next_request_id_{0};
  uint64_t pending_graph_request_{0};
  uint64_t pending_property_request_{0};
  uint64_t pending_set_request_{0};
  QString selected_node_id_;
  std::map<QString, NodeData> nodes_;
  std::vector<EdgeData> edges_;
  std::map<QString, QGraphicsRectItem*> node_items_;
  std::vector<QJsonObject> displayed_properties_;

  QGraphicsView* graph_view_{nullptr};
  QGraphicsScene* graph_scene_{nullptr};
  QLabel* status_label_{nullptr};
  QLabel* selected_node_label_{nullptr};
  QLineEdit* node_search_{nullptr};
  QLineEdit* property_filter_{nullptr};
  QTableWidget* property_table_{nullptr};
  QComboBox* property_editor_{nullptr};
  QPushButton* apply_button_{nullptr};
};
