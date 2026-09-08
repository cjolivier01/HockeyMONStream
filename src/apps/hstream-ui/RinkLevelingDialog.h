#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QDialog>

#include <array>
#include <functional>
#include <vector>

#include "hstream/src/libs/stitching/RinkLeveling.h"

class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QTabWidget;
class ScoreboardSelectionCanvas;

// Offline calibration only: reads a locked snapshot, runs Hugin on temporary
// still images, and returns an angle. Never writes game configuration/artifacts.
class RinkLevelingDialog : public QDialog {
 public:
  RinkLevelingDialog(
      const QString& game_directory,
      const std::array<double, 3>& current_rotation,
      QWidget* parent = nullptr);
  ~RinkLevelingDialog() override;
  QString loadError() const {
    return load_error_;
  }
  std::array<double, 3> rotationDegrees() const;
  QByteArray sourceRevision() const {
    return source_revision_;
  }
  // Caller holds the stitching artifact lock when this is used at publication.
  static QByteArray sourceRevision(const QString& game_directory);

 protected:
  void reject() override;

 private:
  void loadSnapshot();
  void estimate();
  void preview();
  void selectionChanged();
  void startTool(
      const QString& program,
      const QStringList& arguments,
      const QByteArray& input,
      std::function<void(const QByteArray&)> completed);
  void setBusy(bool busy);
  void fail(const QString& message);
  void acceptAngles();

  QString game_directory_;
  QTemporaryDir temporary_;
  QString load_error_;
  QByteArray source_revision_;
  hm::stitching::RinkLevelingProject project_;
  std::array<double, 3> published_rotation_{};
  std::array<double, 3> initial_rotation_{};
  std::array<ScoreboardSelectionCanvas*, 2> canvases_{};
  std::array<QDoubleSpinBox*, 2> angle_spins_{};
  QLabel* status_{nullptr};
  QTabWidget* tabs_{nullptr};
  ScoreboardSelectionCanvas* preview_canvas_{nullptr};
  QPushButton* estimate_button_{nullptr};
  QPushButton* preview_button_{nullptr};
  QPushButton* accept_button_{nullptr};
  QProcess* process_{nullptr};
  bool busy_{false};
  bool estimated_{false};
  bool previewed_{false};
};
