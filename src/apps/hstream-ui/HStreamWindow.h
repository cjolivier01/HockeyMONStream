#pragma once

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QProcess>

#include <yaml-cpp/yaml.h>

#include <map>
#include <string>

class QProcessEnvironment;

namespace hm::ui_internal {

// Restores only paths cleared by the UI's automatic video selection. Other
// keys may have been updated by another config owner in the meantime.
void restore_auto_selection_paths(YAML::Node& current, const YAML::Node& previous);

} // namespace hm::ui_internal

class HStreamWindow : public QMainWindow {
 public:
  explicit HStreamWindow(QWidget* parent = nullptr);

  QString pipelineStateText() const;
  QString outputStateText(const QString& id) const;
  QString logText() const;
  QString gameIdText() const;
  QString gameDirectoryText() const;
  int videoSetCount() const;
  int cameraControlValue(const QString& id) const;
  int cameraTabCount() const;

 private:
  enum class CopiedImportCleanupResult {
    kSuccess,
    kRolledBack,
    kCommittedWithCleanupFailure,
  };

  void buildUi();
  void buildTopBar(QVBoxLayout* root);
  void buildMainArea(QVBoxLayout* root);
  void buildGameControls(QVBoxLayout* root);
  void buildPreviewPane(QVBoxLayout* root);
  void buildOutputControls(QVBoxLayout* parent);
  void buildCameraControls(QVBoxLayout* parent);
  void buildLog(QVBoxLayout* root);

  void startPipeline();
  void pauseOrResumePipeline();
  void stopPipeline();
  void handlePipelineStarted();
  void handlePipelineFinished(int exit_code, QProcess::ExitStatus exit_status);
  void handlePipelineError(QProcess::ProcessError error);
  void readPipelineOutput();
  void togglePreviewFullscreen(int tab_index);
  void restartStage();
  void savePreset();
  void resetCameraControls();
  void refreshGames();
  void selectGame(const QString& game_id);
  void createOrLoadGame();
  void addVideoPath();
  void browseVideoPath();
  void removeSelectedVideoSet();
  void refreshVideoSets();
  QString selectedVideoRole() const;
  QString gameRoot() const;
  QString gameDirectory(const QString& game_id) const;
  QString relativeToGameDir(const QString& path) const;
  bool ensureGameDirectory();
  bool importVideoPath(const QString& source_path, QString* imported_relative_path, bool* created);
  // These helpers mutate video/config state and require the caller to hold a
  // GameConfigTransactionLock for the selected game.
  bool saveCopiedImport(
      const QString& relative_path,
      const QString& auto_group_family = {},
      const QString& source_parent = {});
  bool rollbackImportedVideoPath(const QString& relative_path);
  CopiedImportCleanupResult removeClearedCopiedExplicitImports(
      const QByteArray& original_config,
      bool had_config,
      bool restore_auto_selection_on_failure = true,
      const QByteArray& published_auto_config = {});
  bool syncRuntimeExplicitVideoConfig(YAML::Node& config);
  bool savePrivateConfigForRole(
      const QString& role,
      const QString& relative_path,
      QByteArray* original_config,
      bool* had_config,
      QByteArray* published_config);
  bool removePrivateConfigForRole(
      const QString& role,
      const QString& relative_path,
      QByteArray* original_config,
      bool* had_config,
      bool* copied_import,
      QByteArray* published_config);
  bool restorePrivateConfigAfterRemoveFailure(
      const QByteArray& original_config,
      bool had_config,
      const QByteArray& removed_config);
  bool resolveImportedVideoPath(const QString& relative_path, bool allow_regular_delete, QString* imported_path);
  bool removeImportedVideoPath(const QString& relative_path, bool allow_regular_delete = false);
  void toggleOutput(const QString& id, bool enabled);
  void redirectYoutube();
  void addRtspOutput();
  void appendLog(const QString& message);
  QString writePlaytrackerRuntimeConfig();
  QString pipelineRunnerPath() const;
  QString pipelineConfigPath(const QString& config_name) const;
  QString pipelineWorkingDirectory() const;
  QStringList pipelineArguments() const;
  bool setupPretrainedAssets(const QStringList& pipeline_args);
  void logMissingTensorRtEngineCaches(const QStringList& pipeline_args);
  int stitchingCalibrationControlPoints() const;
  bool prepareStitchingCalibrationRun(
      const QString& runner,
      const QString& working_dir,
      const QProcessEnvironment& env,
      bool* calibration_required);
  bool runStitchingClean(const QString& runner, const QString& working_dir, const QProcessEnvironment& env);
  bool saveStitchingCalibrationState(int control_points, const QString& status);
  QStringList enabledSinkNames() const;
  bool isCalibrationRun() const;
  void updateRunControls();
  bool applySavedControlConfig(YAML::Node& config, bool* invalidate_rink_masks, int* invalidated_config_artifacts);
  void loadSavedControlConfig();
  bool sendLiveCameraControl(const QString& id, int value);
  QSlider* addSlider(QVBoxLayout* layout, const QString& id, const QString& label, int minimum, int maximum, int value);

  QLabel* backend_mode_{nullptr};
  QLabel* pipeline_state_{nullptr};
  QLabel* preview_status_{nullptr};
  QLabel* stitched_status_{nullptr};
  QLabel* preview_external_notice_{nullptr};
  QLabel* stitched_external_notice_{nullptr};
  QLabel* game_path_label_{nullptr};
  QLabel* video_sets_path_label_{nullptr};
  QComboBox* game_selector_{nullptr};
  QComboBox* run_mode_selector_{nullptr};
  QSpinBox* control_points_spin_{nullptr};
  QLineEdit* game_id_edit_{nullptr};
  QLineEdit* video_path_edit_{nullptr};
  QListWidget* video_set_list_{nullptr};
  QRadioButton* role_auto_{nullptr};
  QRadioButton* role_left_{nullptr};
  QRadioButton* role_center_{nullptr};
  QRadioButton* role_right_{nullptr};
  QTextEdit* log_{nullptr};
  QTabWidget* preview_tabs_{nullptr};
  QWidget* preview_surface_{nullptr};
  QWidget* stitched_surface_{nullptr};
  QTabWidget* camera_tabs_{nullptr};
  QVBoxLayout* output_list_{nullptr};
  QProcess* pipeline_process_{nullptr};
  QPushButton* start_button_{nullptr};
  QPushButton* pause_button_{nullptr};
  QPushButton* stop_button_{nullptr};
  QPushButton* program_fullscreen_button_{nullptr};
  QPushButton* stitched_fullscreen_button_{nullptr};
  bool pipeline_paused_{false};
  bool pipeline_uses_process_group_{false};
  bool pipeline_stop_requested_{false};
  bool calibration_pending_{false};
  bool preview_fullscreen_{false};
  QString pipeline_stdout_buffer_;
  QString pipeline_stderr_buffer_;
  int dynamic_rtsp_count_{0};
  std::map<QString, QLabel*> output_states_;
  std::map<QString, QCheckBox*> output_toggles_;
  std::map<QString, QSlider*> camera_sliders_;
  std::map<QString, QLabel*> camera_value_labels_;
  std::map<QString, int> camera_defaults_;
};
