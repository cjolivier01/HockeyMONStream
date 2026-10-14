#include "src/apps/hstream-ui/InputLensWidget.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>

#include <iostream>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  InputLensWidget widget;
  hm::stitching::StitchInputLens lens;
  lens.projection = "full-frame-fisheye";
  lens.optimization = "fixed";
  lens.distortion[1] = 0.0208293582808693;
  lens.center_offset = {-10.102540338009 / 7680, 620.178405843678 / 4320};
  int changes = 0;
  widget.changed = [&] { ++changes; };
  widget.setLens(lens);
  if (widget.lens() != lens || changes != 0) {
    std::cerr << "Loading lens controls lost precision or dirtied the form\n";
    return 1;
  }
  widget.findChild<QDoubleSpinBox*>("inputLensCenterYSpin")->setValue(12.5);
  lens.center_offset[1] = 0.125;
  if (widget.lens() != lens || changes != 1) {
    std::cerr << "Edited center percentage did not replace the preserved value\n";
    return 1;
  }
  widget.findChild<QComboBox*>("inputLensOptimizationCombo")->setCurrentIndex(1);
  lens.optimization = "refine";
  if (widget.lens() != lens || changes != 2)
    return 1;
  widget.setLens({});
  if (widget.lens() != hm::stitching::StitchInputLens{} || changes != 2)
    return 1;
  widget.setMetadataStatus("Warning: camera metadata differs", true);
  widget.setEnabled(false);
  widget.setEnabled(true);
  if (!widget.findChild<QDoubleSpinBox*>("inputLensCenterYSpin")->isReadOnly() ||
      widget.findChild<QComboBox*>("inputLensProjectionCombo")->isEnabled() ||
      widget.findChild<QLabel*>("inputLensMetadataStatus")->text() != "Warning: camera metadata differs")
    return 1;
  widget.setMetadataStatus("", false);
  if (widget.findChild<QDoubleSpinBox*>("inputLensCenterYSpin")->isReadOnly() ||
      !widget.findChild<QComboBox*>("inputLensProjectionCombo")->isEnabled())
    return 1;
  return 0;
}
