#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtTest/qtest_widgets.h>
#include <QtTest/qtestmouse.h>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>

#include <iostream>
#include <string>

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
  QApplication app(argc, argv);
  HmStreamWindow window;
  window.show();

  if (!test_pipeline_buttons(&window) || !test_output_controls(&window) || !test_camera_controls(&window)) {
    return 1;
  }
  return 0;
}
