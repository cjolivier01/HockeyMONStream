#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

template <typename T>
T* require_child(HmStreamWindow* window, const char* name) {
  T* child = window->findChild<T*>(name);
  if (!child) {
    std::cerr << "Missing child: " << name << '\n';
  }
  return child;
}

void activate(QAbstractButton* button) {
  button->click();
  QApplication::processEvents();
}

bool select_list_item(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text().contains(text)) {
      list->setCurrentRow(i);
      return true;
    }
  }
  std::cerr << "Missing list item containing: " << text.toStdString() << '\n';
  return false;
}

bool list_contains(QListWidget* list, const QString& text) {
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text().contains(text)) {
      return true;
    }
  }
  return false;
}

bool write_fake_video(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cerr << "Failed to create fake video: " << path.toStdString() << '\n';
    return false;
  }
  file.write("hmstream-ui-test-video");
  return true;
}

bool test_game_setup(HmStreamWindow* window, const QString& source_dir) {
  auto* game_id = require_child<QLineEdit>(window, "gameIdEdit");
  auto* create = require_child<QPushButton>(window, "createGameButton");
  auto* video_path = require_child<QLineEdit>(window, "videoPathEdit");
  auto* add_video = require_child<QPushButton>(window, "addVideoButton");
  auto* remove_video = require_child<QPushButton>(window, "removeVideoButton");
  auto* center = require_child<QRadioButton>(window, "videoRole_center");
  auto* right = require_child<QRadioButton>(window, "videoRole_right");
  auto* list = require_child<QListWidget>(window, "videoSetList");
  if (!game_id || !create || !video_path || !add_video || !remove_video || !center || !right || !list) {
    return false;
  }

  game_id->setText("..");
  activate(create);
  if (!expect(window->gameIdText().isEmpty(), "Unsafe game IDs should be rejected")) {
    return false;
  }

  const QString auto_video = source_dir + "/GX010001.MP4";
  const QString center_video = source_dir + "/GX010003.MP4";
  const QString right_video = source_dir + "/GX010002.MP4";
  if (!write_fake_video(auto_video) || !write_fake_video(center_video) || !write_fake_video(right_video)) {
    return false;
  }

  game_id->setText("ui-test-game");
  activate(create);
  if (!expect(window->gameIdText() == "ui-test-game", "Game ID field should define the selected game")) {
    return false;
  }

  const fs::path cam_dir = fs::path(window->gameDirectoryText().toStdString()) / "cam1";
  fs::create_directories(cam_dir);
  if (!write_fake_video(QString::fromStdString((cam_dir / "GX010004.MP4").string()))) {
    return false;
  }
  activate(create);
  if (!expect(list_contains(list, "Auto  cam1/GX010004.MP4"), "Auto listing should include camN video sets")) {
    return false;
  }

  video_path->setText(auto_video);
  activate(add_video);
  if (!expect(window->videoSetCount() >= 1, "Adding an auto video should populate the video set list")) {
    return false;
  }
  if (!expect(
          fs::exists(fs::path(window->gameDirectoryText().toStdString()) / "GX010001.MP4"),
          "Auto video should be imported into the game directory for existing discovery")) {
    return false;
  }

  activate(center);
  video_path->setText(center_video);
  activate(add_video);
  if (!expect(select_list_item(list, "Center  GX010003.MP4"), "Explicit Center assignment should remain visible")) {
    return false;
  }

  activate(right);
  video_path->setText(right_video);
  activate(add_video);
  const fs::path config = fs::path(window->gameDirectoryText().toStdString()) / "config.yaml";
  std::ifstream input(config);
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  YAML::Node yaml = YAML::LoadFile(config.string());
  if (!expect(
          yaml["hmstream_ui"]["video_roles"]["center"] &&
              yaml["hmstream_ui"]["video_roles"]["center"][0].as<std::string>() == "GX010003.MP4" &&
              !yaml["game"]["videos"]["center"] && text.find("right") != std::string::npos &&
              text.find("GX010002.MP4") != std::string::npos,
          "Explicit Center should be UI metadata while Right remains pipeline config")) {
    return false;
  }

  if (!select_list_item(list, "Right  GX010002.MP4")) {
    return false;
  }
  activate(remove_video);
  std::ifstream updated_input(config);
  const std::string updated_text(
      (std::istreambuf_iterator<char>(updated_input)), std::istreambuf_iterator<char>());
  return expect(
             updated_text.find("GX010002.MP4") == std::string::npos,
             "Removing an explicit role should update private config") &&
         expect(!list_contains(list, "GX010002.MP4"), "Removed explicit imports should not reappear as Auto");
}

