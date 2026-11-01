#include "src/apps/hstream-ui/HStreamWindow.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"

struct HStreamWindowTestAccess {
  static void HighBit(HStreamWindow& window, const QString& mode = "1") {
    window.setHighBitDepthMode(mode);
  }
  static QString HighBitMode(HStreamWindow& window) {
    return window.highBitDepthMode();
  }
  static void PrecisionCycle(HStreamWindow& window) {
    auto* program = window.camera_checkboxes_.at("Use_10_Bit_Grading");
    auto* stitched = window.stitched_color_checkboxes_.at("Use_10_Bit_Grading");
    window.setHighBitDepthMode("auto");
    for (const auto state : {Qt::Checked, Qt::Unchecked, Qt::PartiallyChecked}) {
      program->click();
      if (program->checkState() != state || stitched->checkState() != state)
        throw std::runtime_error("Program precision cycle or mirror changed");
    }
    for (const auto state : {Qt::Checked, Qt::Unchecked, Qt::PartiallyChecked}) {
      stitched->click();
      if (program->checkState() != state || stitched->checkState() != state)
        throw std::runtime_error("Stitched precision cycle or mirror changed");
    }
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
  auto original_game = Read(games.filePath("override/config.yaml"));
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
  Require(HStreamWindowTestAccess::HighBitMode(window) == "auto", "Shipped precision must resolve to Auto");
  HStreamWindowTestAccess::PrecisionCycle(window);
  auto* output_map = Find<QCheckBox>(window, "outputRinkMapCheck");
  auto* output_size = Find<QComboBox>(window, "outputRinkMapSizeCombo");
  auto* output_position = Find<QComboBox>(window, "outputRinkMapPositionCombo");
  Require(!output_map->isChecked(), "Output map should start disabled");
  output_map->setChecked(true);
  output_size->setCurrentIndex(output_size->findData("large"));
  output_position->setCurrentIndex(output_position->findData("lower-left"));
  const int baseline_width = width->value();
  const auto baseline_zoom = window.cameraControlValue("Zoom_In_Aggressiveness");
  width->setValue(6000);
  auto* blend = Find<QComboBox>(window, "blendModeCombo");
  blend->setCurrentIndex(blend->findData("alpha"));
  Require(save->isEnabled() && HStreamWindowTestAccess::Save(window), "Cannot save default width");
  Require(!save->isEnabled(), "Saved Defaults must be clean");
  auto defaults = YAML::LoadFile(defaults_path.toStdString());
  Require(defaults["stitching"]["max_output_width"].as<int>() == 6000, "Defaults saved to wrong path/value");
  Require(
      defaults["output"]["rink-map"]["enable"].as<bool>() &&
          defaults["output"]["rink-map"]["size"].as<std::string>() == "large" &&
          defaults["output"]["rink-map"]["position"].as<std::string>() == "lower-left",
      "Output map was not saved to Defaults");
  Require(Read(baseline_path) == original_baseline, "Saving defaults changed baseline.yaml");
  selector->setCurrentIndex(selector->findText("inherit"));
  Require(
      width->value() == 6000 && reset->isHidden() && blend->currentData() == "alpha",
      "Game did not inherit defaults or old native blend alias overrode Defaults");
  selector->setCurrentIndex(selector->findText("override"));
  Require(width->value() == 6200, "Game override must outrank defaults");
  Require(output_map->isChecked() && output_size->currentData() == "large", "Game did not inherit output map");
  output_map->setChecked(false);
  Require(save->isEnabled() && HStreamWindowTestAccess::Save(window), "Cannot save game map override");
  Require(
      !YAML::LoadFile(games.filePath("override/config.yaml").toStdString())["output"]["rink-map"]["enable"].as<bool>(),
      "Game cannot disable inherited output map");
  original_game = Read(games.filePath("override/config.yaml"));
  // A real game called Defaults is distinct from the tagged entry.
  for (int i = 1; i < selector->count(); ++i) {
    if (selector->itemText(i) == "Defaults") {
      selector->setCurrentIndex(i);
      Require(width->value() == 6400 && !window.gameIdText().isEmpty(), "Defaults game collided with special entry");
    }
  }
  selector->setCurrentIndex(0);
  Require(width->value() == 6000, "Selecting Defaults leaked game controls");
  Require(output_map->isChecked(), "Game map override leaked into Defaults");
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
  Require(!output_map->isChecked(), "Reset did not clear output map");
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
  // A configured baseline may force either mode. Auto must still be a saved
  // explicit override, rather than disappearing and restoring that forced mode.
  QTemporaryDir custom_baseline;
  Require(custom_baseline.isValid(), "Cannot create precision baseline fixture");
  qputenv("HM_CONFIG_ROOT", custom_baseline.path().toLocal8Bit());
  for (const char* mode : {"1", "0"}) {
    YAML::Node custom = YAML::Load(original_baseline.toStdString());
    custom["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = std::string(mode);
    Write(custom_baseline.filePath("baseline.yaml"), QByteArray::fromStdString(YAML::Dump(custom)));
    Write(defaults_path, "{}\n");
    Write(games.filePath("inherit/config.yaml"), "{}\n");
    HStreamWindow forced;
    auto* forced_selector = Find<QComboBox>(forced, "gameSelector");
    forced_selector->setCurrentIndex(0);
    Require(HStreamWindowTestAccess::HighBitMode(forced) == mode, "Defaults ignored configured precision");
    forced_selector->setCurrentIndex(forced_selector->findText("inherit"));
    Require(HStreamWindowTestAccess::HighBitMode(forced) == mode, "Game ignored baseline precision");
    HStreamWindowTestAccess::HighBit(forced, "auto");
    Require(HStreamWindowTestAccess::Save(forced), "Cannot save Auto over a forced baseline");
    forced_selector->setCurrentIndex(0);
    forced_selector->setCurrentIndex(forced_selector->findText("inherit"));
    Require(HStreamWindowTestAccess::HighBitMode(forced) == "auto", "Auto override was lost on reload");
    forced_selector->setCurrentIndex(0);
    HStreamWindowTestAccess::HighBit(forced, "auto");
    Require(HStreamWindowTestAccess::Save(forced), "Cannot save Auto Defaults over a forced baseline");
    HStreamWindow custom_reopened;
    Find<QComboBox>(custom_reopened, "gameSelector")->setCurrentIndex(0);
    Require(HStreamWindowTestAccess::HighBitMode(custom_reopened) == "auto", "Saved Auto Defaults were lost");
    Find<QPushButton>(custom_reopened, "resetDefaultsButton")->click();
    Require(HStreamWindowTestAccess::HighBitMode(custom_reopened) == mode, "Reset ignored configured precision");
    Require(HStreamWindowTestAccess::Save(custom_reopened), "Cannot reset configured precision");
    const auto reset_defaults = YAML::LoadFile(defaults_path.toStdString());
    Require(reset_defaults.IsMap() && reset_defaults.size() == 0, "Precision reset left an override");
  }
  // Prompt policy comes from the baseline, and each editor must retain explicit
  // false values above it. Reference time is a saved preference too.
  for (const bool baseline_crop : {false, true}) {
    YAML::Node custom = YAML::Load(original_baseline.toStdString());
    custom["hstream_ui"]["show_crop_dialog"] = baseline_crop;
    custom["hstream_ui"]["show_leveling_dialog"] = !baseline_crop;
    custom["stitching"]["stitch_frame_time"] = "00:00:02.345";
    Write(custom_baseline.filePath("baseline.yaml"), QByteArray::fromStdString(YAML::Dump(custom)));
    Write(defaults_path, "{}\n");
    Write(games.filePath("inherit/config.yaml"), "{}\n");
    HStreamWindow calibration;
    auto* calibration_selector = Find<QComboBox>(calibration, "gameSelector");
    calibration_selector->setCurrentIndex(0);
    auto* crop = Find<QCheckBox>(calibration, "showCropDialogCheck");
    auto* leveling = Find<QCheckBox>(calibration, "showLevelingDialogCheck");
    auto* reference = Find<QLineEdit>(calibration, "stitchFrameTimeEdit");
    Require(
        crop->isEnabled() && leveling->isEnabled() && reference->isEnabled(),
        "Defaults must enable calibration prompt and reference controls");
    Require(
        crop->isChecked() == baseline_crop && leveling->isChecked() == !baseline_crop &&
            reference->text() == "00:00:02.345",
        "Defaults ignored canonical calibration baseline values");
    crop->setChecked(!baseline_crop);
    leveling->setChecked(baseline_crop);
    reference->setText("-00:00:01.125");
    Require(HStreamWindowTestAccess::Save(calibration), "Cannot save calibration defaults");
    const auto saved_defaults = YAML::LoadFile(defaults_path.toStdString());
    Require(
        saved_defaults["hstream_ui"]["show_crop_dialog"].as<bool>() == !baseline_crop &&
            saved_defaults["hstream_ui"]["show_leveling_dialog"].as<bool>() == baseline_crop &&
            saved_defaults["stitching"]["stitch_frame_time"].as<std::string>() == "-00:00:01.125",
        "Calibration defaults were not saved under canonical keys");
    const auto valid_reference_defaults = Read(defaults_path);
    for (const char* invalid : {"", "abc", "-00:00:00", "24:00:00"}) {
      reference->setText(invalid);
      Require(!HStreamWindowTestAccess::Save(calibration), "Invalid reference must not save Defaults");
      Require(Read(defaults_path) == valid_reference_defaults, "Invalid reference changed Defaults on disk");
    }
    reference->setText("-00:00:01.125");
    {
      HStreamWindow calibration_reopened;
      Find<QComboBox>(calibration_reopened, "gameSelector")->setCurrentIndex(0);
      Require(
          Find<QCheckBox>(calibration_reopened, "showCropDialogCheck")->isChecked() == !baseline_crop &&
              Find<QCheckBox>(calibration_reopened, "showLevelingDialogCheck")->isChecked() == baseline_crop &&
              Find<QLineEdit>(calibration_reopened, "stitchFrameTimeEdit")->text() == "-00:00:01.125",
          "Restart lost calibration defaults");
    }
    calibration_selector->setCurrentIndex(calibration_selector->findText("inherit"));
    Require(
        crop->isChecked() == !baseline_crop && leveling->isChecked() == baseline_crop &&
            reference->text() == "-00:00:01.125",
        "Game did not inherit calibration defaults");
    crop->setChecked(false);
    leveling->setChecked(false);
    reference->setText("00:00:00");
    Require(HStreamWindowTestAccess::Save(calibration), "Cannot save explicit game calibration settings");
    const auto initial_game = YAML::Load(Read(games.filePath("inherit/config.yaml")).toStdString());
    reference->setText("00:00:00.000");
    Require(HStreamWindowTestAccess::Save(calibration), "Equivalent reference spelling should save");
    const auto explicit_game = Read(games.filePath("inherit/config.yaml"));
    const auto equivalent_game = YAML::Load(explicit_game.toStdString());
    Require(
        equivalent_game["stitching"]["stitch_frame_time"].as<std::string>() == "00:00:00" &&
            YAML::Dump(equivalent_game["hstream_ui"]["stitching_calibration"]) ==
                YAML::Dump(initial_game["hstream_ui"]["stitching_calibration"]),
        "Equivalent reference changed calibration identity");
    reference->setText("-00:00:00");
    Require(!HStreamWindowTestAccess::Save(calibration), "Invalid reference must not save a game");
    Require(Read(games.filePath("inherit/config.yaml")) == explicit_game, "Invalid reference changed the saved game");
    calibration_selector->setCurrentIndex(0);
    calibration_selector->setCurrentIndex(calibration_selector->findText("inherit"));
    Require(
        !crop->isChecked() && !leveling->isChecked() && reference->text() == "00:00:00",
        "Explicit false and zero game settings lost precedence");
    calibration_selector->setCurrentIndex(0);
    const auto calibration_before_reset = Read(defaults_path);
    Find<QPushButton>(calibration, "resetDefaultsButton")->click();
    Require(
        crop->isChecked() == baseline_crop && leveling->isChecked() == !baseline_crop &&
            reference->text() == "00:00:02.345",
        "Reset did not stage canonical calibration defaults");
    Require(Read(defaults_path) == calibration_before_reset, "Calibration reset saved before Save Preset");
    Require(HStreamWindowTestAccess::Save(calibration), "Cannot save reset calibration defaults");
    const auto reset_calibration = YAML::LoadFile(defaults_path.toStdString());
    Require(reset_calibration.IsMap() && reset_calibration.size() == 0, "Calibration reset left an override");
    Require(Read(games.filePath("inherit/config.yaml")) == explicit_game, "Calibration defaults modified a game");
    Write(games.filePath("inherit/config.yaml"), "{}\n");
    calibration_selector->setCurrentIndex(calibration_selector->findText("inherit"));
    Require(
        crop->isChecked() == baseline_crop && leveling->isChecked() == !baseline_crop &&
            reference->text() == "00:00:02.345",
        "Game retained cleared calibration defaults");
  }
  for (const char* key : {"show_crop_dialog", "show_leveling_dialog"}) {
    for (const char* invalid : {"missing", "null", "invalid", "[]"}) {
      YAML::Node custom = YAML::Load(original_baseline.toStdString());
      if (std::string(invalid) == "missing")
        custom["hstream_ui"].remove(key);
      else
        custom["hstream_ui"][key] = YAML::Load(invalid);
      Write(custom_baseline.filePath("baseline.yaml"), QByteArray::fromStdString(YAML::Dump(custom)));
      for (const char* overlay :
           {"{}\n",
            "hstream_ui: {show_crop_dialog: false, show_leveling_dialog: false}\n",
            "hstream_ui: {show_crop_dialog: true, show_leveling_dialog: true}\n"}) {
        Write(defaults_path, overlay);
        try {
          HStreamWindow invalid_calibration_baseline;
          Require(false, "Missing or invalid calibration prompt baseline must fail startup");
        } catch (const std::exception& error) {
          Require(
              std::string(error.what()).find(std::string("hstream_ui.") + key) != std::string::npos,
              "Invalid calibration prompt baseline must identify its required key");
        }
        Require(Read(defaults_path) == overlay, "Rejected baseline modified saved defaults");
      }
    }
  }
  std::cout << "Defaults preset tests passed\n";
  return 0;
}
