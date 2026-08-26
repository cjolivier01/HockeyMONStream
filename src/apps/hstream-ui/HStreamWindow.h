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
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QList>
#include <QtCore/QProcess>

#include <yaml-cpp/yaml.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QProcessEnvironment;
class QCloseEvent;
class QDialog;
class QEvent;
class QIcon;
class QProgressBar;
class QSplitter;
class QTimer;
class QToolButton;
class PipelineInspectorWidget;
class ScoreboardSelectionDialog;

namespace hm::ui_internal {

// Establishes the desktop identity used by WM_CLASS, desktop-file matching,
// taskbar grouping, and human-readable application labels. Call before the
// first native window is created.
void configure_application_identity();
QIcon application_icon();
// Restores only paths cleared by the UI's automatic video selection. Other
// keys may have been updated by another config owner in the meantime.
void restore_auto_selection_paths(YAML::Node& current, const YAML::Node& previous);
bool supports_x11_embedding(const QString& platform_name, bool tegra_runtime = false);
QString preview_channel_for_tab(int tab_index, int camera_count);
// Selects the pipeline runner and source workspace that belong to a Bazel-built
// UI executable without consulting the mutable bazel-bin workspace symlink.
QString matching_development_pipeline_runner(const QString& application_path);
QString matching_development_bazel_bin(const QString& application_path);
QString development_runtime_root_for_application(const QString& application_path);
// Returns an empty string when all artifacts needed by a Bazel development
// runtime are present, or the missing artifact path otherwise.
QString missing_development_runtime_artifact(const QString& bazel_bin_path);

} // namespace hm::ui_internal

class HStreamWindow : public QMainWindow {
  friend struct HStreamWindowTestAccess;

 public:
  explicit HStreamWindow(QWidget* parent = nullptr);
  ~HStreamWindow() override;

  QString pipelineStateText() const;
  QString outputStateText(const QString& id) const;
  QString logText() const;
  QString completeLogText() const;
  QString scoreboardSelectorUrl() const;
  QString gameIdText() const;
  QString gameDirectoryText() const;
  int videoSetCount() const;
  int cameraControlValue(const QString& id) const;
  int cameraTabCount() const;

 protected:
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  enum class CopiedImportCleanupResult {
    kSuccess,
    kRolledBack,
    kCommittedWithCleanupFailure,
  };

  struct PendingRuntimeControl {
    QString element;
    QString property;
    QString runtime_value;
    quint64 batch_id;
  };

  struct RuntimeControlBatch {
    std::map<QString, int> controls;
    size_t pending_commands;
    bool failed;
  };

  struct RuntimePropertyCommand {
    QString element;
    QString property;
    QString value;
  };

  enum class PreviewRequestReason {
    kStartup,
    kTabChange,
    kRecovery,
    kRenderToggle,
  };

  enum class PlaybackProgressState {
    kIdle,
    kRunning,
    kCompleted,
    kError,
    kStopped,
  };

  enum class ArchiveFinalizeStage {
    kIdle,
    kRemux,
    kSyncCompleted,
    kSyncRecovery,
  };

  void buildUi();
  void buildTopBar(QVBoxLayout* root);
  void buildMainArea(QVBoxLayout* root);
  void buildGameControls(QVBoxLayout* root);
  void buildPreviewPane(QVBoxLayout* root);
  void buildOutputControls(QVBoxLayout* parent);
  void buildCameraControls(QVBoxLayout* parent, bool program_stage);
  void buildLog(QVBoxLayout* root);
  void configureControlHelp();
  void loadBaselineDefaults();

