#include "src/apps/hstream-ui/InputLensWidget.h"

#include <QtCore/QEvent>
#include <QtCore/QSignalBlocker>
#include <QtGui/QPalette>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include "src/apps/hstream-ui/ActionIcons.h"

void SetInputOpticsMetadataHighlight(QWidget* widget, bool from_mp4) {
  widget->setProperty("mp4Metadata", from_mp4);
  if (!from_mp4) {
    widget->setPalette(QPalette());
    widget->setAutoFillBackground(false);
    return;
  }
  QPalette palette = widget->parentWidget() ? widget->parentWidget()->palette() : QPalette();
  const QColor accent(25, 165, 135);
  for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button}) {
    const QColor base = palette.color(role);
    palette.setColor(
        role,
        QColor(
            (base.red() * 4 + accent.red()) / 5,
            (base.green() * 4 + accent.green()) / 5,
            (base.blue() * 4 + accent.blue()) / 5));
  }
  // Imported combo boxes are disabled for editing, but their values must stay
  // as legible as the read-only numerical fields beside them.
  for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
    palette.setColor(QPalette::Disabled, role, palette.color(QPalette::Active, role));
  widget->setPalette(palette);
  widget->setAutoFillBackground(true);
}

void SetSharedCameraFovVisible(QDoubleSpinBox* spin, bool visible) {
  spin->setVisible(visible);
  if (auto* form = qobject_cast<QFormLayout*>(spin->parentWidget()->layout()))
    if (auto* label = form->labelForField(spin))
      label->setVisible(visible);
}

InputLensWidget::InputLensWidget(QWidget* parent) : QGroupBox("Camera lens correction", parent) {
  setObjectName("stitchInputLensControls");
  setToolTip("Source-camera optics. Independent values and non-default correction requires the NONA mapping backend.");
  auto* layout = new QFormLayout(this);
  mismatch_group_ = new QGroupBox("Different camera optics", this);
  mismatch_group_->setObjectName("inputLensMismatchGroup");
  auto* mismatch_layout = new QVBoxLayout(mismatch_group_);
  mismatch_status_ = new QLabel(mismatch_group_);
  mismatch_status_->setObjectName("inputLensMismatchStatus");
  mismatch_status_->setWordWrap(true);
  mismatch_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  mismatch_status_->setTextFormat(Qt::PlainText);
  for (const auto& [key, label] : std::array<std::pair<std::string, QString>, 5>{
           {{"left", "Use Left for both"},
            {"right", "Use Right for both"},
            {"camera1", "Use Camera 1 for both"},
            {"camera2", "Use Camera 2 for both"},
            {"average", "Average for both (approximation)"}}}) {
    auto* button = new QPushButton(action_icon(ActionIcon::Apply), label, mismatch_group_);
    button->setObjectName(QString::fromStdString("inputLensUse_" + key));
    button->setToolTip(
        "Copies both camera FOV and lens correction into manual settings. Save Preset or Play applies the change.");
    choice_buttons_[key] = button;
    mismatch_layout->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, key]() {
      const auto found = choices_.find(key);
      if (found != choices_.end() && sharedChoiceSelected) {
        const auto camera = found->second;
        sharedChoiceSelected(camera);
      }
    });
  }
  mismatch_layout->addWidget(mismatch_status_);
  mismatch_group_->hide();
  layout->addRow(mismatch_group_);
  automatic_ = new QCheckBox("Auto lens correction", this);
  automatic_->setObjectName("inputLensAutoCheck");
  automatic_->setToolTip(
      "Use compatible MP4 calibration, then a verified recording-mode profile, then baseline defaults. Independent of Auto camera FOV.");
  layout->addRow(automatic_);
  connect(automatic_, &QCheckBox::toggled, this, [this]() {
    if (automationChanged)
      automationChanged();
  });
  values_group_ = new QGroupBox("Projection, distortion and center offsets", this);
  values_group_->setObjectName("inputLensValuesGroup");
  auto* values_layout = new QFormLayout(values_group_);
  layout->addRow(values_group_);
  metadata_status_ = new QLabel(values_group_);
  metadata_status_->setObjectName("inputLensMetadataStatus");
  metadata_status_->setWordWrap(true);
  metadata_status_->setTextFormat(Qt::PlainText);
  metadata_status_->hide();
  values_layout->addRow(metadata_status_);
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
      "This is stitching's fitting policy, not a setting recorded in the MP4. "
      "Automatic may refit all lens parameters. Refine fits camera horizontal FOV, b and center while retaining a and c. "
      "Fixed retains the camera horizontal FOV and all lens values. Every mode still aligns the camera poses.");
  values_layout->addRow("Source projection", projection_);
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
    values_layout->addRow(labels[i], spin);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, spin]() {
      spin->setProperty("exactLensValue", QVariant());
      if (changed)
        changed();
    });
  }
  layout->addRow("Calibration behavior", optimization_);
  auto* calibration_help =
      new QLabel("Controls whether stitching may refit these values. This behavior is not read from the MP4.", this);
  calibration_help->setWordWrap(true);
  layout->addRow(calibration_help);
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

void InputLensWidget::setMetadataStatus(const QString& message, bool automatic, bool from_mp4) {
  const QSignalBlocker blocker(automatic_);
  automatic_->setChecked(automatic);
  metadata_status_->setText("Values source: " + message);
  metadata_status_->setVisible(!message.isEmpty());
  SetInputOpticsMetadataHighlight(values_group_, automatic && from_mp4);
  QPalette readable;
  for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
    readable.setColor(QPalette::Disabled, role, palette().color(QPalette::Active, role));
  projection_->setPalette(readable);
  optimization_->setPalette(readable);
  projection_->setEnabled(!automatic);
  optimization_->setEnabled(!automatic);
  for (auto* parameter : parameters_)
    parameter->setReadOnly(automatic);
}

bool InputLensWidget::automatic() const {
  return automatic_->isChecked();
}

void InputLensWidget::setSharedChoices(const hm::stitching::AutoInputLensResult& result) {
  choices_ = result.shared_choices;
  input_cameras_ = result.camera.input_cameras;
  values_group_->setVisible(input_cameras_.empty());
  optimization_->setVisible(input_cameras_.empty());
  if (auto* form = qobject_cast<QFormLayout*>(layout()))
    if (auto* label = form->labelForField(optimization_))
      label->setVisible(input_cameras_.empty());
  mismatch_group_->setTitle(input_cameras_.empty() ? "Different camera optics" : "Independent camera optics");
  mismatch_status_->setText(QString::fromStdString(result.shared_choices_message));
  mismatch_group_->setVisible(!result.shared_choices_message.empty());
  const bool unassigned = choices_.count("camera1") || choices_.count("camera2");
  for (const auto& [key, button] : choice_buttons_) {
    button->setVisible(key == "average" || ((key == "camera1" || key == "camera2") == unassigned));
    button->setEnabled(choices_.count(key) != 0);
  }
}

QWidget* InputLensWidget::takeSharedChoicesWidget() {
  layout()->removeWidget(mismatch_group_);
  return mismatch_group_;
}

void InputLensWidget::changeEvent(QEvent* event) {
  QGroupBox::changeEvent(event);
  if (event->type() == QEvent::EnabledChange)
    mismatch_group_->setEnabled(isEnabled());
}
