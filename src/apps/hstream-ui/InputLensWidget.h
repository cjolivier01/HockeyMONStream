#pragma once

#include <QtWidgets/QGroupBox>

#include <array>
#include <functional>

#include "hstream/src/libs/stitching/InputLens.h"

class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;

// Tint only values successfully obtained from the selected MP4 files. Resetting
// restores the inherited palette, including when switching games or policies.
void SetInputOpticsMetadataHighlight(QWidget* widget, bool from_mp4);

class InputLensWidget : public QGroupBox {
 public:
  explicit InputLensWidget(QWidget* parent = nullptr);
  hm::stitching::StitchInputLens lens() const;
  void setLens(const hm::stitching::StitchInputLens& lens);
  void setMetadataStatus(const QString& message, bool automatic, bool from_mp4 = false);
  bool automatic() const;
  std::function<void()> automationChanged;
  std::function<void()> changed;

 private:
  QGroupBox* values_group_;
  QLabel* metadata_status_;
  QCheckBox* automatic_;
  QComboBox* projection_;
  QComboBox* optimization_;
  std::array<QDoubleSpinBox*, 5> parameters_{};
};
