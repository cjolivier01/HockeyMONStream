#pragma once

#include <QtWidgets/QGroupBox>

#include <array>
#include <functional>

#include "hstream/src/libs/stitching/InputLens.h"

class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;

class InputLensWidget : public QGroupBox {
 public:
  explicit InputLensWidget(QWidget* parent = nullptr);
  hm::stitching::StitchInputLens lens() const;
  void setLens(const hm::stitching::StitchInputLens& lens);
  void setMetadataStatus(const QString& message, bool automatic);
  bool automatic() const;
  std::function<void()> automationChanged;
  std::function<void()> changed;

 private:
  QLabel* metadata_status_;
  QCheckBox* automatic_;
  QComboBox* projection_;
  QComboBox* optimization_;
  std::array<QDoubleSpinBox*, 5> parameters_{};
};
