#pragma once

#include "src/apps/hstream-ui/ArchiveCatalog.h"
#include "src/apps/hstream-ui/HighlightPlan.h"
#include "src/apps/hstream-ui/HighlightsEncodeSettings.h"

#include <QtCore/QList>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

#include <memory>

class QCloseEvent;
class QKeyEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTextEdit;
class QTimer;
class PreviewFocusButton;

namespace hm::ui {

class HighlightsArchivePlayer;
class HighlightsVideoTarget;

// Plans highlight intervals for a game and cuts them out of an already
// published destination archive. Preview is a seek inside that archive and
// export is a single ffmpeg pass over it; the capture pipeline is never re-run.
class HighlightsDialog : public QDialog {
 public:
  HighlightsDialog(
      QString game_id,
      QString game_dir,
      QProcessEnvironment env,
      QVector<ArchiveEntry> archives,
      QWidget* parent = nullptr);
  ~HighlightsDialog() override;

  bool isBusy() const;
  void stop();
  void done(int result) override;

 protected:
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  enum class Job { kNone, kInspect, kPreview, kExport };
  enum class Stage { kIdle, kProbe, kEncoders, kEncode };
  // A planned interval resolved onto the selected archive's own timeline.
  struct Clip {
    HighlightInterval interval;
    qint64 archive_start_ms{0};
    qint64 archive_end_ms{0};
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

  const ArchiveEntry* selectedArchive() const;
  qint64 archiveDurationMs() const;
  void refreshArchiveChoices();
  void applyArchiveSelection();
  void commitArchiveOffset();
  // Maps a planned interval onto the archive. Returns an explanation when the
  // interval does not lie inside it.
  QString resolveClip(const HighlightInterval& interval, Clip* clip) const;
  bool buildQueue(bool selected, QVector<Clip>* clips, QString* error) const;

  void beginPreview(bool selected, bool loop);
  void beginExport(bool selected);
  void pollPreview();
  void stopPreview();

  void startInspection();
  void startEncoderQuery();
  void startEncode();
  void publishEncode();
  void finishJob(bool success, const QString& message);
  void requestActiveProcessStop();
  void readProcessOutput();
  void appendProcessError(const QString& output, bool flush = false);
  void consumeEncodeProgress(const QString& output);
  void processFinished(int code, QProcess::ExitStatus status);
  void appendLog(const QString& line);
  bool embeddedPreviewAvailable() const;
  void setPreviewFocused(bool focused);
  QString finalOutputPath(const QString& route) const;

  QString game_id_;
  QString game_dir_;
  QProcessEnvironment env_;
  QVector<ArchiveEntry> archives_;
  QString plan_path_;
  QString plan_load_error_;
  HighlightPlan plan_;
  HighlightPlan frozen_plan_;
  QVector<Clip> queue_;

  int archive_index_{-1};
  qint64 archive_offset_ms_{0};
  ArchiveMediaInfo media_;
  bool media_valid_{false};
  QString media_path_;
  QString media_error_;
  HighlightsEncodeSettings encode_settings_;
  QStringList encoders_;
  bool encoders_known_{false};

  QString work_dir_;
  QString final_partial_path_;
  QString current_route_;
  QString published_path_;
  QString process_output_buffer_;
  QString process_error_buffer_;
  QString probe_output_;
  qint64 encode_total_ms_{0};
  int encode_percent_{-1};

  quint64 job_generation_{0};
  bool loop_{false};
  bool cancelling_{false};
  bool close_when_stopped_{false};
  int close_result_{QDialog::Rejected};
  Job job_{Job::kNone};
  Stage stage_{Stage::kIdle};
  QProcess process_;

  bool preview_supported_{false};
  std::unique_ptr<HighlightsArchivePlayer> player_;
  QString player_path_;
  QTimer* preview_timer_{nullptr};
  int preview_segment_{-1};

  QTableWidget* table_{nullptr};
  QSplitter* preview_splitter_{nullptr};
  HighlightsVideoTarget* video_{nullptr};
  PreviewFocusButton* expand_preview_button_{nullptr};
  QVector<QWidget*> preview_focus_hidden_;
  QList<int> preview_splitter_sizes_;
  bool preview_focused_{false};
  QComboBox* archive_combo_{nullptr};
  QLineEdit* archive_offset_edit_{nullptr};
  QLabel* archive_detail_{nullptr};
  QLineEdit* label_edit_{nullptr};
  QComboBox* mode_combo_{nullptr};
  QLineEdit* first_edit_{nullptr};
  QLineEdit* second_edit_{nullptr};
  QLineEdit* base_name_edit_{nullptr};
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
