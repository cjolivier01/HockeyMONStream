#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/Qt>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

namespace fs = std::filesystem;

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

bool is_video_file(const QString& path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == "mp4" || suffix == "mkv" || suffix == "mov" || suffix == "avi";
}

bool is_auto_chapter_file(const QString& file_name) {
  static const QRegularExpression gopro("^G[A-Z][0-9]{6}\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\\.(MP4|mp4)$");
  static const QRegularExpression left_right("(left|right)(-[0-9])?\\.mp4$");
  return gopro.match(file_name).hasMatch() || insta360.match(file_name).hasMatch() ||
      left_right.match(file_name).hasMatch();
}

bool is_root_auto_file(const QString& file_name) {
  return is_auto_chapter_file(file_name);
}

QString auto_file_family(const QString& file_name) {
  static const QRegularExpression gopro("^G[A-Z][0-9]{6}\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\\.(MP4|mp4)$");
  static const QRegularExpression left_right("(left|right)(-[0-9])?\\.mp4$");
  const QRegularExpressionMatch gopro_match = gopro.match(file_name);
  if (gopro_match.hasMatch()) {
    return "gopro-" + file_name.mid(4, 4);
  }
  const QRegularExpressionMatch insta360_match = insta360.match(file_name);
  if (insta360_match.hasMatch()) {
    return "insta360-" + file_name.mid(4, 15);
  }
  const QRegularExpressionMatch lr_match = left_right.match(file_name);
  if (lr_match.hasMatch()) {
    return "lr-" + lr_match.captured(1).toLower();
  }
  return {};
}

std::optional<QString> explicit_chapter_key(const QString& path) {
  const QString file_name = QFileInfo(path).fileName();
  static const QRegularExpression gopro("^G[A-Z]([0-9]{2})([0-9]{4})\\.(MP4|mp4)$");
  static const QRegularExpression insta360("^VID_([0-9]{8})_([0-9]{6})_([0-9]{3})\\.(MP4|mp4)$");
  static const QRegularExpression left_right("(left|right)(?:-([0-9]))?\\.mp4$");

  const QRegularExpressionMatch gopro_match = gopro.match(file_name);
  if (gopro_match.hasMatch()) {
    return QString("gopro:%1").arg(gopro_match.captured(1));
  }
  const QRegularExpressionMatch insta360_match = insta360.match(file_name);
  if (insta360_match.hasMatch()) {
    return QString("insta360:%1").arg(insta360_match.captured(3));
  }
  const QRegularExpressionMatch lr_match = left_right.match(file_name);
  if (lr_match.hasMatch()) {
    const QString chapter = lr_match.captured(2).isEmpty() ? "1" : lr_match.captured(2);
    return QString("lr:%1").arg(chapter);
  }
  return std::nullopt;
}

QString canonical_dir_path(const QString& path) {
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString existing_auto_cam_dir_for_source(const QDir& game_dir, const QFileInfo& source) {
  const QString family = auto_file_family(source.fileName());
  const QString source_parent = canonical_dir_path(source.absolutePath());
  if (family.isEmpty() || source_parent.isEmpty()) {
    return {};
  }

  std::map<QString, std::pair<QString, QString>> copied_auto_sources;
  const fs::path config_path = fs::path(game_dir.absolutePath().toStdString()) / "config.yaml";
  if (fs::exists(config_path)) {
    try {
      YAML::Node config = YAML::LoadFile(config_path.string());
      YAML::Node entries = config["hmstream_ui"]["auto_import_sources"];
      if (entries && entries.IsSequence()) {
        for (const auto& entry : entries) {
          if (!entry["path"] || !entry["family"] || !entry["source_parent"]) {
            continue;
          }
          copied_auto_sources[QString::fromStdString(entry["path"].as<std::string>())] = {
              QString::fromStdString(entry["family"].as<std::string>()),
              QString::fromStdString(entry["source_parent"].as<std::string>())};
        }
      }
    } catch (const std::exception&) {
      copied_auto_sources.clear();
    }
  }

  std::vector<QFileInfo> cam_dirs;
  const QFileInfoList dirs = game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
  for (const QFileInfo& dir : dirs) {
    if (cam_pattern.match(dir.fileName()).hasMatch()) {
      cam_dirs.push_back(dir);
    }
  }
  std::sort(cam_dirs.begin(), cam_dirs.end(), [&cam_pattern](const QFileInfo& a, const QFileInfo& b) {
    return cam_pattern.match(a.fileName()).captured(1).toInt() < cam_pattern.match(b.fileName()).captured(1).toInt();
  });

  for (const QFileInfo& cam_dir : cam_dirs) {
    const QFileInfoList files =
        QDir(cam_dir.filePath()).entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      if (auto_file_family(file.fileName()) != family) {
        continue;
      }
      QString target_parent;
      if (file.isSymLink()) {
        target_parent = canonical_dir_path(QFileInfo(file.symLinkTarget()).absolutePath());
      } else {
        const QString relative_path = game_dir.relativeFilePath(file.filePath());
        const auto metadata = copied_auto_sources.find(relative_path);
        if (metadata == copied_auto_sources.end() || metadata->second.first != family) {
          continue;
        }
        target_parent = metadata->second.second;
      }
      if (target_parent == source_parent) {
        return cam_dir.fileName();
      }
    }
  }
  return {};
}

