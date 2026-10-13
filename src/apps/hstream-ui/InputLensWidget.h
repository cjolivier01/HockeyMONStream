#pragma once

#include <QtWidgets/QGroupBox>

#include <array>
#include <functional>

#include "hstream/src/libs/stitching/InputLens.h"

class QComboBox;
class QDoubleSpinBox;

class InputLensWidget : public QGroupBox {
 public:
  explicit InputLensWidget(QWidget* parent = nullptr);
  hm::stitching::StitchInputLens lens() const;
  void setLens(const hm::stitching::StitchInputLens& lens);
  std::function<void()> changed;

 private:
  QComboBox* projection_;
  QComboBox* optimization_;
  std::array<QDoubleSpinBox*, 5> parameters_{};
};