  void startPipeline();
  void pauseOrResumePipeline();
  void stopPipeline();
  void handlePipelineStarted();
  void handlePipelineFinished(int exit_code, QProcess::ExitStatus exit_status);
  void handlePipelineError(QProcess::ProcessError error);
  void readPipelineOutput();
  bool handleStartupProgressOutput(const QString& line);
  bool handlePlaybackProgressOutput(const QString& line);
  bool handlePlaybackSeekOutput(const QString& line);
  void setPlaybackStartupStage(const QString& stage, const QString& detail);
  void resetPlaybackProgress(bool starting);
  void setPlaybackProgressState(PlaybackProgressState state, const QString& detail = {});
  void updatePlaybackProgressPresentation();
  void updatePlaybackSeekControls();
  void requestPlaybackSeek(qint64 target_ns);
  void requestPlaybackSeekRelative(qint64 delta_ns);
  void beginPlaybackProgressReset();
  void sendPlaybackProgressReset(quint64 generation);
  int playbackProgressResetTimeoutMs() const;
  void handleArchiveOutputStatus(const QString& line);
  void updateArchiveOutputPathLabel();
  void startArchiveFinalization(const QString& source_path, const QString& game_id, bool hevc_video);
  void readArchiveFinalizationProgress();
  void finishArchiveFinalization(int exit_code, QProcess::ExitStatus exit_status);
  bool startArchiveDurabilitySync(const QString& path, ArchiveFinalizeStage stage, QString* error);
  void completeArchiveFinalization();
  void showArchiveFinalizationFailure(const QString& failure_detail);
  void failArchiveFinalization(const QString& message);
  bool acquireArchiveFinalizerOwnership(const QString& source_path, QString* error);
  void releaseArchiveFinalizerOwnership(bool remove_lock_file);
  void showStitchingCalibrationDialog();
  bool beginObservedStitchingCalibration(const QString& reported_stage);
  void handleStitchingCalibrationOutput(const QString& line);
  void setStitchingCalibrationStage(const QString& stage, const QString& status, const QString& message);
  void completeStitchingCalibration();
  void failStitchingCalibration(const QString& message);
  void closeStitchingCalibrationDialog();
  void handleScoreboardSelectorOutput(const QString& line);
  void switchPipelineRenderTarget(int tab_index);
  bool requestPipelinePreviewChannel(const QString& channel, PreviewRequestReason reason);
  QString selectedPipelinePreviewChannel() const;
  int previewReadyTimeoutMs() const;
  void clearPreviewFrames();
  bool handleGpuPreviewStatus(const QString& line);
  bool handlePreviewOverlayResponse(const QString& line);
  QWidget* previewSurfaceForChannel(const QString& channel) const;
  QWidget* previewTargetForChannel(const QString& channel) const;
  void schedulePreviewReadyTimeout(const QString& channel, quint64 generation, int timeout_ms);
  void schedulePreviewDisableTimeout(quint64 generation, int timeout_ms);
  void scheduleInspectorPreviewIdleRetry(int timeout_ms);
  void recoverPreviewDisableFailure(const QString& reason, bool force = false);
  int previewDisableTimeoutMs() const;
  bool setRuntimeRenderAudioMuted(bool muted);
  void setRuntimePreviewOverlays(bool reconciliation = false);
  void setConfirmedPreviewOverlays(bool players, bool play, bool rink);
  void preparePreviewOverlayUserRequest();
  void resetPreviewOverlayReconciliationState();
  bool adoptPreviewOverlayReconciliationFallback(const QString& reason);
  void restoreConfirmedPreviewOverlays(const QString& reason);
  void timeoutPreviewOverlayRequest(quint64 generation);
  void setRuntimeVideoRendering(bool enabled);
  void setPreviewRenderingLayout(bool rendering);
  void setPreviewFocusAvailable(const QString& channel, bool available);
  void setAllPreviewFocusAvailable(bool available);
  bool canFocusPreview(int tab_index) const;
  void togglePreviewFocus(int tab_index);
  void setPreviewFocusMode(bool focused, int tab_index);
  void restartStage();
  void savePreset();
  void resetCameraControls();
  void captureSavedControlState();
  void updatePresetDirtyState();
  void refreshGames();
  void selectGame(const QString& game_id);
  void createOrLoadGame();
  void addVideoPath();
  void browseVideoPath();
  QString videoBrowseStartDirectory() const;
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
  void schedulePlaytrackerRuntimeControl(const QString& id, int value);
  void schedulePlaycropperRuntimeControl(const QString& id, int value);
  QString pipelineRunnerPath() const;
  QString pipelineConfigPath(const QString& config_name) const;
  QString pipelineWorkingDirectory() const;
  QStringList pipelineArguments() const;
  bool setupPretrainedAssets(const QStringList& pipeline_args);
  void logMissingTensorRtEngineCaches(const QStringList& pipeline_args);
  int stitchingCalibrationControlPoints() const;
  int stitchingCalibrationFrameCount() const;
  int stitchingMaxOutputWidth() const;
  QString stitchFrameTime() const;
  QString controlPointMatcher() const;
  QString mappingBackend() const;
  bool prepareStitchingCalibrationRun(
      const QString& runner,
      const QString& working_dir,
      const QProcessEnvironment& env,
      bool* calibration_required);
  bool runStitchingClean(
      const QString& runner,
      const QString& working_dir,
      const QProcessEnvironment& env,
      bool from_control_points,
      const QString& expected_invalidation_id);
  bool saveStitchingCalibrationState(
      const QString& game_id,
      int control_points,
      const QString& status,
      const QString& stale_from,
      const QString& expected_invalidation_id,
      bool artifacts_invalidated,
      bool require_matching_pending,
      bool* applied = nullptr);
  QStringList enabledSinkNames() const;
  bool isCalibrationRun() const;
  void updateRunControls();
  bool applySavedControlConfig(
      YAML::Node& config,
      bool* invalidate_rink_masks,
      int* invalidated_config_artifacts,
      QString* published_playtracker_sidecar);
  void loadSavedControlConfig();
  bool sendLiveCameraControl(const QString& id, int value);
  bool publishRuntimeControlBatch(
      const std::map<QString, int>& controls,
      const std::vector<RuntimePropertyCommand>& commands);
  void flushScheduledRuntimeControls();
  void timeoutRuntimeControlBatch(quint64 batch_id);
  int runtimeControlAckTimeoutMs() const;
  void scheduleRotationRuntimeControl(const QString& id, int value);
  void synchronizeFixedEdgeRotationControls(const QString& changed_id, int value);
  void handleRuntimeControlResponse(const QString& line);
  void failPendingRuntimeControls(const QString& reason);
  QSlider* addSlider(QVBoxLayout* layout, const QString& id, const QString& label, int minimum, int maximum, int value);
  QCheckBox* addCameraCheckBox(QVBoxLayout* layout, const QString& id, const QString& label, bool checked);