QString sanitized_game_id(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "-");
  value.replace(QRegularExpression("-+"), "-");
  if (value.contains(QRegularExpression("^\\.+$"))) {
    value.clear();
  }
  return value;
}

QString role_label(const QString& role) {
  if (role == "left") {
    return "Left";
  }
  if (role == "center") {
    return "Center";
  }
  if (role == "right") {
    return "Right";
  }
  return "Auto";
}

bool is_explicit_role(const QString& role) {
  return role == "left" || role == "center" || role == "right";
}

QString normalized_config_video_path(const QDir& game_dir, const QString& path) {
  QFileInfo file(path);
  if (file.isAbsolute()) {
    const QString relative = game_dir.relativeFilePath(file.absoluteFilePath());
    if (!relative.startsWith("../") && relative != "..") {
      return relative;
    }
  }
  return path;
}

bool clear_stitching_frame_offsets(YAML::Node& config) {
  bool changed = false;
  YAML::Node game_stitching = config["game"]["stitching"];
  if (game_stitching && game_stitching["frame_offsets"]) {
    game_stitching.remove("frame_offsets");
    changed = true;
  }
  YAML::Node stitching = config["stitching"];
  if (stitching && stitching["frame_offsets"]) {
    stitching.remove("frame_offsets");
    changed = true;
  }
  return changed;
}

bool remove_yaml_key(YAML::Node parent, const char* key) {
  if (!parent || !parent[key]) {
    return false;
  }
  parent.remove(key);
  return true;
}

} // namespace

