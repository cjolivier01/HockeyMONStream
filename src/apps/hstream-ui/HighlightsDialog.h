#pragma once

#include "src/apps/hstream-ui/HighlightPlan.h"

#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

class QCheckBox;
class QCloseEvent;
class QKeyEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTextEdit;
class ScoreboardSelectionDialog;
class PreviewFocusButton;

namespace hm::ui {

class HighlightsVideoTarget;
class HighlightsDialog : public QDialog {
 public:
  HighlightsDialog(
      QString game_id,
      QString game_dir,
      QString runner,
      QString working_dir,
      QString output_root,
      QProcessEnvironment env,
      QStringList base_runner_args,
      QWidget* parent = nullptr);
  ~HighlightsDialog() override;

  bool isBusy() const;
  void stop();

 protected:
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  enum class Job { kNone, kPreview, kExport };
  enum class Stage { kIdle, kCli, kProbe, kVideoPackets, kConcat };
  struct Chunk {
    HighlightInterval interval;
    QStringList paths;
    QVector<qint64> format_start_time_ms;
    QVector<qint64> video_start_time_ms;
    QVector<qint64> effective_duration_ms;
    bool source_eos{false};
  };

  void refreshTable();
  void loadEditor(int row);
  bool readEditor(HighlightInterval* interval);
  bool savePlan();
  void updateControls();
  void addInterval();
  void updateInterval();
  void removeInterval();
  void moveInterval(int delta);
  void beginPreview(bool selected, bool loop);
  void beginExport(bool selected);
  void beginJob(Job job, bool selected, bool loop);
  void runNextClip();
  void startProbe();
  void startVideoPacketProbe();
  void startConcat();
  void publishConcat();
  void finishJob(bool success, const QString& message);
  void requestActiveProcessStop();
  void readProcessOutput();
  void appendProcessError(const QString& output, bool flush = false);
  void consumeVideoPacketOutput(const QString& output, bool flush);
  void processFinished(int code, QProcess::ExitStatus status);
  void appendLog(const QString& line);
  bool embeddedPreviewAvailable() const;
  void handleScoreboardSelectorOutput(const QString& output);
  void closeScoreboardSelector();
  void setPreviewFocused(bool focused);
  QStringList cliArguments(const HighlightInterval& interval, const QStringList& routes) const;
  QString routeOutputPath(int clip_index, const QString& route) const;
  QString finalOutputPath(const QString& route) const;
  static QString routeSink(const QString& route);
  static QString routeName(int sink_id);

  QString game_id_;
  QString game_dir_;
  QString runner_;
  QString working_dir_;
  QString output_root_;
  QProcessEnvironment env_;
  QStringList base_runner_args_;
  QString plan_path_;
  QString plan_load_error_;
  HighlightPlan plan_;
  HighlightPlan frozen_plan_;
  QVector<HighlightInterval> queue_;
  QVector<Chunk> chunks_;
  QStringList routes_;
  QString work_dir_;
  QString publication_work_dir_;
  QString final_partial_path_;
  QStringList published_paths_;
  QString process_output_buffer_;
  QString process_error_buffer_;
  QString scoreboard_selector_output_tail_;
  QString scoreboard_selector_url_;
  QString probe_output_;
  QString probe_packet_buffer_;
  QString probe_baseline_;
  double probe_video_start_seconds_{0};
  double probe_video_end_seconds_{0};
  bool probe_saw_video_packet_{false};
  bool probe_video_packet_error_{false};
  QStringList route_video_codecs_;
  QVector<bool> route_has_audio_;
  QString current_route_;
  QString current_expected_path_;
  int queue_index_{0};
  int probe_clip_index_{0};
  int probe_route_index_{0};
  int concat_route_index_{0};
  quint64 job_generation_{0};
  bool loop_{false};
  bool cancelling_{false};
  bool close_when_stopped_{false};
  QString current_cli_result_;
  Job job_{Job::kNone};
  Stage stage_{Stage::kIdle};
  QProcess process_;
  QPointer<ScoreboardSelectionDialog> scoreboard_selection_dialog_;

  QTableWidget* table_{nullptr};
  QSplitter* preview_splitter_{nullptr};
  HighlightsVideoTarget* video_{nullptr};
  PreviewFocusButton* expand_preview_button_{nullptr};
  QVector<QWidget*> preview_focus_hidden_;
  QList<int> preview_splitter_sizes_;
  bool preview_focused_{false};
  QLineEdit* label_edit_{nullptr};
  QComboBox* mode_combo_{nullptr};
  QLineEdit* first_edit_{nullptr};
  QLineEdit* second_edit_{nullptr};
  QLineEdit* base_name_edit_{nullptr};
  QCheckBox* program_check_{nullptr};
  QCheckBox* program_4k_check_{nullptr};
  QCheckBox* stitched_check_{nullptr};
  QPushButton* add_button_{nullptr};
  QPushButton* update_button_{nullptr};
  QPushButton* remove_button_{nullptr};
  QPushButton* up_button_{nullptr};
  QPushButton* down_button_{nullptr};
  QPushButton* preview_selected_button_{nullptr};
  QPushButton* preview_all_button_{nullptr};
  QPushButton* loop_selected_button_{nullptr};
  QPushButton* loop_button_{nullptr};
  QPushButton* export_selected_button_{nullptr};
  QPushButton* export_all_button_{nullptr};
  QPushButton* stop_button_{nullptr};
  QLabel* status_{nullptr};
  QTextEdit* log_{nullptr};
  bool log_follows_tail_{true};
  bool log_scroll_is_programmatic_{false};
};

} // namespace hm::ui
