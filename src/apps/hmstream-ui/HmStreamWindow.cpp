#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtCore/QDateTime>
#include <QtCore/Qt>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>

namespace {

QString timestamp() {
  return QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
}

QLabel* make_value_label(const QString& object_name, const QString& value) {
  auto* label = new QLabel(value);
  label->setObjectName(object_name);
  label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  label->setMinimumWidth(92);
  return label;
}

} // namespace

HmStreamWindow::HmStreamWindow(QWidget* parent) : QMainWindow(parent) {
  buildUi();
  setPipelineRunning(false);
  appendLog("hmstream-ui started with disconnected demo backend");
}

QString HmStreamWindow::pipelineStateText() const {
  return pipeline_state_ ? pipeline_state_->text() : QString();
}

QString HmStreamWindow::outputStateText(const QString& id) const {
  const auto it = output_states_.find(id);
  return it == output_states_.end() ? QString() : it->second->text();
}

QString HmStreamWindow::logText() const {
  return log_ ? log_->toPlainText() : QString();
}

int HmStreamWindow::cameraControlValue(const QString& id) const {
  const auto it = camera_sliders_.find(id);
  return it == camera_sliders_.end() ? 0 : it->second->value();
}

int HmStreamWindow::cameraTabCount() const {
  return camera_tabs_ ? camera_tabs_->count() : 0;
}

void HmStreamWindow::buildUi() {
  setObjectName("hmstreamUi");
  setWindowTitle("HMStream UI");
  resize(1440, 900);

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(10);

  buildTopBar(root);
  buildMainArea(root);
  buildLog(root);

  setCentralWidget(central);
}

void HmStreamWindow::buildTopBar(QVBoxLayout* root) {
  auto* bar = new QHBoxLayout();
  bar->setSpacing(8);

  auto* title = new QLabel("HMStream Runtime Control");
  title->setObjectName("titleLabel");
  QFont title_font = title->font();
  title_font.setBold(true);
  title_font.setPointSize(title_font.pointSize() + 2);
  title->setFont(title_font);

  pipeline_state_ = make_value_label("pipelineStateLabel", "STOPPED");
  backend_mode_ = new QLabel("Backend: demo/disconnected");
  backend_mode_->setObjectName("backendModeLabel");

  auto* start = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), "Start");
  start->setObjectName("startPipelineButton");
  auto* restart = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), "Restart Stage");
  restart->setObjectName("restartStageButton");
  auto* save = new QPushButton(style()->standardIcon(QStyle::SP_DialogSaveButton), "Save Preset");
  save->setObjectName("savePresetButton");
  auto* reset = new QPushButton("Reset Camera");
  reset->setObjectName("resetCameraButton");
  auto* stop = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), "Stop");
  stop->setObjectName("stopPipelineButton");

  connect(start, &QPushButton::clicked, this, [this]() { setPipelineRunning(true); });
  connect(stop, &QPushButton::clicked, this, [this]() { setPipelineRunning(false); });
  connect(restart, &QPushButton::clicked, this, [this]() { restartStage(); });
  connect(save, &QPushButton::clicked, this, [this]() { savePreset(); });
  connect(reset, &QPushButton::clicked, this, [this]() { resetCameraControls(); });

  bar->addWidget(title);
  bar->addSpacing(16);
  bar->addWidget(new QLabel("Pipeline:"));
  bar->addWidget(pipeline_state_);
  bar->addWidget(backend_mode_);
  bar->addStretch(1);
  bar->addWidget(start);
  bar->addWidget(restart);
  bar->addWidget(save);
  bar->addWidget(reset);
  bar->addWidget(stop);
  root->addLayout(bar);
}

void HmStreamWindow::buildMainArea(QVBoxLayout* root) {
  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName("mainSplitter");

  auto* left = new QWidget();
  auto* left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 0, 0);
  buildPreviewPane(left_layout);

  auto* right = new QWidget();
  auto* right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(0, 0, 0, 0);
  buildOutputControls(right_layout);
  buildCameraControls(right_layout);
  right_layout->addStretch(1);

  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);
  root->addWidget(splitter, 1);
}