HmStreamWindow::HmStreamWindow(QWidget* parent) : QMainWindow(parent) {
  buildUi();
  refreshGames();
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

QString HmStreamWindow::gameIdText() const {
  return game_id_edit_ ? game_id_edit_->text() : QString();
}

QString HmStreamWindow::gameDirectoryText() const {
  return game_path_label_ ? game_path_label_->text() : QString();
}

int HmStreamWindow::videoSetCount() const {
  return video_set_list_ ? video_set_list_->count() : 0;
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
  buildGameControls(left_layout);
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

void HmStreamWindow::buildGameControls(QVBoxLayout* root) {
  auto* group = new QGroupBox("Game");
  group->setObjectName("gameSetupGroup");
  auto* layout = new QGridLayout(group);
  layout->setColumnStretch(1, 1);

  game_selector_ = new QComboBox();
  game_selector_->setObjectName("gameSelector");
  connect(game_selector_, &QComboBox::currentTextChanged, this, [this](const QString& game_id) {
    if (!game_id.isEmpty() && game_id != game_id_edit_->text()) {
      selectGame(game_id);
    }
  });

  game_id_edit_ = new QLineEdit();
  game_id_edit_->setObjectName("gameIdEdit");
  game_id_edit_->setPlaceholderText("game-id");
  connect(game_id_edit_, &QLineEdit::editingFinished, this, [this]() {
    game_id_edit_->setText(sanitized_game_id(game_id_edit_->text()));
    refreshVideoSets();
  });

  auto* create = new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton), "Create / Load");
  create->setObjectName("createGameButton");
  connect(create, &QPushButton::clicked, this, [this]() { createOrLoadGame(); });

  auto* refresh = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), "Refresh");
  refresh->setObjectName("refreshGamesButton");
  connect(refresh, &QPushButton::clicked, this, [this]() { refreshGames(); });

  game_path_label_ = new QLabel(gameRoot());
  game_path_label_->setObjectName("gamePathLabel");
  game_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

  layout->addWidget(new QLabel("Existing"), 0, 0);
  layout->addWidget(game_selector_, 0, 1);
  layout->addWidget(refresh, 0, 2);
  layout->addWidget(new QLabel("Game ID"), 1, 0);
  layout->addWidget(game_id_edit_, 1, 1);
  layout->addWidget(create, 1, 2);
  layout->addWidget(new QLabel("Path"), 2, 0);
  layout->addWidget(game_path_label_, 2, 1, 1, 2);

  auto* video_group = new QGroupBox("Video Sets");
  video_group->setObjectName("videoSetsGroup");
  auto* video_layout = new QGridLayout(video_group);
  video_layout->setColumnStretch(0, 1);

  video_path_edit_ = new QLineEdit();
  video_path_edit_->setObjectName("videoPathEdit");
  video_path_edit_->setPlaceholderText("/path/to/video.mp4");

  auto* browse = new QPushButton(style()->standardIcon(QStyle::SP_DirOpenIcon), "Browse");
  browse->setObjectName("browseVideoButton");
  connect(browse, &QPushButton::clicked, this, [this]() { browseVideoPath(); });

  auto* add = new QPushButton(style()->standardIcon(QStyle::SP_FileDialogNewFolder), "Add");
  add->setObjectName("addVideoButton");
  connect(add, &QPushButton::clicked, this, [this]() { addVideoPath(); });

  auto* remove = new QPushButton(style()->standardIcon(QStyle::SP_TrashIcon), "Remove");
  remove->setObjectName("removeVideoButton");
  connect(remove, &QPushButton::clicked, this, [this]() { removeSelectedVideoSet(); });

  role_auto_ = new QRadioButton("Auto");
  role_auto_->setObjectName("videoRole_auto");
  role_auto_->setChecked(true);
  role_left_ = new QRadioButton("Left");
  role_left_->setObjectName("videoRole_left");
  role_center_ = new QRadioButton("Center");
  role_center_->setObjectName("videoRole_center");
  role_right_ = new QRadioButton("Right");
  role_right_->setObjectName("videoRole_right");

  auto* role_group = new QButtonGroup(video_group);
  role_group->addButton(role_auto_);
  role_group->addButton(role_left_);
  role_group->addButton(role_center_);
  role_group->addButton(role_right_);

  auto* roles = new QHBoxLayout();
  roles->setSpacing(12);
  roles->addWidget(role_auto_);
  roles->addWidget(role_left_);
  roles->addWidget(role_center_);
  roles->addWidget(role_right_);
  roles->addStretch(1);

  video_set_list_ = new QListWidget();
  video_set_list_->setObjectName("videoSetList");
  video_set_list_->setMinimumHeight(92);

  video_layout->addWidget(video_path_edit_, 0, 0);
  video_layout->addWidget(browse, 0, 1);
  video_layout->addWidget(add, 0, 2);
  video_layout->addLayout(roles, 1, 0, 1, 3);
  video_layout->addWidget(video_set_list_, 2, 0, 1, 2);
  video_layout->addWidget(remove, 2, 2);

  layout->addWidget(video_group, 3, 0, 1, 3);
  root->addWidget(group);
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
  if (running) {
    appendLog(QString("demo pipeline started --game-id=%1").arg(game_id_edit_ ? game_id_edit_->text() : QString()));
  } else {
    appendLog("demo pipeline stopped");
  }
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

void HmStreamWindow::refreshGames() {
  if (!game_selector_) {
    return;
  }
  const QString current = game_id_edit_ ? game_id_edit_->text() : QString();
  const bool blocked = game_selector_->blockSignals(true);
  game_selector_->clear();

  QDir root(gameRoot());
  if (!root.exists()) {
    root.mkpath(".");
  }
  const QFileInfoList dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& dir : dirs) {
    game_selector_->addItem(dir.fileName());
  }
  game_selector_->blockSignals(blocked);

  if (!current.isEmpty()) {
    const int index = game_selector_->findText(current);
    if (index >= 0) {
      game_selector_->setCurrentIndex(index);
    }
  } else if (game_selector_->count() > 0) {
    selectGame(game_selector_->currentText());
  } else if (game_path_label_) {
    game_path_label_->setText(gameRoot());
  }
}

void HmStreamWindow::selectGame(const QString& game_id) {
  if (!game_id_edit_) {
    return;
  }
  game_id_edit_->setText(sanitized_game_id(game_id));
  if (game_path_label_) {
    game_path_label_->setText(gameDirectory(game_id_edit_->text()));
  }
  refreshVideoSets();
  appendLog(QString("game selected %1").arg(game_id_edit_->text()));
}

void HmStreamWindow::createOrLoadGame() {
  if (!ensureGameDirectory()) {
    return;
  }
  refreshGames();
  refreshVideoSets();
  appendLog(QString("game ready %1").arg(game_id_edit_->text()));
}