  QLabel* backend_mode_{nullptr};
  QLabel* pipeline_state_{nullptr};
  QProgressBar* playback_progress_{nullptr};
  QWidget* playback_seek_controls_{nullptr};
  QSlider* playback_seek_slider_{nullptr};
  QPushButton* playback_seek_back_button_{nullptr};
  QPushButton* playback_seek_forward_button_{nullptr};
  QLabel* playback_seek_position_{nullptr};
  QLabel* preview_status_{nullptr};
  QLabel* stitched_status_{nullptr};
  QLabel* preview_external_notice_{nullptr};
  QLabel* stitched_external_notice_{nullptr};
  QLabel* game_path_label_{nullptr};
  QLabel* video_sets_path_label_{nullptr};
  QLabel* archive_output_path_label_{nullptr};
  QWidget* game_controls_{nullptr};
  QWidget* video_controls_{nullptr};
  QComboBox* game_selector_{nullptr};
  QComboBox* run_mode_selector_{nullptr};
  QSpinBox* control_points_spin_{nullptr};
  QSpinBox* calibration_frame_count_spin_{nullptr};
  QSpinBox* stitch_max_output_width_spin_{nullptr};
  QComboBox* control_point_matcher_combo_{nullptr};
  QComboBox* mapping_backend_combo_{nullptr};
  QTimeEdit* stitch_frame_time_edit_{nullptr};
  QLineEdit* game_id_edit_{nullptr};
  QLineEdit* video_path_edit_{nullptr};
  QListWidget* video_set_list_{nullptr};
  QRadioButton* role_auto_{nullptr};
  QRadioButton* role_left_{nullptr};
  QRadioButton* role_center_{nullptr};
  QRadioButton* role_right_{nullptr};
  QTextEdit* log_{nullptr};
  QTabWidget* preview_tabs_{nullptr};
  PipelineInspectorWidget* pipeline_inspector_{nullptr};
  int pipeline_inspector_tab_index_{-1};
  QWidget* top_bar_{nullptr};
  QWidget* setup_panel_{nullptr};
  QWidget* log_panel_{nullptr};
  QSplitter* main_log_splitter_{nullptr};
  QSplitter* setup_preview_splitter_{nullptr};
  QWidget* preview_surface_{nullptr};
  QWidget* preview_render_target_{nullptr};
  QWidget* stitched_surface_{nullptr};
  QWidget* stitched_render_target_{nullptr};
  std::vector<QWidget*> camera_preview_surfaces_;
  std::vector<QWidget*> camera_preview_render_targets_;
  std::vector<QLabel*> camera_preview_notices_;
  QTabWidget* program_control_tabs_{nullptr};
  QTabWidget* stitched_control_tabs_{nullptr};
  std::vector<QWidget*> preview_hosts_;
  std::vector<QWidget*> associated_control_panels_;
  std::vector<QToolButton*> associated_control_toggles_;
  std::vector<bool> normal_associated_controls_visible_;
  std::vector<QWidget*> focus_hidden_widgets_;
  QVBoxLayout* output_list_{nullptr};
  QProcess* pipeline_process_{nullptr};
  QPushButton* start_button_{nullptr};
  QPushButton* pause_button_{nullptr};
  QPushButton* save_preset_button_{nullptr};
  QPushButton* stop_button_{nullptr};
  QCheckBox* render_video_toggle_{nullptr};
  QCheckBox* show_player_tracking_toggle_{nullptr};
  QCheckBox* show_play_tracking_toggle_{nullptr};
  QCheckBox* show_rink_mask_toggle_{nullptr};
  bool confirmed_show_player_tracking_{false};
  bool confirmed_show_play_tracking_{false};
  bool confirmed_show_rink_mask_{false};
  quint64 preview_overlay_generation_{0};
  quint64 pending_preview_overlay_generation_{0};
  bool preview_overlay_stale_apply_observed_{false};
  bool pending_preview_overlay_is_reconciliation_{false};
  quint64 unresolved_preview_overlay_reconciliation_generation_{0};
  bool preview_overlay_reconciliation_fallback_valid_{false};
  bool preview_overlay_reconciliation_fallback_players_{false};
  bool preview_overlay_reconciliation_fallback_play_{false};
  bool preview_overlay_reconciliation_fallback_rink_{false};
  int preview_overlay_reconciliation_attempts_{0};
  QCheckBox* drivegpt_csv_toggle_{nullptr};
  bool pipeline_paused_{false};
  bool pipeline_uses_process_group_{false};
  bool pipeline_stop_requested_{false};
  bool pipeline_render_embedded_{false};
  QString playback_elapsed_;
  QString playback_total_;
  QString playback_remaining_;
  QString playback_eta_;
  QString playback_speed_;
  QString playback_fps_;
  QString playback_fps_average_;
  QString playback_stage_;
  QString playback_startup_detail_;
  QString playback_instances_;
  QString playback_terminal_detail_;
  PlaybackProgressState playback_progress_state_{PlaybackProgressState::kIdle};
  int playback_progress_x10_{0};
  bool playback_progress_determinate_{false};
  bool playback_warming_after_resume_{false};
  bool playback_accept_stale_after_reset_timeout_{false};
  quint64 playback_reset_generation_{0};
  quint64 pending_playback_reset_generation_{0};
  int playback_reset_attempts_{0};
  qint64 playback_position_ns_{0};
  qint64 playback_duration_ns_{0};
  quint64 playback_seek_generation_{0};
  quint64 pending_playback_seek_generation_{0};
  quint64 playback_seek_recovery_generation_{0};
  bool playback_seek_channel_available_{false};
  bool active_run_local_render_only_{false};
  bool calibration_pending_{false};
  bool calibration_dialog_failed_{false};
  bool calibration_waiting_for_playback_restart_{false};
  bool calibration_playback_restart_observed_{false};
  bool preview_focus_mode_{false};
  bool preview_layout_compacted_{false};
  int focused_preview_tab_{-1};
  QList<int> normal_main_log_sizes_;
  QList<int> normal_setup_preview_sizes_;
  quint64 preview_generation_{1};
  QString active_preview_channel_;
  QString pending_preview_channel_;
  quint64 pending_preview_generation_{0};
  int preview_recovery_attempts_{0};
  int preview_disable_attempts_{0};
  bool preview_runtime_ready_{false};
  std::set<QString> preview_frame_channels_received_;
  QString active_run_game_id_;
  QString active_archive_output_path_;
  QString active_archive_recovery_path_;
  qint64 active_archive_initial_size_{-1};
  qint64 active_archive_initial_mtime_ms_{-1};
  bool active_archive_video_is_hevc_{false};
  QProcess* archive_finalize_process_{nullptr};
  QDialog* archive_finalize_dialog_{nullptr};
  QLabel* archive_finalize_icon_{nullptr};
  QLabel* archive_finalize_headline_{nullptr};
  QLabel* archive_finalize_detail_{nullptr};
  QProgressBar* archive_finalize_progress_{nullptr};
  QPushButton* archive_finalize_ok_button_{nullptr};
  QString archive_finalize_source_path_;
  QString archive_finalize_game_id_;
  QString archive_finalize_target_path_;
  QString archive_finalize_partial_path_;
  QString archive_finalize_temporary_dir_;
  QString archive_finalize_blocked_source_path_;
  QString archive_finalize_stdout_buffer_;
  QString archive_finalize_error_output_;
  QString archive_finalize_pending_failure_detail_;
  QString archive_finalize_owner_lock_path_;
  qint64 archive_finalize_duration_us_{-1};
  int archive_finalize_owner_lock_fd_{-1};
  ArchiveFinalizeStage archive_finalize_stage_{ArchiveFinalizeStage::kIdle};
  bool archive_finalize_failed_{false};
  bool active_run_is_calibration_{false};
  bool active_run_high_bit_depth_{false};
  int active_calibration_control_points_{0};
  int active_calibration_frame_count_{0};
  int active_stitch_max_output_width_{0};
  QString active_stitch_frame_time_;
  QString active_control_point_matcher_;
  QString active_mapping_backend_;
  QString active_calibration_start_stage_;
  QString active_calibration_invalidation_id_;
  bool calibration_restart_requested_{false};
  bool active_force_reconfigure_{false};
  QString pipeline_stdout_buffer_;
  QString pipeline_stderr_buffer_;
  bool capture_complete_log_{false};
  QString complete_log_;
  QString scoreboard_selector_url_;
  ScoreboardSelectionDialog* scoreboard_selection_dialog_{nullptr};
  QDialog* calibration_dialog_{nullptr};
  QLabel* calibration_icon_{nullptr};
  QLabel* calibration_headline_{nullptr};
  QLabel* calibration_detail_{nullptr};
  QProgressBar* calibration_progress_{nullptr};
  QPushButton* calibration_ok_button_{nullptr};
  QPushButton* calibration_cancel_button_{nullptr};
  QString active_calibration_stage_;
  std::map<QString, QLabel*> calibration_stage_icons_;
  std::map<QString, QLabel*> calibration_stage_labels_;
  int dynamic_rtsp_count_{0};
  std::map<QString, QLabel*> output_states_;
  std::map<QString, QCheckBox*> output_toggles_;
  std::map<QString, QSlider*> camera_sliders_;
  std::map<QString, QCheckBox*> camera_checkboxes_;
  std::map<QString, QLabel*> camera_value_labels_;
  std::map<QString, int> camera_defaults_;
  YAML::Node baseline_config_;
  QString baseline_config_root_;
  QString default_stitch_frame_time_{"00:00:00"};
  int default_stitch_max_output_width_{0};
  QString default_control_point_matcher_{"superpoint-lightglue"};
  QString default_mapping_backend_{"nona"};
  std::map<QString, int> saved_camera_controls_;
  int saved_stitching_control_points_{0};
  int saved_stitching_calibration_frame_count_{0};
  int saved_stitch_max_output_width_{0};
  QString development_runtime_root_;
  QString development_pipeline_runner_;
  QString development_bazel_bin_;
  QString saved_stitch_frame_time_;
  QString saved_control_point_matcher_;
  QString saved_mapping_backend_;
  std::set<QString> preset_save_retry_game_ids_;
  std::vector<PendingRuntimeControl> pending_runtime_controls_;
  std::map<quint64, RuntimeControlBatch> runtime_control_batches_;
  std::map<QString, int> scheduled_rotation_controls_;
  std::map<QString, int> scheduled_playtracker_controls_;
  std::map<QString, int> scheduled_playcropper_controls_;
  bool scheduled_rotation_controls_ready_{false};
  bool scheduled_playtracker_controls_ready_{false};
  bool scheduled_playcropper_controls_ready_{false};
  std::optional<std::map<QString, int>> publishing_playtracker_controls_;
  bool scheduled_playtracker_force_all_targets_{false};
  bool publishing_playtracker_force_all_targets_{false};
  quint64 next_runtime_control_batch_id_{0};
  quint64 scheduled_rotation_control_generation_{0};
  quint64 scheduled_playtracker_control_generation_{0};
  quint64 scheduled_playcropper_control_generation_{0};
  QString last_playtracker_runtime_snapshot_;
};