void HmStreamWindow::buildPreviewPane(QVBoxLayout* root) {
  auto* tabs = new QTabWidget();
  tabs->setObjectName("previewTabs");

  auto* program = new QWidget();
  auto* layout = new QVBoxLayout(program);
  auto* preview = new QLabel("Embedded --show attach point\n\nDemo backend: no GStreamer preview is attached yet.");
  preview->setObjectName("previewSurface");
  preview->setAlignment(Qt::AlignCenter);
  preview->setMinimumHeight(420);
  preview->setFrameShape(QFrame::StyledPanel);
  preview->setStyleSheet("QLabel#previewSurface { background: #12171c; color: #dce6ef; }");

  preview_status_ = new QLabel("Demo backend stopped");
  preview_status_->setObjectName("previewStatusLabel");
  layout->addWidget(preview, 1);
  layout->addWidget(preview_status_);

  tabs->addTab(program, "Program");
  tabs->addTab(new QLabel("Stitched canvas preview"), "Stitched");
  tabs->addTab(new QLabel("Camera 1 preview"), "Camera 1");
  tabs->addTab(new QLabel("Camera 2 preview"), "Camera 2");
  tabs->addTab(new QLabel("Camera 3 preview"), "Camera 3");
  root->addWidget(tabs, 1);
}

void HmStreamWindow::buildOutputControls(QVBoxLayout* parent) {
  auto* group = new QGroupBox("Output Routing");
  group->setObjectName("outputRoutingGroup");
  auto* layout = new QVBoxLayout(group);
  output_list_ = layout;

  const std::vector<std::pair<QString, QString>> outputs = {
      {"youtube-primary", "YouTube Primary"},
      {"rtsp-local", "RTSP Local"},
      {"archive-file", "Archive File"},
      {"spare-rtmp", "Spare RTMP"},
  };

  for (const auto& [id, label] : outputs) {
    auto* row = new QHBoxLayout();
    auto* toggle = new QCheckBox(label);
    toggle->setObjectName("outputToggle_" + id);
    toggle->setChecked(false);
    auto* state = make_value_label("outputState_" + id, "STOPPED");
    output_toggles_[id] = toggle;
    output_states_[id] = state;
    connect(toggle, &QCheckBox::toggled, this, [this, id](bool enabled) { toggleOutput(id, enabled); });
    row->addWidget(toggle, 1);
    row->addWidget(state);
    layout->addLayout(row);
  }

  auto* redirect = new QPushButton("Redirect YouTube");
  redirect->setObjectName("redirectYoutubeButton");
  connect(redirect, &QPushButton::clicked, this, [this]() { redirectYoutube(); });

  auto* add_rtsp = new QPushButton("Add RTSP Mount");
  add_rtsp->setObjectName("addRtspButton");
  connect(add_rtsp, &QPushButton::clicked, this, [this]() { addRtspOutput(); });

  layout->addWidget(redirect);
  layout->addWidget(add_rtsp);
  parent->addWidget(group);
}

void HmStreamWindow::buildCameraControls(QVBoxLayout* parent) {
  auto* group = new QGroupBox("Camera Controls");
  group->setObjectName("cameraControlsGroup");
  auto* layout = new QVBoxLayout(group);

  auto* camera_selector = new QComboBox();
  camera_selector->setObjectName("cameraSelector");
  camera_selector->addItems({"cam1", "cam2", "cam3"});
  layout->addWidget(camera_selector);

  camera_tabs_ = new QTabWidget();
  camera_tabs_->setObjectName("cameraControlTabs");

  auto* exposure = new QWidget();
  auto* exposure_layout = new QVBoxLayout(exposure);
  addSlider(exposure_layout, "exposure", "Exposure compensation", -40, 40, -13);
  addSlider(exposure_layout, "isoLimit", "ISO limit", 100, 6400, 1600);
  addSlider(exposure_layout, "shutter", "Shutter denominator", 30, 480, 120);
  exposure_layout->addStretch(1);

  auto* color = new QWidget();
  auto* color_layout = new QVBoxLayout(color);
  addSlider(color_layout, "whiteBalance", "White balance", 2500, 9000, 4300);
  addSlider(color_layout, "saturation", "Saturation", 0, 200, 100);
  addSlider(color_layout, "contrast", "Contrast", 0, 200, 100);
  color_layout->addStretch(1);

  auto* stitch = new QWidget();
  auto* stitch_layout = new QVBoxLayout(stitch);
  addSlider(stitch_layout, "stitchYaw", "Stitch yaw", -150, 150, 24);
  addSlider(stitch_layout, "stitchPitch", "Stitch pitch", -150, 150, 0);
  addSlider(stitch_layout, "stitchRoll", "Stitch roll", -150, 150, 0);
  stitch_layout->addStretch(1);

  auto* plugin = new QWidget();
  auto* plugin_layout = new QVBoxLayout(plugin);
  auto* property = new QLineEdit("nvarguscamerasrc.exposuretimerange");
  property->setObjectName("pluginPropertyEdit");
  plugin_layout->addWidget(new QLabel("GStreamer property"));
  plugin_layout->addWidget(property);
  plugin_layout->addStretch(1);

  camera_tabs_->addTab(exposure, "Exposure");
  camera_tabs_->addTab(color, "Color");
  camera_tabs_->addTab(stitch, "Stitch");
  camera_tabs_->addTab(plugin, "Plugin");
  layout->addWidget(camera_tabs_);
  parent->addWidget(group, 1);
}