void HmStreamWindow::addVideoPath() {
  if (!video_path_edit_) {
    return;
  }
  const QString role = selectedVideoRole();
  if (!ensureGameDirectory()) {
    return;
  }
  if (is_explicit_role(role)) {
    const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
    if (fs::exists(config_path)) {
      try {
        YAML::LoadFile(config_path.string());
      } catch (const std::exception& exc) {
        appendLog(QString("could not update private config: %1").arg(exc.what()));
        return;
      }
    }
  }

  QString imported_relative_path;
  if (!importVideoPath(video_path_edit_->text(), &imported_relative_path)) {
    return;
  }

  if (!savePrivateConfigForRole(role, imported_relative_path)) {
    removeImportedVideoPath(imported_relative_path, isCopiedImport(imported_relative_path));
    return;
  }
  refreshVideoSets();
  appendLog(QString("video set added role=%1 path=%2").arg(role_label(role), imported_relative_path));
}

void HmStreamWindow::browseVideoPath() {
  const QString start_dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  const QString path = QFileDialog::getOpenFileName(
      this,
      "Add Video",
      start_dir.isEmpty() ? QDir::homePath() : start_dir,
      "Videos (*.mp4 *.MP4 *.mkv *.MKV *.mov *.MOV *.avi *.AVI)");
  if (!path.isEmpty() && video_path_edit_) {
    video_path_edit_->setText(path);
  }
}

void HmStreamWindow::removeSelectedVideoSet() {
  if (!video_set_list_) {
    return;
  }
  const int row = video_set_list_->currentRow();
  if (row < 0) {
    appendLog("select a video set before removing");
    return;
  }
  auto* item = video_set_list_->takeItem(row);
  if (!item) {
    return;
  }
  const QString role = item->data(Qt::UserRole).toString();
  const QString relative_path = item->data(Qt::UserRole + 1).toString();
  const bool copied_import = isCopiedImport(relative_path);
  const QString config_file = QDir(gameDirectory(game_id_edit_->text())).filePath("config.yaml");
  const bool had_config = QFile::exists(config_file);
  QByteArray original_config;
  if (had_config) {
    QFile file(config_file);
    if (file.open(QIODevice::ReadOnly)) {
      original_config = file.readAll();
    }
  }

  if (!removePrivateConfigForRole(role, relative_path)) {
    video_set_list_->insertItem(row, item);
    return;
  }
  if (!removeImportedVideoPath(relative_path, copied_import)) {
    if (had_config) {
      QFile file(config_file);
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(original_config);
      } else {
        appendLog(QString("failed to restore private config after remove failure %1").arg(config_file));
      }
    } else {
      QFile::remove(config_file);
    }
    video_set_list_->insertItem(row, item);
    refreshVideoSets();
    return;
  }

  appendLog(QString("video set removed role=%1 path=%2").arg(role_label(role), relative_path));
  delete item;
  refreshVideoSets();
}

void HmStreamWindow::refreshVideoSets() {
  if (!video_set_list_ || !game_id_edit_) {
    return;
  }
  video_set_list_->clear();
  const QString dir = gameDirectory(game_id_edit_->text());
  if (game_path_label_) {
    game_path_label_->setText(dir);
  }
  if (game_id_edit_->text().isEmpty() || !QDir(dir).exists()) {
    return;
  }

  std::set<QString> seen;
  std::set<QString> configured_paths;
  auto add_item = [&](const QString& role, const QString& path) {
    const QString key = role + "\n" + path;
    if (seen.count(key)) {
      return;
    }
    seen.insert(key);
    auto* item = new QListWidgetItem(QString("%1  %2").arg(role_label(role), path));
    item->setData(Qt::UserRole, role);
    item->setData(Qt::UserRole + 1, path);
    video_set_list_->addItem(item);
  };

  QDir game_dir(dir);
  const fs::path config_path = fs::path(dir.toStdString()) / "config.yaml";
  if (fs::exists(config_path)) {
    try {
      YAML::Node config = YAML::LoadFile(config_path.string());
      YAML::Node explicit_roles = config["hmstream_ui"]["video_roles"];
      for (const QString& role : {QString("left"), QString("center"), QString("right")}) {
        YAML::Node role_videos = explicit_roles[role.toStdString()];
        if (role_videos && role_videos.IsSequence()) {
          for (const auto& item : role_videos) {
            const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
            configured_paths.insert(path);
            add_item(role, path);
          }
        }
      }
      YAML::Node videos = config["game"]["videos"];
      for (const QString& role : {QString("left"), QString("right")}) {
        YAML::Node role_videos = videos[role.toStdString()];
        if (role_videos && role_videos.IsSequence()) {
          for (const auto& item : role_videos) {
            const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
            if (!configured_paths.count(path)) {
              add_item("auto", path);
            }
          }
        }
      }
    } catch (const std::exception& exc) {
      appendLog(QString("could not read private config: %1").arg(exc.what()));
    }
  }

  const QFileInfoList dirs = game_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  std::vector<QFileInfo> cam_dirs;
  const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
  for (const QFileInfo& dir_info : dirs) {
    if (cam_pattern.match(dir_info.fileName()).hasMatch()) {
      cam_dirs.push_back(dir_info);
    }
  }
  std::sort(cam_dirs.begin(), cam_dirs.end(), [](const QFileInfo& a, const QFileInfo& b) {
    const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
    return cam_pattern.match(a.fileName()).captured(1).toInt() < cam_pattern.match(b.fileName()).captured(1).toInt();
  });

  bool listed_cam_video = false;
  for (const QFileInfo& cam_dir : cam_dirs) {
    const QFileInfoList files =
        QDir(cam_dir.filePath()).entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      const QString relative_path = game_dir.relativeFilePath(file.filePath());
      if (is_auto_chapter_file(file.fileName()) && !configured_paths.count(relative_path)) {
        add_item("auto", relative_path);
        listed_cam_video = true;
      }
    }
  }

  if (!listed_cam_video) {
    const QFileInfoList files = game_dir.entryInfoList(QDir::Files | QDir::System | QDir::Readable, QDir::Name);
    for (const QFileInfo& file : files) {
      const QString relative_path = game_dir.relativeFilePath(file.filePath());
      if (is_root_auto_file(file.fileName()) && !configured_paths.count(relative_path)) {
        add_item("auto", relative_path);
      }
    }
  }
}

