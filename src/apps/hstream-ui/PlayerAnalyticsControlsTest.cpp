#include "src/apps/hstream-ui/PlayerAnalyticsControls.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
template <class T>
T* Control(PlayerAnalyticsControls& widget, const char* name) {
  auto* result = widget.findChild<T*>(name);
  Require(result != nullptr, name);
  return result;
}
void Write(const QString& path, const QByteArray& text) {
  QFile file(path);
  Require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "Cannot create test configuration");
}
YAML::Node Defaults() {
  return YAML::Load(
      "plot: {plot_pose: false, plot_actions: false, plot_jersey_numbers: false}\n"
      "pipeline: {primary-gie: {enable: 1}, tracker: {enable: 1}}\n");
}
YAML::Node PoseManifest() {
  auto node = YAML::Load(R"(
schema_version: 1
feature: pose
model_family: rtmpose
model_id: ui-metadata-test
source: {url: 'https://example.invalid/model.pth', license: Apache-2.0}
onnx: {file: model.onnx, opset: 17}
engine: {file: model.engine, tensorrt_version: '10.3.0', tensorrt_build_version: 1,
         cuda_runtime_version: 12060, gpu_name: Example GPU, compute_capability: '8.7', precision: fp16, max_batch: 8}
inputs: [{name: image, dtype: float32, shape: [-1,3,256,192]}]
outputs: [{name: simcc_x, dtype: float32, shape: [-1,17,384]}, {name: simcc_y, dtype: float32, shape: [-1,17,512]}]
topology_id: coco17
preprocessing: {id: rtmpose-coco17-affine-v1, color: rgb,
                mean: [123.675,116.28,103.53], std: [58.395,57.12,57.375],
                padding: 1.25, simcc_split_ratio: 2, flip_test: false}
)");
  for (const char* section : {"source", "onnx", "engine"})
    node[section]["sha256"] = std::string(64, 'a');
  node["source"]["config_sha256"] = std::string(64, 'b');
  return node;
}
QString Bundle(const QString& root, const QString& name) {
  const QString path = QDir(root).filePath(name);
  Require(QDir().mkpath(path), "Cannot create bundle fixture");
  Write(QDir(path).filePath("manifest.json"), QByteArray::fromStdString(YAML::Dump(PoseManifest())));
  // Deliberately not real models. The UI may stat their existence but must never
  // deserialize/read them; runtime compatibility is the native engine's job.
  Write(QDir(path).filePath("model.engine"), "not an engine");
  Write(QDir(path).filePath("model.onnx"), "not an onnx");
  return path;
}
void Disabled() {
  PlayerAnalyticsControls widget;
  const auto invalid = YAML::Load(R"(
pipeline:
  player-analytics:
    pose: {enable: false, bundle: /proc/1/analytics-disabled-do-not-open, rate-hz: broken}
    jersey: {enable: false, bundle: [invalid], roi-mode: unknown}
    action: {enable: false, bundle: /proc/1/analytics-disabled-do-not-open}
    max-due-rois: invalid
  tracker: {reid-enable: false, reid-config-file: /proc/1/analytics-disabled-do-not-open, ll-config-file: custom.yaml}
)");
  widget.loadConfig(Defaults(), YAML::Node(), invalid, "/configs");
  Require(!widget.isDirty() && widget.validateForRun().isEmpty(), "Disabled inference inspected model limits/paths");
  const std::string before = YAML::Dump(invalid);
  auto saved = YAML::Clone(invalid);
  Require(
      widget.applyChanges(saved).isEmpty() && YAML::Dump(saved) == before, "Unrelated save rewrote native settings");
  Control<QCheckBox>(widget, "playerDrawPose")->setChecked(true);
  Control<QCheckBox>(widget, "playerDrawActions")->setChecked(true);
  Require(
      widget.validateForRun().isEmpty() && !Control<QCheckBox>(widget, "playerPoseEnable")->isChecked(),
      "Drawing activated compute or validated disabled models");
  const auto args = widget.arguments();
  Require(
      args.contains("--options=plot.plot_pose=true") &&
          args.contains("--options=pipeline.player-analytics.draw-pose=true") && !args.join(' ').contains("bundle=") &&
          !args.join(' ').contains("reid-config-file="),
      "Drawing preferences or disabled argument omission incorrect");
}
void LegacyProgramBoxes(const QString& root) {
  PlayerAnalyticsControls widget;
  auto defaults = Defaults();
  defaults["plot"]["debug_play_tracker"] = false;
  defaults["plot"]["plot_individual_player_tracking"] = false;
  auto game = YAML::Load("plot: {debug_play_tracker: true, retained: yes}");
  widget.loadConfig(defaults, YAML::Node(), game, root);
  auto* boxes = Control<QCheckBox>(widget, "playerDrawBoxes");
  Require(boxes->isChecked() && !widget.isDirty(), "Legacy debug boxes were not reflected in controls");
  const auto original = YAML::Dump(game);
  Require(
      widget.applyChanges(game).isEmpty() && YAML::Dump(game) == original,
      "Unchanged debug-only save rewrote its configuration");
  Require(
      !widget.arguments().join(' ').contains("plot-player-tracking=") &&
          !widget.arguments().join(' ').contains("plot_individual_player_tracking="),
      "Unchanged box arguments overrode inherited native/debug settings");
  boxes->setChecked(false);
  Require(
      widget.arguments().contains("--options=pipeline.hmplaycropper.plot-player-tracking=false"),
      "Explicit box edit did not reach launch/export");
  Require(
      widget.applyChanges(game).isEmpty() && game["plot"]["debug_play_tracker"].as<bool>() &&
          !game["pipeline"]["hmplaycropper"]["plot-player-tracking"].as<bool>(),
      "Box edit disabled other debug layers or failed to persist its override");
  widget.loadConfig(defaults, YAML::Node(), game, root);
  Require(!boxes->isChecked() && !widget.isDirty(), "Saved override lost against same-layer debug OR");
  auto user = YAML::Load("plot: {debug_play_tracker: true}");
  game = YAML::Load("plot: {plot_individual_player_tracking: false}");
  widget.loadConfig(defaults, user, game, root);
  Require(boxes->isChecked(), "Later individual false erased inherited debug true");
  user["pipeline"]["hmplaycropper"]["plot-player-tracking"] = false;
  widget.loadConfig(defaults, user, game, root);
  Require(!boxes->isChecked(), "False canonical rank displaced native override of true source");
  game["plot"]["debug_play_tracker"] = true;
  widget.loadConfig(defaults, user, game, root);
  Require(boxes->isChecked(), "Later true debug source did not override older native false");
}
void LayersAndPersistence(const QString& root) {
  PlayerAnalyticsControls widget;
  int changes = 0;
  widget.setChangedCallback([&] { ++changes; });
  auto defaults = Defaults();
  defaults["pipeline"]["player-analytics"]["draw-pose"] = true;
  auto user = YAML::Load("plot: {plot_pose: false}\npipeline: {player-analytics: {pose: {rate-hz: 20}}}");
  auto game = YAML::Load(R"(
plot: {plot_pose: true, unknown_flag: keep}
pipeline:
  tracker: {ll-config-file: existing-native.yaml, private: {retain: yes}}
  hmplaycropper: {private-properties: {plot-player-tracking: 1, retained: custom}}
  player-analytics:
    draw-pose: false
    gpu-id: 2
    max-due-rois: 12
    jersey: {confidence-threshold: 0.91, future-option: [a,b]}
    custom-model-option: keep
)");
  widget.loadConfig(defaults, YAML::Node(), YAML::Node(), root);
  Require(Control<QCheckBox>(widget, "playerDrawPose")->isChecked(), "Same-default native did not beat canonical");
  widget.loadConfig(defaults, user, YAML::Node(), root);
  Require(
      !Control<QCheckBox>(widget, "playerDrawPose")->isChecked(), "Higher user canonical did not beat default native");
  widget.loadConfig(defaults, user, game, root);
  Require(
      !widget.isDirty() && changes == 0 && !Control<QCheckBox>(widget, "playerDrawPose")->isChecked(),
      "Load emitted edits or ignored same-game native drawing");
  Control<QCheckBox>(widget, "playerDrawPose")->setChecked(true);
  Control<QCheckBox>(widget, "playerDrawBoxes")->setChecked(true);
  Require(widget.isDirty(), "Drawing edits did not mark dirty");
  Require(widget.applyChanges(game).isEmpty(), "Cannot save drawing choices");
  Require(
      game["plot"]["plot_pose"].as<bool>() && game["pipeline"]["player-analytics"]["draw-pose"].as<bool>() &&
          game["pipeline"]["tracker"]["ll-config-file"].as<std::string>() == "existing-native.yaml" &&
          game["pipeline"]["player-analytics"]["jersey"]["future-option"].size() == 2 &&
          game["pipeline"]["player-analytics"]["gpu-id"].as<int>() == 2 &&
          game["pipeline"]["hmplaycropper"]["private-properties"]["plot-player-tracking"].as<int>() == 1 &&
          game["pipeline"]["hmplaycropper"]["private-properties"]["retained"].as<std::string>() == "custom" &&
          game["plot"]["unknown_flag"].as<std::string>() == "keep",
      "Edited canonical drawing failed to reconcile its alias or destroyed advanced/native keys");
  const auto path = QDir(root).filePath("config.yaml");
  Write(path, QByteArray::fromStdString(YAML::Dump(game)));
  widget.loadConfig(defaults, user, YAML::LoadFile(path.toStdString()), root);
  Require(
      !widget.isDirty() && Control<QCheckBox>(widget, "playerDrawPose")->isChecked(), "Saved drawing failed reload");
  const auto unchanged = YAML::Dump(game);
  Require(
      widget.applyChanges(game).isEmpty() && YAML::Dump(game) == unchanged,
      "Repeated save changed unknown/native values");
  widget.resetToDefaults();
  Require(
      widget.isDirty() && !Control<QCheckBox>(widget, "playerDrawPose")->isChecked(),
      "Reset did not restore inherited choice");
  widget.loadConfig(defaults, user, YAML::Node(), root);
  Require(
      !widget.isDirty() && !Control<QCheckBox>(widget, "playerDrawBoxes")->isChecked(),
      "Unsaved edits leaked between games");
  // A null canonical mapping is suppressed and retains a lower native value.
  auto null_user = YAML::Load("plot: {plot_pose: null}");
  widget.loadConfig(defaults, null_user, YAML::Node(), root);
  Require(Control<QCheckBox>(widget, "playerDrawPose")->isChecked(), "Null canonical erased inherited native drawing");
  widget.loadConfig(defaults, YAML::Node(), YAML::Load("pipeline: {player-analytics: null}"), root);
  Require(!Control<QCheckBox>(widget, "playerDrawPose")->isChecked(), "Null native subtree retained removed drawing");
  defaults = Defaults();
  defaults["plot"]["plot_pose"] = true;
  defaults["pipeline"]["player-analytics"]["pose"]["enable"] = true;
  defaults["pipeline"]["player-analytics"]["pose"]["bundle"] = "/inherited/pose";
  widget.loadConfig(defaults, null_user, YAML::Load("pipeline: {player-analytics: {pose: null}}"), root);
  Require(
      !Control<QCheckBox>(widget, "playerPoseEnable")->isChecked() &&
          Control<QLineEdit>(widget, "playerPoseBundle")->text().isEmpty() &&
          !Control<QCheckBox>(widget, "playerDrawPose")->isChecked() && widget.validateForRun().isEmpty() &&
          widget.arguments().contains("--options=pipeline.player-analytics.pose.enable=false"),
      "Null subtree retained inherited inference or null canonical mapping");
}
void DependenciesAndExport(const QString& root) {
  PlayerAnalyticsControls widget;
  auto defaults = Defaults();
  widget.loadConfig(defaults, YAML::Node(), YAML::Node(), root);
  Control<QCheckBox>(widget, "playerActionEnable")->setChecked(true);
  Control<QLineEdit>(widget, "playerActionBundle")->setText("/missing/action");
  Require(widget.validateForRun().contains("require explicit pose"), "Action without pose was accepted");
  widget.loadConfig(defaults, YAML::Node(), YAML::Node(), root);
  Control<QCheckBox>(widget, "playerJerseyEnable")->setChecked(true);
  Control<QLineEdit>(widget, "playerJerseyBundle")->setText("/missing/jersey");
  Require(widget.validateForRun().contains("Missing jersey"), "BBox jersey incorrectly required pose");
  Control<QComboBox>(widget, "playerJerseyRoiMode")->setCurrentIndex(1);
  Require(widget.validateForRun().contains("require explicit pose"), "Guided jersey without pose accepted");
  auto advanced = YAML::Load(
      "pipeline: {player-analytics: {pose: {enable: 1, bundle: x, rate-hz: 9}, action: {enable: 1, bundle: y}}}");
  widget.loadConfig(defaults, YAML::Node(), advanced, root);
  Require(widget.validateForRun().contains("rate-hz >= 10"), "Action ignored inherited pose rate prerequisite");
  advanced = YAML::Load(
      "pipeline: {player-analytics: {pose: {enable: 1, bundle: x}, jersey: {enable: 1, bundle: y, roi-mode: pose}, max-due-rois: 1}}");
  widget.loadConfig(defaults, YAML::Node(), advanced, root);
  Require(widget.validateForRun().contains("max-due-rois >= 2"), "Guided budget prerequisite missing");
  widget.loadConfig(defaults, YAML::Node(), YAML::Node(), root);
  const auto bundle = Bundle(root, "prepared pose with spaces");
  Control<QCheckBox>(widget, "playerPoseEnable")->setChecked(true);
  Control<QLineEdit>(widget, "playerPoseBundle")->setText("prepared pose with spaces");
  Require(
      widget.validateForRun().isEmpty(), "UI attempted model deserialization or rejected valid metadata prerequisites");
  const auto unsaved = widget.arguments();
  Require(
      unsaved.contains("--options=pipeline.player-analytics.pose.enable=true") &&
          unsaved.contains("--options=pipeline.player-analytics.pose.bundle=" + bundle),
      "Unsaved effective choices/relative bundle did not reach runner arguments");
  YAML::Node saved;
  Require(widget.applyChanges(saved).isEmpty(), "Cannot persist enabled choice");
  widget.loadConfig(defaults, YAML::Node(), saved, root);
  Require(!widget.isDirty() && widget.arguments() == unsaved, "Save/reload changed effective exported arguments");
  QFile::remove(QDir(bundle).filePath("model.engine"));
  Require(
      widget.validateForRun().contains("Missing or unreadable prepared pose"), "Missing prepared engine not diagnosed");
  Write(QDir(bundle).filePath("model.engine"), "metadata-only test");
  defaults["pipeline"]["tracker"]["enable"] = 0;
  widget.loadConfig(defaults, YAML::Node(), saved, root);
  Require(widget.validateForRun().contains("tracker.enable"), "Disabled tracker prerequisite not shown");
  defaults = Defaults();
  defaults["pipeline"]["primary-gie"]["enable"] = 0;
  widget.loadConfig(defaults, YAML::Node(), saved, root);
  Require(widget.validateForRun().contains("primary-gie.enable"), "Disabled detector prerequisite not shown");
  defaults = Defaults();
  const auto special = Bundle(root, "saved,pose=one");
  saved["pipeline"]["player-analytics"]["pose"]["bundle"] = special.toStdString();
  widget.loadConfig(defaults, YAML::Node(), saved, root);
  Require(
      widget.validateForRun().isEmpty() && !widget.arguments().join(' ').contains("pose.bundle="),
      "Valid unchanged delimiter path was rejected or misserialized");
  Control<QLineEdit>(widget, "playerPoseBundle")->setText(special + "x");
  Require(widget.validateForRun().contains("unsaved bundle"), "Unrepresentable unsaved path was silently dropped");
}
void ReId(const QString& root) {
  PlayerAnalyticsControls widget;
  widget.loadConfig(Defaults(), YAML::Node(), YAML::Node(), root);
  Control<QCheckBox>(widget, "playerReidEnable")->setChecked(true);
  Require(widget.validateForRun().contains("prepared configuration"), "ReID without configuration accepted");
  const auto config = QDir(root).filePath("reid.yaml");
  Write(config, "ReID: {modelEngineFile: reid.engine}\n");
  Control<QLineEdit>(widget, "playerReidConfig")->setText("reid.yaml");
  Require(widget.validateForRun().contains("modelEngineFile"), "Missing ReID engine not reported");
  Write(QDir(root).filePath("reid.engine"), "not read by UI");
  Require(
      widget.validateForRun().isEmpty() &&
          widget.arguments().contains("--options=pipeline.tracker.reid-config-file=" + config),
      "Prepared ReID prerequisite/export path failed");
  Control<QCheckBox>(widget, "playerReidEnable")->setChecked(false);
  Control<QLineEdit>(widget, "playerReidConfig")->setText("/proc/1/analytics-disabled-do-not-open");
  Require(widget.validateForRun().isEmpty(), "Disabled ReID inspected retained path");
}
} // namespace
int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QTemporaryDir directory;
  Require(directory.isValid(), "Cannot create test directory");
  Disabled();
  LegacyProgramBoxes(directory.path());
  LayersAndPersistence(directory.path());
  DependenciesAndExport(directory.path());
  ReId(directory.path());
  std::cout
      << "Player analytics UI defaults, layers, native preservation, prerequisites, save/reload and export passed\n";
}