bool test_pipeline_buttons(HmStreamWindow* window) {
  auto* stop = require_child<QPushButton>(window, "stopPipelineButton");
  auto* start = require_child<QPushButton>(window, "startPipelineButton");
  auto* restart = require_child<QPushButton>(window, "restartStageButton");
  if (!stop || !start || !restart) {
    return false;
  }

  activate(stop);
  if (!expect(window->pipelineStateText() == "DEMO STOPPED", "Stop button should stop the demo pipeline")) {
    return false;
  }

  activate(start);
  if (!expect(window->pipelineStateText() == "DEMO PLAYING", "Start button should start the demo pipeline")) {
    return false;
  }

  activate(restart);
  return expect(window->logText().contains("stage restart requested"), "Restart button should log a stage restart");
}

bool test_output_controls(HmStreamWindow* window) {
  auto* spare = require_child<QCheckBox>(window, "outputToggle_spare-rtmp");
  auto* youtube_redirect = require_child<QPushButton>(window, "redirectYoutubeButton");
  auto* add_rtsp = require_child<QPushButton>(window, "addRtspButton");
  if (!spare || !youtube_redirect || !add_rtsp) {
    return false;
  }

  activate(spare);
  if (!expect(window->outputStateText("spare-rtmp") == "DEMO LIVE", "Spare RTMP toggle should start the output")) {
    return false;
  }

  activate(youtube_redirect);
  if (!expect(
          window->outputStateText("youtube-primary") == "REDIRECTED",
          "Redirect button should mark YouTube output redirected")) {
    return false;
  }

  activate(add_rtsp);
  return expect(
      window->outputStateText("rtsp-dynamic-1") == "DEMO LIVE",
      "Add RTSP button should create a live dynamic RTSP output");
}

bool test_camera_controls(HmStreamWindow* window) {
  if (!expect(window->cameraTabCount() >= 4, "Camera controls should be grouped on tabs")) {
    return false;
  }

  auto* exposure = require_child<QSlider>(window, "cameraSlider_exposure");
  auto* yaw = require_child<QSlider>(window, "cameraSlider_stitchYaw");
  auto* reset = require_child<QPushButton>(window, "resetCameraButton");
  auto* save = require_child<QPushButton>(window, "savePresetButton");
  if (!exposure || !yaw || !reset || !save) {
    return false;
  }

  exposure->setValue(5);
  yaw->setValue(-20);
  if (!expect(window->cameraControlValue("exposure") == 5, "Exposure slider should update controller state") ||
      !expect(window->cameraControlValue("stitchYaw") == -20, "Stitch yaw slider should update controller state")) {
    return false;
  }

  activate(save);
  if (!expect(window->logText().contains("preset saved"), "Save preset button should log persistence")) {
    return false;
  }

  activate(reset);
  return expect(window->cameraControlValue("exposure") == -13, "Reset should restore exposure default");
}

} // namespace

int main(int argc, char** argv) {
  QTemporaryDir game_root;
  QTemporaryDir source_root;
  if (!game_root.isValid() || !source_root.isValid()) {
    return 1;
  }
  qputenv("HM_GAME_DIR", game_root.path().toLocal8Bit());

  QApplication app(argc, argv);
  HmStreamWindow window;
  window.show();

  if (!test_game_setup(&window, source_root.path()) || !test_pipeline_buttons(&window) ||
      !test_output_controls(&window) || !test_camera_controls(&window)) {
    return 1;
  }
  return 0;
}
