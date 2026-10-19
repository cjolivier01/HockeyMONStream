#include "src/apps/hstream-ui/RinkFramingControls.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <iostream>

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  RinkFramingControls controls;
  auto* enable = controls.findChild<QCheckBox*>("rinkFramingEnable");
  auto* anticipate = controls.findChild<QCheckBox*>("rinkFramingAnticipate");
  auto* goals = controls.findChild<QCheckBox*>("rinkFramingGoals");
  auto* zoom = controls.findChild<QCheckBox*>("rinkFramingZoom");
  const QString prefix = "--options=pipeline.ds-playtracker.private-properties.";
  bool ok = !enable->isChecked() && anticipate->isChecked() && !anticipate->isEnabled() && !goals->isChecked() &&
      !zoom->isChecked() && !controls.isDirty() && controls.arguments().contains(prefix + "rink-camera-enable=0");
  YAML::Node empty(YAML::NodeType::Map), user, game;
  user["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"] = true;
  game["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"] = 0;
  controls.loadConfig(empty, user, game);
  ok &= !enable->isChecked() && !controls.isDirty();
  enable->setChecked(true);
  ok &= anticipate->isEnabled() && anticipate->isChecked() && !goals->isChecked() && !zoom->isChecked() &&
      controls.isDirty() && controls.arguments().contains(prefix + "rink-camera-enable=1") &&
      controls.arguments().contains(prefix + "rink-camera-frame-goals=0") &&
      controls.arguments().contains(prefix + "rink-camera-adaptive-zoom=0");
  controls.applyChanges(game);
  ok &= game["pipeline"]["ds-playtracker"]["private-properties"]["rink-camera-enable"].as<int>() == 1;
  controls.loadConfig(empty, user, game);
  ok &= !controls.isDirty() && enable->isChecked();
  controls.resetToDefaults();
  ok &= controls.isDirty() && !enable->isChecked();
  if (!ok)
    std::cerr << "Rink framing defaults, precedence, persistence or exported arguments differ\n";
  return ok ? 0 : 1;
}
