#pragma once

#include <QtWidgets/QWidget>
#include <functional>
#include "hstream/src/libs/stitching/VideoLensMetadata.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class InputLensWidget;
class QTimeEdit;

// One source owns its complete prescription. Display rounding never changes
// untouched values; the parent binds the editor to the source, not its tab index.
class CameraOpticsWidget : public QWidget {
 public:
  explicit CameraOpticsWidget(
      const std::vector<hm::stitching::StitchCameraConfiguration>& profiles,
      QWidget* parent = nullptr);
  void setCamera(
      const hm::stitching::StitchCameraSelection& camera,
      const hm::stitching::InputCameraMetadata& metadata);
  hm::stitching::StitchCameraSelection camera() const {
    return camera_;
  }
  std::function<void(bool refresh)> changed;

 private:
  void notify(bool refresh);
  std::vector<hm::stitching::StitchCameraConfiguration> profiles_;
  hm::stitching::StitchCameraSelection camera_;
  QComboBox* profile_;
  QCheckBox* automatic_fov_;
  QDoubleSpinBox* horizontal_;
  QDoubleSpinBox* vertical_;
  QLabel* recording_;
  QLabel* fov_status_;
  InputLensWidget* lens_;
};

// Independent camera trim, shown separately from optical properties.
class CameraSourceStartWidget : public QWidget {
 public:
  explicit CameraSourceStartWidget(QWidget* parent = nullptr);
  void setStart(double seconds);
  std::function<void(double)> changed;

 private:
  QTimeEdit* start_;
};
