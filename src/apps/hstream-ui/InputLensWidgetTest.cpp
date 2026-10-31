#include "src/apps/hstream-ui/InputLensWidget.h"
#include <QtWidgets/QCheckBox>
#include "src/apps/hstream-ui/CameraOpticsWidget.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <iostream>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QDoubleSpinBox unattached_fov;
  SetSharedCameraFovVisible(&unattached_fov, false);
  SetSharedCameraFovVisible(&unattached_fov, true);
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
  auto* values = widget.findChild<QGroupBox*>("inputLensValuesGroup");
  auto* projection = widget.findChild<QComboBox*>("inputLensProjectionCombo");
  auto* calibration = widget.findChild<QComboBox*>("inputLensOptimizationCombo");
  const auto original_color = values->palette().color(QPalette::Window);
  widget.setMetadataStatus("Read from MP4", true, true);
  if (!values->property("mp4Metadata").toBool() || values->palette().color(QPalette::Window) == original_color ||
      !values->isAncestorOf(projection) || values->isAncestorOf(calibration) || changes != 2) {
    std::cerr << "MP4 provenance must tint lens values without claiming calibration policy or changing inputs\n";
    return 1;
  }
  widget.setMetadataStatus("Warning: camera metadata differs", true);
  widget.setEnabled(false);
  widget.setEnabled(true);
  if (!widget.findChild<QDoubleSpinBox*>("inputLensCenterYSpin")->isReadOnly() ||
      widget.findChild<QComboBox*>("inputLensProjectionCombo")->isEnabled() ||
      widget.findChild<QLabel*>("inputLensMetadataStatus")->text() !=
          "Values source: Warning: camera metadata differs" ||
      values->property("mp4Metadata").toBool() || values->palette().color(QPalette::Window) != original_color)
    return 1;
  widget.setMetadataStatus("", false);
  if (widget.findChild<QDoubleSpinBox*>("inputLensCenterYSpin")->isReadOnly() ||
      !widget.findChild<QComboBox*>("inputLensProjectionCombo")->isEnabled())
    return 1;
  hm::stitching::AutoInputLensResult mismatch;
  mismatch.camera_details = "Left: 1x zoom; Right: 1.3x zoom";
  mismatch.camera.input_cameras = {{"left.mp4", {125.12, 70.36, lens}}, {"right.mp4", {97.07, 54.59, lens}}};
  widget.setCameraDetails(mismatch);
  if (widget.inputCameras() != mismatch.camera.input_cameras || !values->isHidden() || !calibration->isHidden())
    return 1;
  if (!widget.findChildren<QPushButton*>().empty() ||
      widget.findChild<QLabel*>("inputLensMismatchStatus")->text() != QString::fromStdString(mismatch.camera_details))
    return 1;
  widget.setCameraDetails({});
  if (!widget.inputCameras().empty() || values->isHidden() || calibration->isHidden() ||
      !widget.findChild<QGroupBox*>("inputLensMismatchGroup")->isHidden())
    return 1;
  const std::vector<hm::stitching::StitchCameraConfiguration> profiles{
      {"manual", "Manual", 100, 60}, {"auto", "Auto", 110, 70}};
  CameraOpticsWidget left(profiles), right(profiles);
  hm::stitching::StitchCameraSelection exact{"manual", 125.12273264892974, 70.36576080322266, lens};
  exact.auto_fov = exact.auto_lens = false;
  left.setCamera(exact, {});
  right.setCamera(exact, {});
  int left_changes = 0, right_changes = 0;
  left.changed = [&](bool) { ++left_changes; };
  right.changed = [&](bool) { ++right_changes; };
  left.findChild<QDoubleSpinBox*>("sourceCameraHorizontalFovSpin")->setValue(115.25);
  if (left.camera().horizontal_fov != 115.25 || left.camera().vertical_fov != exact.vertical_fov ||
      right.camera() != exact || left_changes != 1 || right_changes)
    return 1;
  left.findChild<QCheckBox*>("sourceCameraAutoFovCheck")->setChecked(true);
  if (!left.camera().automatic_fov() || right.camera().automatic_fov() || right_changes)
    return 1;
  left.findChild<QComboBox*>("cameraProfileCombo")->setCurrentIndex(1);
  if (!left.camera().automatic_lens() || right.camera().automatic_lens() || right_changes)
    return 1;
  return 0;
}