QString HmStreamWindow::selectedVideoRole() const {
  if (role_left_ && role_left_->isChecked()) {
    return "left";
  }
  if (role_center_ && role_center_->isChecked()) {
    return "center";
  }
  if (role_right_ && role_right_->isChecked()) {
    return "right";
  }
  return "auto";
}

QString HmStreamWindow::gameRoot() const {
  const QByteArray env = qgetenv("HM_GAME_DIR");
  if (!env.isEmpty()) {
    return QString::fromLocal8Bit(env);
  }
  return QDir::home().filePath("Videos");
}

QString HmStreamWindow::gameDirectory(const QString& game_id) const {
  if (game_id.isEmpty()) {
    return gameRoot();
  }
  return QDir(gameRoot()).filePath(game_id);
}

QString HmStreamWindow::relativeToGameDir(const QString& path) const {
  const QDir dir(gameDirectory(game_id_edit_->text()));
  return dir.relativeFilePath(path);
}

bool HmStreamWindow::ensureGameDirectory() {
  if (!game_id_edit_) {
    return false;
  }
  const QString game_id = sanitized_game_id(game_id_edit_->text());
  game_id_edit_->setText(game_id);
  if (game_id.isEmpty()) {
    appendLog("game id is required");
    return false;
  }
  QDir root(gameRoot());
  if (!root.exists() && !root.mkpath(".")) {
    appendLog(QString("failed to create game root %1").arg(root.path()));
    return false;
  }
  if (!root.exists(game_id) && !root.mkdir(game_id)) {
    appendLog(QString("failed to create game %1").arg(game_id));
    return false;
  }
  if (game_path_label_) {
    game_path_label_->setText(gameDirectory(game_id));
  }
  return true;
}

