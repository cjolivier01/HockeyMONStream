#pragma once

#include <deque>
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
  void reject() override;

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
  void recordEdit();
  void flushDimensionEdit();
  void travelHistory(bool redo);
  void refreshEditControls();
  void refreshGuidance();
  void cancelLine();
  hm::stitching::RinkLayout dimensionValues() const;
  using Detections = decltype(RinkMapWorkResult::detections);
  struct EditState {
    hm::stitching::RinkMapDocument document;
    std::shared_ptr<const Detections> detections;
  };
  EditState captureEdit() const;
  bool hasUnsavedEdits() const;

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
  QLabel* guidance_{nullptr};
  QLabel* edited_{nullptr};
  QPushButton* detect_{nullptr};
  QPushButton* stop_{nullptr};
  QPushButton* save_{nullptr};
  QPushButton* remove_{nullptr};
  QPushButton* undo_{nullptr};
  QPushButton* redo_{nullptr};
  std::array<QPushButton*, 2> board_shortcuts_{};
  PreviewFocusButton* focus_{nullptr};
  std::optional<QPointF> first_line_point_;
  int first_line_camera_{0};
  bool rebuilding_{false};
  bool busy_{false};
  bool resolved_{false};
  bool close_prompt_active_{false};
  bool close_after_worker_{false};
  std::optional<hm::stitching::RinkMapDocument> clean_document_;
  std::shared_ptr<const Detections> history_detections_{std::make_shared<Detections>()};
  std::deque<EditState> undo_history_, redo_history_;
  std::optional<EditState> pending_dimension_edit_;
};
