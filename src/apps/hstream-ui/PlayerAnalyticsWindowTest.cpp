#include "src/apps/hstream-ui/HStreamWindow.h"
#include "src/apps/hstream-ui/PlayerAnalyticsControls.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

#include <cstdlib>
#include <iostream>

struct HStreamWindowTestAccess {
  static bool save(HStreamWindow& window) {
    return window.savePreset();
  }
  static QStringList args(const HStreamWindow& window, bool standalone = true) {
    return window.pipelineArguments(standalone);
  }
  static bool validate(HStreamWindow& window) {
    return window.validatePlayerAnalyticsForRun();
  }
  static void freeze(HStreamWindow& window) {
    window.active_run_game_id_ = window.game_id_edit_->text();
    window.active_player_analytics_arguments_ = window.player_analytics_controls_->arguments();
  }
  static void unfreeze(HStreamWindow& window) {
    window.active_run_game_id_.clear();
  }
  static QStringList calibration(HStreamWindow& window) {
    const QSignalBlocker block(window.run_mode_selector_);
    const int previous = window.run_mode_selector_->currentIndex();
    window.run_mode_selector_->setCurrentIndex(window.run_mode_selector_->findData("stitch-calibration"));
    const auto result = window.pipelineArguments(true);
    if (!window.validatePlayerAnalyticsForRun())
      std::abort();
    window.run_mode_selector_->setCurrentIndex(previous);
    return result;
  }
};
namespace {
void Require(bool value, const char* text) {
  if (!value) {
    std::cerr << text << '\n';
    std::exit(1);
  }
}
template <class T>
T* Find(HStreamWindow& window, const char* name) {
  auto* control = window.findChild<T*>(name);
  Require(control != nullptr, name);
  return control;
}
void Write(const QString& path, const QByteArray& content) {
  QFile file(path);
  Require(file.open(QIODevice::WriteOnly) && file.write(content) == content.size(), "Cannot write test game");
}
} // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir games;
  Require(games.isValid(), "Cannot create test games");
  qputenv("HM_GAME_DIR", games.path().toLocal8Bit());
  QDir().mkpath(games.filePath("analytics-one"));
  QDir().mkpath(games.filePath("analytics-two"));
  const auto config_path = games.filePath("analytics-one/config.yaml");
  Write(
      config_path,
      "pipeline:\n  tracker: {reid-enable: false, ll-config-file: existing-native.yaml}\n"
      "  player-analytics:\n    pose: {enable: false, bundle: /disabled/pose, rate-hz: 20}\n"
      "    jersey: {enable: false, confidence-threshold: 0.91}\n"
      "    action: {enable: false}\n    draw-pose: false\n    retained: [custom, data]\n"
      "plot: {plot_pose: true, debug_play_tracker: true}\n");
  HStreamWindow window;
  auto* game = Find<QComboBox>(window, "gameSelector");
  game->setCurrentIndex(game->findText("analytics-one"));
  auto* draw = Find<QCheckBox>(window, "playerDrawPose");
  auto* pose = Find<QCheckBox>(window, "playerPoseEnable");
  auto* path = Find<QLineEdit>(window, "playerPoseBundle");
  auto* model = Find<QComboBox>(window, "playerPoseModel");
  auto* save = Find<QPushButton>(window, "savePresetButton");
  auto* preview = Find<QCheckBox>(window, "showPlayerTrackingCheck");
  auto* boxes = Find<QCheckBox>(window, "playerDrawBoxes");
  const auto detector = Find<QComboBox>(window, "detectorPrecisionCombo")->currentData();
  Require(
      !draw->isChecked() && !pose->isChecked() && model->currentData() == "custom",
      "Window load lost native drawing precedence or legacy model selection");
  Require(
      boxes->isChecked() && !HStreamWindowTestAccess::args(window).join(' ').contains("plot-player-tracking=") &&
          !HStreamWindowTestAccess::args(window, false).join(' ').contains("plot-player-tracking="),
      "Unchanged launch/export removed legacy debug-derived boxes");
  preview->setChecked(true);
  boxes->setChecked(true);
  boxes->setChecked(false);
  Require(preview->isChecked(), "Program boxes modified the preview-only toggle");
  draw->setChecked(true);
  pose->setChecked(true);
  path->setText("/missing prepared pose");
  const auto unsaved = HStreamWindowTestAccess::args(window);
  Require(
      save->isEnabled() && unsaved.contains("--options=pipeline.player-analytics.pose.enable=true") &&
          unsaved.contains("--options=pipeline.player-analytics.pose.bundle=/missing prepared pose") &&
          unsaved.contains("--options=plot.plot_pose=true"),
      "Unsaved analytics did not reach runner/export");
  Require(!HStreamWindowTestAccess::validate(window), "Missing enabled bundle did not block Program launch");
  Require(
      static_cast<PlayerAnalyticsControls*>(Find<QWidget>(window, "playerAnalyticsControls"))
          ->validateForRun()
          .contains("Missing pose"),
      "Structural detector/tracker defaults were lost before bundle validation");
  model->setCurrentIndex(model->findData("rtmpose-m-coco17-256x192"));
  Require(
      HStreamWindowTestAccess::validate(window) &&
          HStreamWindowTestAccess::args(window).contains(
              "--options=pipeline.player-analytics.pose.model=rtmpose-m-coco17-256x192") &&
          !HStreamWindowTestAccess::args(window).join(' ').contains("pose.bundle="),
      "Built-in pose selection required manual preparation or exported its stale custom path");
  Require(
      !HStreamWindowTestAccess::calibration(window).join(' ').contains("player-analytics"),
      "Calibration launch consumed analytics choices");
  HStreamWindowTestAccess::freeze(window);
  draw->setChecked(false);
  model->setCurrentIndex(model->findData("custom"));
  Require(
      HStreamWindowTestAccess::args(window, false).contains("--options=plot.plot_pose=true") &&
          HStreamWindowTestAccess::args(window).contains("--options=plot.plot_pose=false") &&
          HStreamWindowTestAccess::args(window, false)
              .contains("--options=pipeline.player-analytics.pose.model=rtmpose-m-coco17-256x192") &&
          HStreamWindowTestAccess::args(window).contains("--options=pipeline.player-analytics.pose.model=custom"),
      "Mid-run edit changed active snapshot or failed to update next-run arguments");
  HStreamWindowTestAccess::unfreeze(window);
  draw->setChecked(true);
  model->setCurrentIndex(model->findData("rtmpose-m-coco17-256x192"));
  Require(HStreamWindowTestAccess::save(window), "Window could not save analytics preset");
  const auto saved = YAML::LoadFile(config_path.toStdString());
  Require(
      saved["pipeline"]["player-analytics"]["pose"]["enable"].as<bool>() &&
          saved["pipeline"]["player-analytics"]["pose"]["model"].as<std::string>() == "rtmpose-m-coco17-256x192" &&
          saved["pipeline"]["player-analytics"]["pose"]["bundle"].as<std::string>() == "/missing prepared pose" &&
          saved["pipeline"]["player-analytics"]["draw-pose"].as<bool>() &&
          saved["pipeline"]["player-analytics"]["retained"].size() == 2 &&
          saved["pipeline"]["player-analytics"]["pose"]["rate-hz"].as<int>() == 20 &&
          saved["pipeline"]["tracker"]["ll-config-file"].as<std::string>() == "existing-native.yaml" &&
          saved["plot"]["debug_play_tracker"].as<bool>() &&
          !saved["pipeline"]["hmplaycropper"]["plot-player-tracking"].as<bool>(),
      "Preset save lost native/advanced choices");
  QStringList exported;
  for (const auto& argument : saved["hstream_ui"]["job"]["arguments"])
    exported << QString::fromStdString(argument.as<std::string>());
  Require(
      exported.contains("--options=pipeline.player-analytics.pose.model=rtmpose-m-coco17-256x192") &&
          !exported.join(' ').contains("pose.bundle=") &&
          exported.contains("--options=pipeline.player-analytics.pose.enable=true") &&
          exported.contains("--options=plot.plot_pose=true"),
      "Saved job failed to capture unsaved analytics edits");
  Require(
      !save->isEnabled() && Find<QComboBox>(window, "detectorPrecisionCombo")->currentData() == detector &&
          preview->isChecked(),
      "Saving analytics reset unrelated controls or retained dirty state");
  pose->setChecked(false);
  game->setCurrentIndex(game->findText("analytics-two"));
  Require(!pose->isChecked(), "Unsaved inference leaked into the next game");
  game->setCurrentIndex(game->findText("analytics-one"));
  Require(
      pose->isChecked() && draw->isChecked() && path->text() == "/missing prepared pose" &&
          model->currentData() == "rtmpose-m-coco17-256x192",
      "Game switching did not reload saved analytics");
  HStreamWindow reload;
  auto* reload_game = Find<QComboBox>(reload, "gameSelector");
  reload_game->setCurrentIndex(reload_game->findText("analytics-one"));
  Require(
      Find<QCheckBox>(reload, "playerPoseEnable")->isChecked() && HStreamWindowTestAccess::save(reload),
      "Fresh window failed save/reload preservation");
  std::cout
      << "Window next-run snapshot, dirty state, preset/job persistence, calibration omission and preview independence passed\n";
}