bool HmStreamWindow::importVideoPath(const QString& source_path, QString* imported_relative_path) {
  if (!ensureGameDirectory()) {
    return false;
  }
  const QFileInfo source(source_path);
  if (!source.exists() || !source.isFile()) {
    appendLog(QString("video file not found %1").arg(source_path));
    return false;
  }
  if (!is_video_file(source.fileName())) {
    appendLog(QString("unsupported video extension %1").arg(source.fileName()));
    return false;
  }
  const QString role = selectedVideoRole();
  if (role == "auto" && !is_auto_chapter_file(source.fileName())) {
    appendLog(QString("auto video file name is not discoverable by the pipeline %1").arg(source.fileName()));
    return false;
  }

  const QString game_dir = gameDirectory(game_id_edit_->text());
  QDir target_dir(game_dir);
  int max_cam_index = 0;
  if (role == "auto") {
    const QFileInfoList dirs = target_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QRegularExpression cam_pattern("^cam([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
    for (const QFileInfo& dir : dirs) {
      const QRegularExpressionMatch match = cam_pattern.match(dir.fileName());
      if (match.hasMatch()) {
        max_cam_index = std::max(max_cam_index, match.captured(1).toInt());
      }
    }
    QString cam_dir = existing_auto_cam_dir_for_source(target_dir, source);
    if (cam_dir.isEmpty()) {
      cam_dir = QString("cam%1").arg(max_cam_index + 1);
    }
    if (!target_dir.exists(cam_dir) && !target_dir.mkdir(cam_dir)) {
      appendLog(QString("failed to create video set directory %1").arg(cam_dir));
      return false;
    }
    target_dir.cd(cam_dir);
  } else if (is_explicit_role(role)) {
    const QString ui_dir = ".hmstream-ui";
    const QString role_dir = ui_dir + "/" + role;
    if (!target_dir.exists(ui_dir) && !target_dir.mkdir(ui_dir)) {
      appendLog(QString("failed to create UI metadata directory %1").arg(ui_dir));
      return false;
    }
    if (!target_dir.exists(role_dir) && !target_dir.mkpath(role_dir)) {
      appendLog(QString("failed to create %1 video directory %2").arg(role_label(role), role_dir));
      return false;
    }
    target_dir.cd(role_dir);
  }
  QString dest_name = source.fileName();
  QString dest_path = target_dir.filePath(dest_name);
  int suffix = 2;
  while (QFileInfo::exists(dest_path) && QFileInfo(dest_path).canonicalFilePath() != source.canonicalFilePath() &&
         role == "auto") {
    target_dir = QDir(game_dir);
    const QString cam_dir = QString("cam%1").arg(++max_cam_index);
    if (!target_dir.exists(cam_dir) && !target_dir.mkdir(cam_dir)) {
      appendLog(QString("failed to create video set directory %1").arg(cam_dir));
      return false;
    }
    target_dir.cd(cam_dir);
    dest_name = source.fileName();
    dest_path = target_dir.filePath(dest_name);
  }
  while (QFileInfo::exists(dest_path) && QFileInfo(dest_path).canonicalFilePath() != source.canonicalFilePath() &&
         role != "auto") {
    dest_name = QString("%1-%2.%3").arg(source.completeBaseName()).arg(suffix++).arg(source.suffix());
    dest_path = target_dir.filePath(dest_name);
  }

  if (!QFileInfo::exists(dest_path)) {
    try {
      fs::create_symlink(fs::path(source.absoluteFilePath().toStdString()), fs::path(dest_path.toStdString()));
    } catch (const std::exception& exc) {
      if (!QFile::copy(source.absoluteFilePath(), dest_path)) {
        appendLog(QString("failed to import video link or copy: %1").arg(exc.what()));
        return false;
      }
      const QString auto_group_family = role == "auto" ? auto_file_family(source.fileName()) : QString();
      const QString source_parent = role == "auto" ? canonical_dir_path(source.absolutePath()) : QString();
      if (!saveCopiedImport(relativeToGameDir(dest_path), auto_group_family, source_parent)) {
        QFile::remove(dest_path);
        return false;
      }
      appendLog(QString("video symlink unavailable; copied import to %1").arg(dest_name));
    }
  }

  *imported_relative_path = relativeToGameDir(dest_path);
  return true;
}

bool HmStreamWindow::saveCopiedImport(
    const QString& relative_path,
    const QString& auto_group_family,
    const QString& source_parent) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not update copied import metadata: %1").arg(exc.what()));
      return false;
    }
  }

  YAML::Node list = config["hmstream_ui"]["copied_imports"];
  if (!list || !list.IsSequence()) {
    config["hmstream_ui"]["copied_imports"] = YAML::Node(YAML::NodeType::Sequence);
    list = config["hmstream_ui"]["copied_imports"];
  }
  bool copied_exists = false;
  for (const auto& item : list) {
    if (QString::fromStdString(item.as<std::string>()) == relative_path) {
      copied_exists = true;
      break;
    }
  }
  if (!copied_exists) {
    list.push_back(relative_path.toStdString());
  }

  if (!auto_group_family.isEmpty() && !source_parent.isEmpty()) {
    YAML::Node sources = config["hmstream_ui"]["auto_import_sources"];
    if (!sources || !sources.IsSequence()) {
      config["hmstream_ui"]["auto_import_sources"] = YAML::Node(YAML::NodeType::Sequence);
      sources = config["hmstream_ui"]["auto_import_sources"];
    }
    bool source_exists = false;
    for (const auto& item : sources) {
      if (item["path"] && QString::fromStdString(item["path"].as<std::string>()) == relative_path) {
        source_exists = true;
        break;
      }
    }
    if (!source_exists) {
      YAML::Node entry(YAML::NodeType::Map);
      entry["path"] = relative_path.toStdString();
      entry["family"] = auto_group_family.toStdString();
      entry["source_parent"] = source_parent.toStdString();
      sources.push_back(entry);
    }
  }

  std::ofstream out(config_path);
  if (!out) {
    appendLog(QString("failed to write copied import metadata %1").arg(QString::fromStdString(config_path.string())));
    return false;
  }
  out << config << "\n";
  return true;
}

