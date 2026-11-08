#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

#include <iostream>
#include <stdexcept>

struct HStreamWindowTestAccess {
  static bool save(HStreamWindow& window) {
    return window.savePreset();
  }
  static QStringList args(const HStreamWindow& window) {
    return window.pipelineArguments(true);
  }
  static void reload(HStreamWindow& window) {
    window.loadSavedControlConfig();
  }
  static void defaults(HStreamWindow& window) {
    window.selectDefaults();
  }
  static void reset(HStreamWindow& window) {
    window.resetDefaultsToBaseline();
  }
};
namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Write(const QString& path, const QByteArray& text) {
  QFile file(path);
  Check(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "Cannot write test configuration");
}
template <class T>
T* Find(HStreamWindow& window, const char* name) {
  auto* result = window.findChild<T*>(name);
  Check(result != nullptr, name);
  return result;
}
} // namespace
int main(int argc, char** argv) {
  try {
    QTemporaryDir root;
    Check(root.isValid(), "Cannot create test directory");
    qputenv("HOME", root.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", root.filePath("config").toUtf8());
    qputenv("HM_GAME_DIR", root.filePath("games").toUtf8());
    QDir().mkpath(root.filePath("games/one/_hockeymon"));
    QDir().mkpath(root.filePath("games/two/_hockeymon"));
    const auto config_path = root.filePath("games/one/_hockeymon/config.yaml");
    Write(
        config_path,
        "rink: {scoreboard: {scoreboard_scale: 1.2}}\npipeline: {hmplaycropper: {private-properties: {scoreboard-scale: 1.7}}}\n");
    QApplication app(argc, argv);
    HStreamWindow window;
    auto* game = Find<QComboBox>(window, "gameSelector");
    auto* scale = Find<QDoubleSpinBox>(window, "scoreboardScaleSpin");
    auto* save = Find<QPushButton>(window, "savePresetButton");
    game->setCurrentIndex(game->findText("two"));
    Check(scale->value() == 1, "Fresh game must read baseline scale 1");
    game->setCurrentIndex(game->findText("one"));
    Check(scale->value() == 1.7, "Native scale alias must win within the game layer");
    scale->setValue(.5);
    Check(save->isEnabled(), "Scale edit did not mark preset dirty");
    Check(
        HStreamWindowTestAccess::args(window).contains("--options=rink.scoreboard.scoreboard_scale=0.5"),
        "Draft scale did not reach the next playback");
    Check(HStreamWindowTestAccess::save(window), "Could not save scale");
    const auto saved = YAML::LoadFile(config_path.toStdString());
    Check(saved["rink"]["scoreboard"]["scoreboard_scale"].as<double>() == .5, "Scale did not persist");
    Check(
        !saved["pipeline"]["hmplaycropper"]["private-properties"]["scoreboard-scale"].IsDefined(),
        "Saved scale kept a conflicting alias");
    HStreamWindowTestAccess::reload(window);
    Check(scale->value() == .5, "Saved scale did not reload");
    game->setCurrentIndex(game->findText("two"));
    Check(scale->value() == 1, "Game scale leaked into another game");
    HStreamWindowTestAccess::defaults(window);
    scale->setValue(2);
    Check(HStreamWindowTestAccess::save(window), "Could not save default scale");
    Check(
        YAML::LoadFile(
            root.filePath(".config/hstream-ui/defaults.yaml").toStdString())["rink"]["scoreboard"]["scoreboard_scale"]
                .as<double>() == 2,
        "Default scale did not persist");
    HStreamWindowTestAccess::reset(window);
    Check(scale->value() == 1 && HStreamWindowTestAccess::save(window), "Reset did not restore baseline scale");
    const auto reset = YAML::LoadFile(root.filePath(".config/hstream-ui/defaults.yaml").toStdString());
    Check(reset.IsMap() && reset.size() == 0, "Reset left a scale override");
    const auto artifacts = qEnvironmentVariable("HSTREAM_SCOREBOARD_ARTIFACTS");
    if (!artifacts.isEmpty()) {
      window.resize(1600, 1000);
      window.show();
      QApplication::processEvents();
      QDir().mkpath(artifacts);
      Check(window.grab().save(artifacts + "/qt-after.png"), "Cannot capture scoreboard controls");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
