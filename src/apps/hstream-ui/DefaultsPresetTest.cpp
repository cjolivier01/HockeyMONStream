#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

#include <cstdlib>
#include <iostream>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"

struct HStreamWindowTestAccess {
  static void HighBit(HStreamWindow& window, const QString& mode = "1") {
    window.setHighBitDepthMode(mode);
  }
  static QString HighBitMode(HStreamWindow& window) {
    return window.highBitDepthMode();
  }
  static bool Save(HStreamWindow& window) {
    return window.savePreset();
  }
  static void Set(HStreamWindow& window, const QString& id, int value) {
    window.camera_sliders_.at(id)->setValue(value);
  }
};
namespace {
void Require(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
QByteArray Read(const QString& path) {
  QFile file(path);
  Require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
  return file.readAll();
}
void Write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  Require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot write fixture");
}
template <class T>
T* Find(HStreamWindow& window, const char* name) {
  auto* widget = window.findChild<T*>(name);
  Require(widget != nullptr, name);
  return widget;
}
} // namespace
int main(int argc, char** argv) {
  QTemporaryDir home;
  QTemporaryDir games;
  Require(home.isValid() && games.isValid(), "Cannot create isolated test directories");
  qputenv("HOME", home.path().toLocal8Bit());
  qputenv("XDG_CONFIG_HOME", home.filePath("settings").toLocal8Bit());
  qputenv("HM_GAME_DIR", games.path().toLocal8Bit());
  QApplication app(argc, argv);
  const auto baseline = hm::baseline_config::load();
  Require(baseline.ok(), "Cannot load bundled baseline");
  const auto baseline_path = QString::fromStdString(baseline->path.string());
  const auto original_baseline = Read(baseline_path);
  const auto defaults_path = home.filePath(".config/hstream-ui/defaults.yaml");
  QDir().mkpath(games.filePath("inherit"));
  QDir().mkpath(games.filePath("override"));
  QDir().mkpath(games.filePath("Defaults"));
  Write(games.filePath("inherit/config.yaml"), "{}\n");
  Write(games.filePath("override/config.yaml"), "stitching: {max_output_width: 6200}\n");
  Write(games.filePath("Defaults/config.yaml"), "stitching: {max_output_width: 6400}\n");
  const auto original_game = Read(games.filePath("override/config.yaml"));
  QDir().mkpath(home.filePath(".hstream"));
  Write(
      home.filePath(".hstream/hstream.yaml"),
      "pipeline: {hmstitcher: {blend-mode: gpu-hard-seam}}\n"
      "rink: {camera: {zoom_in_aggressiveness: 37}}\n");
  const auto legacy_preferences = Read(home.filePath(".hstream/hstream.yaml"));
  HStreamWindow window;
  auto* selector = Find<QComboBox>(window, "gameSelector");
  auto* width = Find<QSpinBox>(window, "stitchMaxOutputWidthSpin");
  auto* save = Find<QPushButton>(window, "savePresetButton");
  auto* reset = Find<QPushButton>(window, "resetDefaultsButton");
  Require(selector->itemText(0) == "Defaults", "Defaults must be a distinct selector item");
  selector->setCurrentIndex(0);
  Require(!reset->isHidden() && reset->isEnabled(), "Reset must be available for Defaults");
  Require(window.gameIdText().isEmpty(), "Defaults must not masquerade as a game");
  Require(!Find<QPushButton>(window, "startPipelineButton")->isEnabled(), "Defaults cannot run a pipeline");
  Require(!QFile::exists(defaults_path), "Selecting Defaults must not create a preset");
  const int baseline_width = width->value();
  const auto baseline_zoom = window.cameraControlValue("Zoom_In_Aggressiveness");
  width->setValue(6000);
  auto* blend = Find<QComboBox>(window, "blendModeCombo");
  blend->setCurrentIndex(blend->findData("alpha"));
  Require(save->isEnabled() && HStreamWindowTestAccess::Save(window), "Cannot save default width");
  Require(!save->isEnabled(), "Saved Defaults must be clean");
  auto defaults = YAML::LoadFile(defaults_path.toStdString());
  Require(defaults["stitching"]["max_output_width"].as<int>() == 6000, "Defaults saved to wrong path/value");
  Require(Read(baseline_path) == original_baseline, "Saving defaults changed baseline.yaml");
  selector->setCurrentIndex(selector->findText("inherit"));
  Require(
      width->value() == 6000 && reset->isHidden() && blend->currentData() == "alpha",
      "Game did not inherit defaults or old native blend alias overrode Defaults");
  selector->setCurrentIndex(selector->findText("override"));
  Require(width->value() == 6200, "Game override must outrank defaults");
  // A real game called Defaults is distinct from the tagged entry.
  for (int i = 1; i < selector->count(); ++i) {
    if (selector->itemText(i) == "Defaults") {
      selector->setCurrentIndex(i);
      Require(width->value() == 6400 && !window.gameIdText().isEmpty(), "Defaults game collided with special entry");
    }
  }
  selector->setCurrentIndex(0);
  Require(width->value() == 6000, "Selecting Defaults leaked game controls");
  Find<QSpinBox>(window, "calibrationFrameCountSpin")->setValue(6);
  Find<QSpinBox>(window, "controlPointsSpin")->setValue(1000);
  HStreamWindowTestAccess::HighBit(window);
  HStreamWindowTestAccess::Set(window, "Exposure_x100", 110);
  Require(HStreamWindowTestAccess::Save(window), "Cannot save color and calibration defaults");
  selector->setCurrentIndex(selector->findText("inherit"));
  Require(
      Find<QSpinBox>(window, "calibrationFrameCountSpin")->value() == 6 &&
          Find<QSpinBox>(window, "controlPointsSpin")->value() == 1000 &&
          window.cameraControlValue("Exposure_x100") == 110 && window.cameraControlValue("Use_10_Bit_Grading") == 1,
      "Game failed to inherit calibration and color defaults");
  Find<QSpinBox>(window, "calibrationFrameCountSpin")->setValue(4);
  HStreamWindowTestAccess::HighBit(window, "auto");
  Require(HStreamWindowTestAccess::Save(window), "Cannot override default frame count/precision in a game");
  auto saved_game = YAML::LoadFile(games.filePath("inherit/config.yaml").toStdString());
  Require(
      saved_game["stitching"]["calibration_frame_count"].as<int>() == 4 &&
          saved_game["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"].as<std::string>() == "auto",
      "Game overrides were removed because they equal the shipped baseline");
  saved_game["pipeline"]["hmplaycropper"]["properties"]["exposure"] = 0.9;
  saved_game["hstream_ui"].remove("camera_controls");
  Write(games.filePath("inherit/config.yaml"), QByteArray::fromStdString(YAML::Dump(saved_game)));
  selector->setCurrentIndex(0);
  selector->setCurrentIndex(selector->findText("inherit"));
  Require(
      Find<QSpinBox>(window, "calibrationFrameCountSpin")->value() == 4 &&
          HStreamWindowTestAccess::HighBitMode(window) == "auto" && window.cameraControlValue("Exposure_x100") == 90,
      "Inherited values overrode explicit game frame count, Auto precision or other-owner tone");
  Write(games.filePath("inherit/config.yaml"), "{}\n");
  selector->setCurrentIndex(0);
  const auto before_reset = Read(defaults_path);
  reset->click();
  Require(width->value() == baseline_width && save->isEnabled(), "Reset did not stage baseline settings");
  Require(Read(defaults_path) == before_reset, "Reset wrote defaults before Save Preset");
  Require(HStreamWindowTestAccess::Save(window), "Cannot save reset defaults");
  defaults = YAML::LoadFile(defaults_path.toStdString());
  Require(defaults.IsMap() && defaults.size() == 0, "Reset and Save must leave an empty overlay");
  selector->setCurrentIndex(selector->findText("inherit"));
  Require(
      width->value() == baseline_width && window.cameraControlValue("Zoom_In_Aggressiveness") == baseline_zoom,
      "Game retained cleared defaults or resurrected legacy tuning");
  Require(Read(home.filePath(".hstream/hstream.yaml")) == legacy_preferences, "Defaults modified legacy preferences");
  selector->setCurrentIndex(0);
  reset->click();
  width->setValue(5800);
  Require(HStreamWindowTestAccess::Save(window), "Cannot edit after reset");
  Require(
      YAML::LoadFile(defaults_path.toStdString())["stitching"]["max_output_width"].as<int>() == 5800,
      "Reset discarded subsequent edits");
  // Tracker controls need a native config too; it must be user-owned, not game-local.
  HStreamWindowTestAccess::Set(window, "Max_Speed_X_x10", 20);
  Require(HStreamWindowTestAccess::Save(window), "Cannot save native tracker defaults");
  defaults = YAML::LoadFile(defaults_path.toStdString());
  const auto tracker = defaults["pipeline"]["ds-playtracker"]["config-file"].as<std::string>();
  Require(
      QString::fromStdString(tracker).startsWith(home.filePath(".config/hstream-ui/")),
      "Default tracker file escaped the user config directory");
  Require(QFile::exists(QString::fromStdString(tracker)), "Default tracker reference is missing");
  Require(Read(games.filePath("override/config.yaml")) == original_game, "Defaults edits modified a game");
  Require(Read(baseline_path) == original_baseline, "Defaults/reset modified baseline.yaml");
  // A fresh window must load the same settings.
  HStreamWindow reopened;
  Find<QComboBox>(reopened, "gameSelector")->setCurrentIndex(0);
  Require(
      Find<QSpinBox>(reopened, "stitchMaxOutputWidthSpin")->value() == 5800 &&
          reopened.cameraControlValue("Max_Speed_X_x10") == 20,
      "Restart lost defaults controls");
  HStreamWindowTestAccess::Set(reopened, "Max_Speed_X_x10", 0);
  Require(HStreamWindowTestAccess::Save(reopened), "Cannot restore one tracker default");
  defaults = YAML::LoadFile(defaults_path.toStdString());
  Require(
      !defaults["pipeline"] || !defaults["pipeline"]["ds-playtracker"] ||
          !defaults["pipeline"]["ds-playtracker"]["config-file"],
      "Restoring tracker controls left a stale native override");
  std::cout << "Defaults preset tests passed\n";
  return 0;
}
