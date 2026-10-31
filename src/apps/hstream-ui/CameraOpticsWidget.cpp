#include "src/apps/hstream-ui/CameraOpticsWidget.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include "src/apps/hstream-ui/InputLensWidget.h"

CameraOpticsWidget::CameraOpticsWidget(
    const std::vector<hm::stitching::StitchCameraConfiguration>& profiles,
    QWidget* parent)
    : QWidget(parent), profiles_(profiles) {
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  profile_ = new QComboBox(this);
  profile_->setObjectName("cameraProfileCombo");
  for (const auto& profile : profiles_)
    profile_->addItem(QString::fromStdString(profile.display_name), QString::fromStdString(profile.id));
  form->addRow("Camera / recording-mode defaults", profile_);
  recording_ = new QLabel(this);
  recording_->setObjectName("cameraRecordingMetadata");
  recording_->setWordWrap(true);
  recording_->setTextFormat(Qt::PlainText);
  form->addRow(recording_);
  layout->addLayout(form);
  auto* fov = new QGroupBox("Camera field of view", this);
  auto* fov_layout = new QFormLayout(fov);
  automatic_fov_ = new QCheckBox("Auto camera FOV", fov);
  automatic_fov_->setObjectName("sourceCameraAutoFovCheck");
  fov_layout->addRow(automatic_fov_);
  fov_status_ = new QLabel(fov);
  fov_status_->setObjectName("cameraFovSourceLabel");
  fov_status_->setWordWrap(true);
  fov_status_->setTextFormat(Qt::PlainText);
  fov_layout->addRow(fov_status_);
  horizontal_ = new QDoubleSpinBox(fov);
  vertical_ = new QDoubleSpinBox(fov);
  horizontal_->setObjectName("sourceCameraHorizontalFovSpin");
  vertical_->setObjectName("sourceCameraVerticalFovSpin");
  horizontal_->setRange(0.01, 359.99);
  vertical_->setRange(0.01, 180);
  for (auto* spin : {horizontal_, vertical_}) {
    spin->setDecimals(2);
    spin->setSingleStep(0.1);
    spin->setSuffix(QString::fromUtf8("°"));
  }
  fov_layout->addRow("Horizontal FOV", horizontal_);
  fov_layout->addRow("Vertical FOV", vertical_);
  layout->addWidget(fov);
  lens_ = new InputLensWidget(this);
  layout->addWidget(lens_);
  for (auto* child : lens_->findChildren<QObject*>())
    if (!child->objectName().isEmpty())
      child->setObjectName("sourceOptics_" + child->objectName());
  layout->addStretch();
  for (auto* form_layout : findChildren<QFormLayout*>()) {
    form_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  }
  for (auto* combo : findChildren<QComboBox*>()) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(12);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  for (auto* label : {recording_, fov_status_})
    label->setMinimumWidth(0);
  connect(profile_, &QComboBox::currentIndexChanged, this, [this]() {
    for (const auto& profile : profiles_)
      if (profile.id == profile_->currentData().toString().toStdString()) {
        camera_ = {profile.id, profile.horizontal_fov, profile.vertical_fov, profile.input_lens};
        camera_.auto_fov = camera_.auto_lens = profile.id != "manual";
        setCamera(camera_, {});
        notify(true);
        return;
      }
  });
  connect(automatic_fov_, &QCheckBox::toggled, this, [this](bool value) {
    camera_.auto_fov = value;
    notify(value);
  });
  connect(horizontal_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    camera_.horizontal_fov = value;
    notify(false);
  });
  connect(vertical_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
    camera_.vertical_fov = value;
    notify(false);
  });
  lens_->automationChanged = [this]() {
    camera_.auto_lens = lens_->automatic();
    notify(lens_->automatic());
  };
  lens_->changed = [this]() {
    camera_.input_lens = lens_->lens();
    notify(false);
  };
}

void CameraOpticsWidget::setCamera(
    const hm::stitching::StitchCameraSelection& camera,
    const hm::stitching::InputCameraMetadata& metadata) {
  camera_ = camera;
  camera_.input_cameras.clear();
  const QSignalBlocker profile_block(profile_), auto_block(automatic_fov_), horizontal_block(horizontal_),
      vertical_block(vertical_);
  profile_->setCurrentIndex(profile_->findData(QString::fromStdString(camera_.configuration)));
  automatic_fov_->setChecked(camera_.automatic_fov());
  horizontal_->setValue(camera_.horizontal_fov);
  vertical_->setValue(camera_.vertical_fov);
  horizontal_->setReadOnly(camera_.automatic_fov());
  vertical_->setReadOnly(camera_.automatic_fov());
  recording_->setText(QString::fromStdString(metadata.recording_message));
  fov_status_->setText(QString::fromStdString(metadata.fov_message));
  lens_->setLens(camera_.input_lens);
  lens_->setMetadataStatus(
      QString::fromStdString(metadata.lens_message), camera_.automatic_lens(), metadata.lens_from_mp4);
  SetInputOpticsMetadataHighlight(recording_, metadata.recording_metadata_found);
  SetInputOpticsMetadataHighlight(horizontal_, metadata.horizontal_fov_from_mp4);
  SetInputOpticsMetadataHighlight(vertical_, metadata.vertical_fov_from_mp4);
}

void CameraOpticsWidget::notify(bool refresh) {
  if (changed)
    changed(refresh);
}
