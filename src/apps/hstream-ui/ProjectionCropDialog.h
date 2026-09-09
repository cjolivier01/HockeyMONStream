#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QProcess>
#include <QtCore/QRectF>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtWidgets/QDialog>

#include <array>
#include <functional>

#include "hstream/src/libs/stitching/GameConfig.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

// The image is a bounded offline preview. Crop coordinates are fractions of
// the full projected canvas, independent of widget size and preview resolution.
class ProjectionCropCanvas : public QWidget {
 public:
  explicit ProjectionCropCanvas(QWidget* parent = nullptr);
  void setImage(const QImage& image);
  void setPlaceholderMessage(const QString& message);
  void setCrop(const std::array<double, 4>& crop, bool editable, bool keep_full_width);
  const std::array<double, 4>& crop() const {
    return crop_;
  }
  QRectF imageRect() const;
  std::function<void(const std::array<double, 4>&)> cropChanged;

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;

 private:
  QRectF cropRect() const;
  int hitTest(const QPointF& point) const;
  QImage image_;
  QString placeholder_message_{"Preparing crop preview…"};
  std::array<double, 4> crop_{0, 1, 0, 1};
  bool editable_{false};
  bool keep_full_width_{false};
  int dragged_{0};
  QPointF press_;
  std::array<double, 4> drag_start_{};
};

class ProjectionCropDialog : public QDialog {
 public:
  ProjectionCropDialog(
      const QString& game_directory,
      const hm::stitching::StitchProjectionFraming& framing,
      const QString& projection,
      const std::vector<double>& projection_parameters,
      const hm::stitching::StitchCameraSelection& camera,
      const QString& preview_unavailable_reason = {},
      QWidget* parent = nullptr);
  ~ProjectionCropDialog() override;
  hm::stitching::StitchProjectionFraming framing() const;
  QByteArray sourceRevision() const {
    return preview_ready_ ? source_revision_ : QByteArray();
  }

 protected:
  void reject() override;

 private:
  void syncControls();
  void seedManualFromAuto();
  void loadPreview();
  void runTool(const QString& program, const QStringList& arguments, std::function<void()> completed);
  void previewFailed(const QString& message);
  void acceptCrop();
  void stopTool();
  QString game_directory_;
  hm::stitching::StitchProjectionFraming initial_;
  QString projection_;
  std::vector<double> projection_parameters_;
  hm::stitching::StitchCameraSelection camera_;
  QTemporaryDir temporary_;
  QByteArray source_revision_;
  QSize preview_size_;
  std::array<double, 4> manual_crop_;
  std::array<double, 4> auto_crop_{0, 1, 0, 1};
  std::array<double, 2> saved_horizontal_{};
  bool manual_edited_{false};
  bool preview_ready_{false};
  bool auto_ready_{false};
  ProjectionCropCanvas* canvas_{nullptr};
  QComboBox* mode_{nullptr};
  QCheckBox* keep_width_{nullptr};
  std::array<QDoubleSpinBox*, 4> edges_{};
  QLabel* coverage_{nullptr};
  QLabel* status_{nullptr};
  QPushButton* accept_{nullptr};
  QProcess* process_{nullptr};
};
