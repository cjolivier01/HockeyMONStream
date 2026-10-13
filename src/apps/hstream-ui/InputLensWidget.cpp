#include "src/apps/hstream-ui/InputLensWidget.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>

InputLensWidget::InputLensWidget(QWidget* parent) : QGroupBox("Input lens", parent) {
  setObjectName("stitchInputLensControls");
  setToolTip("Shared source-camera optics. Non-default correction requires the NONA mapping backend.");
  auto* layout = new QFormLayout(this);
  projection_ = new QComboBox(this);
  projection_->setObjectName("inputLensProjectionCombo");
  projection_->addItem("Rectilinear", "rectilinear");
  projection_->addItem("Full-frame fisheye (wide angle)", "full-frame-fisheye");
  projection_->setToolTip(
      "Model of the original camera images. The output panorama projection is selected separately.");
  optimization_ = new QComboBox(this);
  optimization_->setObjectName("inputLensOptimizationCombo");
  optimization_->addItem("Automatic lens fit", "auto");
  optimization_->addItem("Refine FOV, b and center", "refine");
  optimization_->addItem("Keep lens correction fixed", "fixed");
  optimization_->setToolTip(
      "Automatic may refit all lens parameters. Refine fits camera horizontal FOV, b and center while retaining a and c. "
      "Fixed retains the camera horizontal FOV and all values below. Every mode still aligns the camera poses.");
  layout->addRow("Source projection", projection_);
  layout->addRow("Calibration", optimization_);
  const char* names[] = {
      "inputLensASpin", "inputLensBSpin", "inputLensCSpin", "inputLensCenterXSpin", "inputLensCenterYSpin"};
  const char* labels[] = {"Distortion a", "Distortion b", "Distortion c", "Center X offset", "Center Y offset"};
  for (size_t i = 0; i < parameters_.size(); ++i) {
    auto* spin = parameters_[i] = new QDoubleSpinBox(this);
    spin->setObjectName(names[i]);
    spin->setDecimals(i < 3 ? 9 : 6);
    spin->setRange(i < 3 ? -1 : -50, i < 3 ? 1 : 50);
    spin->setSingleStep(i < 3 ? 0.001 : 0.1);
    if (i >= 3)
      spin->setSuffix(" %");
    spin->setToolTip(
        i < 3
            ? "Hugin radial lens coefficient, shared by the two source cameras. Used as a seed unless lens correction is fixed."
            : "Hugin optical-center shift as a percentage of the full source width (X) or height (Y). Scales with source resolution.");
    layout->addRow(labels[i], spin);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, spin]() {
      spin->setProperty("exactLensValue", QVariant());
      if (changed)
        changed();
    });
  }
  for (auto* combo : {projection_, optimization_})
    connect(combo, &QComboBox::currentIndexChanged, this, [this]() {
      if (changed)
        changed();
    });
}

hm::stitching::StitchInputLens InputLensWidget::lens() const {
  hm::stitching::StitchInputLens lens;
  lens.projection = projection_->currentData().toString().toStdString();
  lens.optimization = optimization_->currentData().toString().toStdString();
  for (size_t i = 0; i < parameters_.size(); ++i) {
    const QVariant exact = parameters_[i]->property("exactLensValue");
    const double value = exact.isValid() ? exact.toDouble() : parameters_[i]->value() / (i < 3 ? 1 : 100);
    if (i < 3)
      lens.distortion[i] = value;
    else
      lens.center_offset[i - 3] = value;
  }
  return lens;
}

void InputLensWidget::setLens(const hm::stitching::StitchInputLens& lens) {
  const QSignalBlocker projection_block(projection_), optimization_block(optimization_);
  projection_->setCurrentIndex(projection_->findData(QString::fromStdString(lens.projection)));
  optimization_->setCurrentIndex(optimization_->findData(QString::fromStdString(lens.optimization)));
  for (size_t i = 0; i < parameters_.size(); ++i) {
    const QSignalBlocker block(parameters_[i]);
    const double value = i < 3 ? lens.distortion[i] : lens.center_offset[i - 3];
    parameters_[i]->setValue(value * (i < 3 ? 1 : 100));
    parameters_[i]->setProperty("exactLensValue", value);
  }
}
