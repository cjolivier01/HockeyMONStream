#pragma once

#include <memory>

#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QDialog>

#include "src/apps/hstream-ui/RinkMapBackend.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTabWidget;
class RinkMapCanvas;
class RinkMapSchematic;
class PreviewFocusButton;

class RinkMapDialog : public QDialog {
 public:
  explicit RinkMapDialog(const QString& game_directory, QWidget* parent = nullptr);
  ~RinkMapDialog() override;

 private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void refresh();
  void update(bool detect = false);
  void rebuildTable();
  void addPoint(int camera, QPointF point);
  void save();
  void setBusy(bool busy);
  void restorePreview();
  void setStatus(const QString& message, bool error = false);

  struct Job;
  std::optional<RinkMapSnapshot> snapshot_;
  RinkMapWorkResult state_;
  std::shared_ptr<Job> job_;
  QThread* worker_{nullptr};
  QTimer refresh_timer_;
  std::array<RinkMapCanvas*, 2> cameras_{};
  std::array<QDoubleSpinBox*, 5> dimensions_{};
  RinkMapSchematic* schematic_{nullptr};
  QComboBox* feature_{nullptr};
  QTableWidget* table_{nullptr};
  QTabWidget* tabs_{nullptr};
  QWidget* sidebar_{nullptr};
  QLabel* status_{nullptr};
  QPushButton* detect_{nullptr};
  QPushButton* stop_{nullptr};
  QPushButton* save_{nullptr};
  QPushButton* remove_{nullptr};
  PreviewFocusButton* focus_{nullptr};
  std::optional<QPointF> first_line_point_;
  int first_line_camera_{0};
  bool rebuilding_{false};
  bool busy_{false};
  bool resolved_{false};
};