bool HmStreamWindow::isCopiedImport(const QString& relative_path) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  if (!fs::exists(config_path)) {
    return false;
  }
  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    YAML::Node list = config["hmstream_ui"]["copied_imports"];
    if (!list || !list.IsSequence()) {
      return false;
    }
    const QDir game_dir(gameDirectory(game_id_edit_->text()));
    for (const auto& item : list) {
      const QString path = normalized_config_video_path(game_dir, QString::fromStdString(item.as<std::string>()));
      if (path == relative_path) {
        return true;
      }
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

bool HmStreamWindow::syncRuntimeExplicitVideoConfig(YAML::Node& config) {
  bool changed = false;
  YAML::Node explicit_left = config["hmstream_ui"]["video_roles"]["left"];
  YAML::Node explicit_right = config["hmstream_ui"]["video_roles"]["right"];
  const bool has_left = explicit_left && explicit_left.IsSequence() && explicit_left.size() > 0;
  const bool has_right = explicit_right && explicit_right.IsSequence() && explicit_right.size() > 0;
  if (has_left && has_right) {
    std::map<QString, QString> left_by_chapter;
    std::map<QString, QString> right_by_chapter;
    bool parsed = true;
    for (const auto& item : explicit_left) {
      const QString path = QString::fromStdString(item.as<std::string>());
      const std::optional<QString> chapter = explicit_chapter_key(path);
      if (!chapter || left_by_chapter.count(*chapter)) {
        parsed = false;
        break;
      }
      left_by_chapter[*chapter] = path;
    }
    for (const auto& item : explicit_right) {
      const QString path = QString::fromStdString(item.as<std::string>());
      const std::optional<QString> chapter = explicit_chapter_key(path);
      if (!chapter || right_by_chapter.count(*chapter)) {
        parsed = false;
        break;
      }
      right_by_chapter[*chapter] = path;
    }
    if (parsed && !left_by_chapter.empty() && left_by_chapter.size() == right_by_chapter.size()) {
      bool same_chapters = true;
      for (const auto& [chapter, _] : left_by_chapter) {
        if (!right_by_chapter.count(chapter)) {
          same_chapters = false;
          break;
        }
      }
      if (same_chapters) {
        YAML::Node left_list(YAML::NodeType::Sequence);
        YAML::Node right_list(YAML::NodeType::Sequence);
        for (const auto& [chapter, left_path] : left_by_chapter) {
          left_list.push_back(left_path.toStdString());
          right_list.push_back(right_by_chapter.at(chapter).toStdString());
        }
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
    if (explicit_left.size() == 1 && explicit_right.size() == 1) {
      const QString left_path = QString::fromStdString(explicit_left[0].as<std::string>());
      const QString right_path = QString::fromStdString(explicit_right[0].as<std::string>());
      if (!explicit_chapter_key(left_path) && !explicit_chapter_key(right_path)) {
        YAML::Node left_list(YAML::NodeType::Sequence);
        YAML::Node right_list(YAML::NodeType::Sequence);
        left_list.push_back(left_path.toStdString());
        right_list.push_back(right_path.toStdString());
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
    if (explicit_left.size() == explicit_right.size()) {
      YAML::Node left_list(YAML::NodeType::Sequence);
      YAML::Node right_list(YAML::NodeType::Sequence);
      bool all_unparseable = true;
      for (size_t i = 0; i < explicit_left.size(); ++i) {
        const QString left_path = QString::fromStdString(explicit_left[i].as<std::string>());
        const QString right_path = QString::fromStdString(explicit_right[i].as<std::string>());
        if (explicit_chapter_key(left_path) || explicit_chapter_key(right_path)) {
          all_unparseable = false;
          break;
        }
        left_list.push_back(left_path.toStdString());
        right_list.push_back(right_path.toStdString());
      }
      if (all_unparseable && explicit_left.size() > 0) {
        config["game"]["videos"]["left"] = left_list;
        config["game"]["videos"]["right"] = right_list;
        return true;
      }
    }
  }

  changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
  changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
  if (has_left && has_right) {
    appendLog("explicit Left/Right runtime config will apply after both sides have matching chapter sets");
  } else {
    appendLog("explicit Left/Right selection will apply after both sides are assigned");
  }
  return changed;
}

bool HmStreamWindow::savePrivateConfigForRole(const QString& role, const QString& relative_path) {
  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  YAML::Node config;
  if (fs::exists(config_path)) {
    try {
      config = YAML::LoadFile(config_path.string());
    } catch (const std::exception& exc) {
      appendLog(QString("could not update private config: %1").arg(exc.what()));
      return false;
    }
  }

  bool changed = false;
  if (is_explicit_role(role)) {
    YAML::Node list = config["hmstream_ui"]["video_roles"][role.toStdString()];
    if (!list || !list.IsSequence()) {
      config["hmstream_ui"]["video_roles"][role.toStdString()] = YAML::Node(YAML::NodeType::Sequence);
      list = config["hmstream_ui"]["video_roles"][role.toStdString()];
      changed = true;
    }
    bool exists = false;
    for (const auto& item : list) {
      if (QString::fromStdString(item.as<std::string>()) == relative_path) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      list.push_back(relative_path.toStdString());
      changed = true;
    }
    changed = clear_stitching_frame_offsets(config) || changed;
  }

  if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
  }

  if (role == "auto") {
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
    appendLog("auto video set will be discovered from the game directory");
  }

  if (!changed) {
    return true;
  }

  std::ofstream out(config_path);
  if (!out) {
    appendLog(QString("failed to write private config %1").arg(QString::fromStdString(config_path.string())));
    return false;
  }
  if (config.IsDefined() && !config.IsNull()) {
    out << config << "\n";
  }
  return true;
}

bool HmStreamWindow::removePrivateConfigForRole(const QString& role, const QString& relative_path) {
  if (!is_explicit_role(role) && role != "auto") {
    return true;
  }

  const fs::path config_path = fs::path(gameDirectory(game_id_edit_->text()).toStdString()) / "config.yaml";
  if (!fs::exists(config_path)) {
    return true;
  }

  YAML::Node config;
  try {
    config = YAML::LoadFile(config_path.string());
  } catch (const std::exception& exc) {
    appendLog(QString("could not update private config: %1").arg(exc.what()));
    return false;
  }

  bool changed = false;
  auto matches_path = [&](const QString& value) {
    if (value == relative_path) {
      return true;
    }
    const QString normalized = normalized_config_video_path(QDir(gameDirectory(game_id_edit_->text())), value);
    return normalized == relative_path;
  };

  auto remove_from_list = [&](YAML::Node parent, const QString& key) {
    YAML::Node list = parent[key.toStdString()];
    if (!list || !list.IsSequence()) {
      return;
    }

    YAML::Node replacement(YAML::NodeType::Sequence);
    for (const auto& item : list) {
      const QString value = QString::fromStdString(item.as<std::string>());
      if (matches_path(value)) {
        changed = true;
      } else {
        replacement.push_back(value.toStdString());
      }
    }
    parent[key.toStdString()] = replacement;
  };

  auto remove_auto_source_metadata = [&]() {
    YAML::Node list = config["hmstream_ui"]["auto_import_sources"];
    if (!list || !list.IsSequence()) {
      return;
    }

    YAML::Node replacement(YAML::NodeType::Sequence);
    for (const auto& item : list) {
      if (item["path"] && matches_path(QString::fromStdString(item["path"].as<std::string>()))) {
        changed = true;
      } else {
        replacement.push_back(item);
      }
    }
    config["hmstream_ui"]["auto_import_sources"] = replacement;
  };

  if (is_explicit_role(role)) {
    remove_from_list(config["hmstream_ui"]["video_roles"], role);
  }
  remove_from_list(config["hmstream_ui"], "copied_imports");
  remove_auto_source_metadata();
  if (role == "auto") {
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "left") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "center") || changed;
    changed = remove_yaml_key(config["hmstream_ui"]["video_roles"], "right") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "left") || changed;
    changed = remove_yaml_key(config["game"]["videos"], "right") || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
  } else if (role == "left" || role == "right") {
    changed = syncRuntimeExplicitVideoConfig(config) || changed;
    changed = clear_stitching_frame_offsets(config) || changed;
  }
  if (!changed) {
    return true;
  }

  std::ofstream out(config_path);
  if (!out) {
    appendLog(QString("failed to write private config %1").arg(QString::fromStdString(config_path.string())));
    return false;
  }
  out << config << "\n";
  return true;
}

bool HmStreamWindow::removeImportedVideoPath(const QString& relative_path, bool allow_regular_delete) {
  const QDir game_dir(gameDirectory(game_id_edit_->text()));
  const QString game_root = QDir::cleanPath(game_dir.absolutePath());
  const QFileInfo requested(relative_path);
  const QString imported_path =
      requested.isAbsolute() ? requested.absoluteFilePath() : game_dir.absoluteFilePath(relative_path);
  const QString normalized_path = QDir::cleanPath(QFileInfo(imported_path).absoluteFilePath());
  if (normalized_path != game_root && !normalized_path.startsWith(game_root + "/")) {
    appendLog(QString("not deleting video outside game directory %1").arg(relative_path));
    return true;
  }
  const QFileInfo imported(imported_path);
  if (!imported.exists()) {
    return true;
  }
  if (!imported.isSymLink() && !allow_regular_delete) {
    appendLog(QString("not deleting regular video file %1").arg(relative_path));
    return true;
  }
  if (!QFile::remove(imported_path)) {
    appendLog(QString("failed to remove imported video link %1").arg(relative_path));
    return false;
  }
  return true;
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