void HmStreamWindow::buildLog(QVBoxLayout* root) {
  log_ = new QPlainTextEdit();
  log_->setObjectName("runtimeLog");
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(250);
  log_->setMinimumHeight(110);
  root->addWidget(log_);
}

void HmStreamWindow::setPipelineRunning(bool running) {
  pipeline_state_->setText(running ? "DEMO PLAYING" : "DEMO STOPPED");
  preview_status_->setText(running ? "Demo backend running - preview not attached" : "Demo backend stopped");
  appendLog(running ? "demo pipeline started" : "demo pipeline stopped");
}

void HmStreamWindow::restartStage() {
  appendLog("stage restart requested");
  pipeline_state_->setText("RESTARTING");
  pipeline_state_->setText("DEMO PLAYING");
}

void HmStreamWindow::savePreset() {
  appendLog("runtime camera/output preset saved");
}

void HmStreamWindow::resetCameraControls() {
  const std::map<QString, int> defaults = {
      {"exposure", -13},
      {"isoLimit", 1600},
      {"shutter", 120},
      {"whiteBalance", 4300},
      {"saturation", 100},
      {"contrast", 100},
      {"stitchYaw", 24},
      {"stitchPitch", 0},
      {"stitchRoll", 0},
  };
  for (const auto& [id, value] : defaults) {
    const auto it = camera_sliders_.find(id);
    if (it != camera_sliders_.end()) {
      it->second->setValue(value);
    }
  }
  appendLog("camera controls reset to defaults");
}

void HmStreamWindow::toggleOutput(const QString& id, bool enabled) {
  output_states_[id]->setText(enabled ? "DEMO LIVE" : "STOPPED");
  appendLog(QString("demo output %1 %2").arg(id, enabled ? "started" : "stopped"));
}

void HmStreamWindow::redirectYoutube() {
  QCheckBox* toggle = output_toggles_["youtube-primary"];
  const bool was_blocked = toggle->blockSignals(true);
  toggle->setChecked(true);
  toggle->blockSignals(was_blocked);
  output_states_["youtube-primary"]->setText("REDIRECTED");
  appendLog("youtube-primary redirected with redacted RTMP destination in demo backend");
}

void HmStreamWindow::addRtspOutput() {
  ++dynamic_rtsp_count_;
  const QString id = QString("rtsp-dynamic-%1").arg(dynamic_rtsp_count_);
  auto* row = new QHBoxLayout();
  auto* toggle = new QCheckBox(QString("RTSP Mount /dynamic%1").arg(dynamic_rtsp_count_));
  toggle->setObjectName("outputToggle_" + id);
  toggle->setChecked(true);
  auto* state = make_value_label("outputState_" + id, "DEMO LIVE");
  output_toggles_[id] = toggle;
  output_states_[id] = state;
  connect(toggle, &QCheckBox::toggled, this, [this, id](bool enabled) { toggleOutput(id, enabled); });
  row->addWidget(toggle, 1);
  row->addWidget(state);
  output_list_->insertLayout(output_list_->count() - 2, row);
  appendLog(QString("demo rtsp server mount /dynamic%1 added").arg(dynamic_rtsp_count_));
}

void HmStreamWindow::appendLog(const QString& message) {
  log_->appendPlainText(QString("%1 %2").arg(timestamp(), message));
}

QSlider* HmStreamWindow::addSlider(
    QVBoxLayout* layout,
    const QString& id,
    const QString& label,
    int minimum,
    int maximum,
    int value) {
  auto* row = new QGridLayout();
  auto* name = new QLabel(label);
  auto* value_label = make_value_label("cameraValue_" + id, QString::number(value));
  auto* slider = new QSlider(Qt::Horizontal);
  slider->setObjectName("cameraSlider_" + id);
  slider->setRange(minimum, maximum);
  slider->setValue(value);
  camera_sliders_[id] = slider;
  connect(slider, &QSlider::valueChanged, this, [this, id, value_label](int new_value) {
    value_label->setText(QString::number(new_value));
    appendLog(QString("camera control %1=%2 apply=live").arg(id).arg(new_value));
  });
  row->addWidget(name, 0, 0);
  row->addWidget(value_label, 0, 1);
  row->addWidget(slider, 1, 0, 1, 2);
  layout->addLayout(row);
  return slider;
}
