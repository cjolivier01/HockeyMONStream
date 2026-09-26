#include "src/apps/hstream-ui/HighlightsDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace hm::ui {
namespace {

QString safeFileComponent(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
  value.remove(QRegularExpression("^_+|_+$"));
  return value.left(64);
}

QString ffconcatSeconds(qint64 milliseconds) {
  return QString::number(static_cast<double>(milliseconds) / 1000.0, 'f', 3);
}

QString streamSignature(const QJsonObject& stream) {
  QJsonObject normalized;
  for (const char* key :
       {"codec_type",
        "codec_name",
        "profile",
        "level",
        "width",
        "height",
        "pix_fmt",
        "sample_rate",
        "channels",
        "channel_layout"}) {
    if (stream.contains(QLatin1String(key)))
      normalized.insert(QLatin1String(key), stream.value(QLatin1String(key)));
  }
  return QString::fromUtf8(QJsonDocument(normalized).toJson(QJsonDocument::Compact));
}

} // namespace

HighlightsDialog::HighlightsDialog(
    QString game_id,
    QString game_dir,
    QString runner,
    QString working_dir,
    QString output_root,
    QProcessEnvironment env,
    QStringList base_runner_args,
    QWidget* parent)
    : QDialog(parent),
      game_id_(std::move(game_id)),
      game_dir_(std::move(game_dir)),
      runner_(std::move(runner)),
      working_dir_(std::move(working_dir)),
      output_root_(std::move(output_root)),
      env_(std::move(env)),
      base_runner_args_(std::move(base_runner_args)),
      plan_path_(QDir(game_dir_).filePath("highlights.json")) {
  setObjectName("highlightsDialog");
  setWindowTitle("Highlights — " + game_id_);
  resize(850, 660);
  auto* root = new QVBoxLayout(this);
  auto* intro = new QLabel("Add event times or exact ranges. Event clips use 75% of the duration before the event.");
  intro->setWordWrap(true);
  root->addWidget(intro);

  table_ = new QTableWidget(0, 5, this);
  table_->setObjectName("highlightsTable");
  table_->setHorizontalHeaderLabels({"Name", "Input", "Start", "End", "Length"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column < 5; ++column)
    table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  root->addWidget(table_, 1);
  connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) {
    loadEditor(row);
    updateControls();
  });

  auto* editor = new QFormLayout();
  label_edit_ = new QLineEdit(this);
  label_edit_->setObjectName("highlightLabelEdit");
  editor->addRow("Name", label_edit_);
  mode_combo_ = new QComboBox(this);
  mode_combo_->setObjectName("highlightModeCombo");
  mode_combo_->addItem("Event + duration", true);
  mode_combo_->addItem("Start + end", false);
  editor->addRow("Timing", mode_combo_);
  first_edit_ = new QLineEdit(this);
  first_edit_->setObjectName("highlightFirstTimeEdit");
  first_edit_->setPlaceholderText("HH:MM:SS.mmm");
  editor->addRow("Event / start", first_edit_);
  second_edit_ = new QLineEdit(this);
  second_edit_->setObjectName("highlightSecondTimeEdit");
  second_edit_->setPlaceholderText("HH:MM:SS.mmm");
  editor->addRow("Duration / end", second_edit_);
  root->addLayout(editor);
  connect(mode_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    first_edit_->setPlaceholderText(mode_combo_->currentData().toBool() ? "Event HH:MM:SS.mmm" : "Start HH:MM:SS.mmm");
    second_edit_->setPlaceholderText(
        mode_combo_->currentData().toBool() ? "Duration HH:MM:SS.mmm" : "End HH:MM:SS.mmm");
  });

  auto* edit_actions = new QHBoxLayout();
  add_button_ = new QPushButton("Add", this);
  add_button_->setObjectName("highlightAddButton");
  update_button_ = new QPushButton("Apply changes", this);
  update_button_->setObjectName("highlightUpdateButton");
  remove_button_ = new QPushButton("Remove", this);
  remove_button_->setObjectName("highlightRemoveButton");
  up_button_ = new QPushButton("Move up", this);
  down_button_ = new QPushButton("Move down", this);
  for (auto* button : {add_button_, update_button_, remove_button_, up_button_, down_button_})
    edit_actions->addWidget(button);
  edit_actions->addStretch();
  root->addLayout(edit_actions);
  connect(add_button_, &QPushButton::clicked, this, &HighlightsDialog::addInterval);
  connect(update_button_, &QPushButton::clicked, this, &HighlightsDialog::updateInterval);
  connect(remove_button_, &QPushButton::clicked, this, &HighlightsDialog::removeInterval);
  connect(up_button_, &QPushButton::clicked, this, [this] { moveInterval(-1); });
  connect(down_button_, &QPushButton::clicked, this, [this] { moveInterval(1); });

  auto* playback = new QHBoxLayout();
  preview_selected_button_ = new QPushButton("Preview selected", this);
  preview_selected_button_->setObjectName("highlightPreviewSelectedButton");
  preview_all_button_ = new QPushButton("Preview all", this);
  preview_all_button_->setObjectName("highlightPreviewAllButton");
  loop_selected_button_ = new QPushButton("Loop selected", this);
  loop_selected_button_->setObjectName("highlightLoopSelectedButton");
  loop_button_ = new QPushButton("Loop all", this);
  loop_button_->setObjectName("highlightLoopButton");
  for (auto* button : {preview_selected_button_, preview_all_button_, loop_selected_button_, loop_button_})
    playback->addWidget(button);
  playback->addStretch();
  root->addLayout(playback);
  connect(preview_selected_button_, &QPushButton::clicked, this, [this] { beginPreview(true, false); });
  connect(preview_all_button_, &QPushButton::clicked, this, [this] { beginPreview(false, false); });
  connect(loop_selected_button_, &QPushButton::clicked, this, [this] { beginPreview(true, true); });
  connect(loop_button_, &QPushButton::clicked, this, [this] { beginPreview(false, true); });

  auto* export_row = new QHBoxLayout();
  export_row->addWidget(new QLabel("Base name", this));
  base_name_edit_ = new QLineEdit(this);
  base_name_edit_->setObjectName("highlightBaseNameEdit");
  base_name_edit_->setMaximumWidth(170);
  export_row->addWidget(base_name_edit_);
  program_check_ = new QCheckBox("Program", this);
  program_check_->setObjectName("highlightProgramCheck");
  program_check_->setChecked(true);
  program_4k_check_ = new QCheckBox("4K Program", this);
  program_4k_check_->setObjectName("highlightProgram4kCheck");
  stitched_check_ = new QCheckBox("Stitched", this);
  stitched_check_->setObjectName("highlightStitchedCheck");
  for (auto* check : {program_check_, program_4k_check_, stitched_check_})
    export_row->addWidget(check);
  export_row->addStretch();
  root->addLayout(export_row);
  connect(base_name_edit_, &QLineEdit::editingFinished, this, [this] {
    if (isBusy())
      return;
    const QString previous = plan_.base_name;
    plan_.base_name = base_name_edit_->text().trimmed();
    if (!savePlan())
      plan_.base_name = previous;
  });

  auto* export_actions = new QHBoxLayout();
  export_selected_button_ = new QPushButton("Export selected", this);
  export_selected_button_->setObjectName("highlightExportSelectedButton");
  export_all_button_ = new QPushButton("Export all", this);
  export_all_button_->setObjectName("highlightExportAllButton");
  stop_button_ = new QPushButton("Stop", this);
  stop_button_->setObjectName("highlightStopButton");
  export_actions->addWidget(export_selected_button_);
  export_actions->addWidget(export_all_button_);
  export_actions->addStretch();
  export_actions->addWidget(stop_button_);
  root->addLayout(export_actions);
  connect(export_selected_button_, &QPushButton::clicked, this, [this] { beginExport(true); });
  connect(export_all_button_, &QPushButton::clicked, this, [this] { beginExport(false); });
  connect(stop_button_, &QPushButton::clicked, this, &HighlightsDialog::stop);

  status_ = new QLabel(this);
  status_->setObjectName("highlightStatus");
  status_->setWordWrap(true);
  root->addWidget(status_);
  log_ = new QPlainTextEdit(this);
  log_->setObjectName("highlightLog");
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(400);
  log_->setMaximumHeight(145);
  root->addWidget(log_);

  process_.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this, &HighlightsDialog::readProcessOutput);
  connect(&process_, &QProcess::readyReadStandardError, this, &HighlightsDialog::readProcessOutput);
  connect(
      &process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &HighlightsDialog::processFinished);
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart)
      finishJob(false, "Could not start " + process_.program() + ": " + process_.errorString());
  });

  QString load_error;
  if (!LoadHighlightPlan(plan_path_, &plan_, &load_error))
    status_->setText("Could not load highlights: " + load_error);
  else
    status_->setText("Ready. Intervals are saved with this game.");
  base_name_edit_->setText(plan_.base_name);
  refreshTable();
  updateControls();
}

HighlightsDialog::~HighlightsDialog() {
  if (process_.state() != QProcess::NotRunning) {
    process_.terminate();
    if (!process_.waitForFinished(4000)) {
      process_.kill();
      process_.waitForFinished(1000);
    }
  }
}

bool HighlightsDialog::isBusy() const {
  return job_ != Job::kNone;
}

void HighlightsDialog::closeEvent(QCloseEvent* event) {
  if (isBusy()) {
    close_when_stopped_ = true;
    stop();
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

void HighlightsDialog::appendLog(const QString& line) {
  if (!line.trimmed().isEmpty())
    log_->appendPlainText(line.trimmed());
}

void HighlightsDialog::refreshTable() {
  const int selected = table_->currentRow();
  table_->setRowCount(plan_.intervals.size());
  for (int row = 0; row < plan_.intervals.size(); ++row) {
    const auto& interval = plan_.intervals[row];
    const QStringList cells = {
        interval.label,
        interval.event_mode ? "Event " + FormatHighlightTime(interval.event_ms) : "Range",
        FormatHighlightTime(interval.start_ms),
        FormatHighlightTime(interval.end_ms),
        FormatHighlightTime(interval.end_ms - interval.start_ms)};
    for (int column = 0; column < cells.size(); ++column)
      table_->setItem(row, column, new QTableWidgetItem(cells[column]));
  }
  if (!plan_.intervals.isEmpty())
    table_->setCurrentCell(std::clamp(selected, 0, static_cast<int>(plan_.intervals.size()) - 1), 0);
  else
    loadEditor(-1);
  updateControls();
}

void HighlightsDialog::loadEditor(int row) {
  if (row < 0 || row >= plan_.intervals.size())
    return;
  const auto& interval = plan_.intervals[row];
  label_edit_->setText(interval.label);
  mode_combo_->setCurrentIndex(interval.event_mode ? 0 : 1);
  first_edit_->setText(FormatHighlightTime(interval.event_mode ? interval.event_ms : interval.start_ms));
  second_edit_->setText(FormatHighlightTime(interval.event_mode ? interval.duration_ms : interval.end_ms));
}

bool HighlightsDialog::readEditor(HighlightInterval* interval) {
  interval->label = label_edit_->text().trimmed();
  interval->event_mode = mode_combo_->currentData().toBool();
  QString error;
  qint64 first = 0;
  qint64 second = 0;
  if (!ParseHighlightTime(first_edit_->text(), &first, &error) ||
      !ParseHighlightTime(second_edit_->text(), &second, &error)) {
    status_->setText("Invalid time: " + error);
    return false;
  }
  if (interval->event_mode) {
    interval->event_ms = first;
    interval->duration_ms = second;
  } else {
    interval->start_ms = first;
    interval->end_ms = second;
  }
  if (!NormalizeHighlightInterval(interval, &error)) {
    status_->setText("Invalid interval: " + error);
    return false;
  }
  return true;
}

bool HighlightsDialog::savePlan() {
  QString error;
  if (SaveHighlightPlan(plan_path_, plan_, &error)) {
    status_->setText("Intervals saved to " + plan_path_);
    return true;
  }
  status_->setText("Could not save intervals: " + error);
  return false;
}

void HighlightsDialog::updateControls() {
  const bool idle = !isBusy();
  const bool selected = table_->currentRow() >= 0 && table_->currentRow() < plan_.intervals.size();
  for (auto* widget :
       {static_cast<QWidget*>(table_),
        static_cast<QWidget*>(label_edit_),
        static_cast<QWidget*>(mode_combo_),
        static_cast<QWidget*>(first_edit_),
        static_cast<QWidget*>(second_edit_),
        static_cast<QWidget*>(base_name_edit_),
        static_cast<QWidget*>(program_check_),
        static_cast<QWidget*>(program_4k_check_),
        static_cast<QWidget*>(stitched_check_)})
    widget->setEnabled(idle);
  add_button_->setEnabled(idle);
  update_button_->setEnabled(idle && selected);
  remove_button_->setEnabled(idle && selected);
  up_button_->setEnabled(idle && selected && table_->currentRow() > 0);
  down_button_->setEnabled(idle && selected && table_->currentRow() + 1 < plan_.intervals.size());
  preview_selected_button_->setEnabled(idle && selected);
  preview_all_button_->setEnabled(idle && !plan_.intervals.isEmpty());
  loop_selected_button_->setEnabled(idle && selected);
  loop_button_->setEnabled(idle && !plan_.intervals.isEmpty());
  export_selected_button_->setEnabled(idle && selected);
  export_all_button_->setEnabled(idle && !plan_.intervals.isEmpty());
  stop_button_->setEnabled(!idle);
}

void HighlightsDialog::addInterval() {
  HighlightInterval interval;
  if (!readEditor(&interval))
    return;
  plan_.intervals.append(interval);
  if (!savePlan()) {
    plan_.intervals.removeLast();
    return;
  }
  refreshTable();
  table_->setCurrentCell(plan_.intervals.size() - 1, 0);
}

void HighlightsDialog::updateInterval() {
  const int row = table_->currentRow();
  if (row < 0 || row >= plan_.intervals.size())
    return;
  HighlightInterval interval;
  if (!readEditor(&interval))
    return;
  const auto previous = plan_.intervals[row];
  plan_.intervals[row] = interval;
  if (!savePlan()) {
    plan_.intervals[row] = previous;
    return;
  }
  refreshTable();
}

void HighlightsDialog::removeInterval() {
  const int row = table_->currentRow();
  if (row < 0 || row >= plan_.intervals.size())
    return;
  const auto previous = plan_.intervals;
  plan_.intervals.removeAt(row);
  if (!savePlan()) {
    plan_.intervals = previous;
    return;
  }
  refreshTable();
}

void HighlightsDialog::moveInterval(int delta) {
  const int row = table_->currentRow();
  if (row < 0 || row + delta < 0 || row + delta >= plan_.intervals.size())
    return;
  plan_.intervals.swapItemsAt(row, row + delta);
  if (!savePlan()) {
    plan_.intervals.swapItemsAt(row, row + delta);
    return;
  }
  refreshTable();
  table_->setCurrentCell(row + delta, 0);
}

void HighlightsDialog::beginPreview(bool selected, bool loop) {
  beginJob(Job::kPreview, selected, loop);
}

void HighlightsDialog::beginExport(bool selected) {
  beginJob(Job::kExport, selected, false);
}

void HighlightsDialog::beginJob(Job job, bool selected, bool loop) {
  if (isBusy())
    return;
  if (plan_.intervals.isEmpty()) {
    status_->setText("Add an interval first.");
    return;
  }
  frozen_plan_ = plan_;
  frozen_plan_.base_name = base_name_edit_->text().trimmed();
  if (job == Job::kExport && safeFileComponent(frozen_plan_.base_name).isEmpty()) {
    status_->setText("Enter a base name for the video.");
    return;
  }
  const QString previous_base_name = plan_.base_name;
  plan_.base_name = frozen_plan_.base_name;
  if (!savePlan()) {
    plan_.base_name = previous_base_name;
    return;
  }
  queue_.clear();
  if (selected) {
    const int row = table_->currentRow();
    if (row < 0 || row >= frozen_plan_.intervals.size())
      return;
    queue_.append(frozen_plan_.intervals[row]);
  } else {
    queue_ = frozen_plan_.intervals;
  }
  routes_.clear();
  if (job == Job::kExport) {
    if (program_check_->isChecked())
      routes_ << "program";
    if (program_4k_check_->isChecked())
      routes_ << "program_4k";
    if (stitched_check_->isChecked())
      routes_ << "stitched";
    if (routes_.isEmpty()) {
      status_->setText("Choose at least one output.");
      return;
    }
    route_video_codecs_.clear();
    route_has_audio_.clear();
    for (int i = 0; i < routes_.size(); ++i)
      route_video_codecs_ << QString();
    for (int i = 0; i < routes_.size(); ++i)
      route_has_audio_.append(false);
    const QString work_parent = QDir(output_root_).filePath(safeFileComponent(game_id_));
    if (!QDir().mkpath(work_parent)) {
      status_->setText("Could not create highlights output directory at " + work_parent);
      return;
    }
    QTemporaryDir work(QDir(work_parent).filePath(".highlights-work-XXXXXX"));
    if (!work.isValid()) {
      status_->setText("Could not create highlights work directory in " + work_parent);
      return;
    }
    work.setAutoRemove(false);
    work_dir_ = work.path();
    QTemporaryDir publication(QDir(game_dir_).filePath(".highlights-finalize-XXXXXX"));
    if (!publication.isValid()) {
      QDir(work_dir_).removeRecursively();
      work_dir_.clear();
      status_->setText("Could not create highlights publication directory in " + game_dir_);
      return;
    }
    publication.setAutoRemove(false);
    publication_work_dir_ = publication.path();
  }
  job_ = job;
  stage_ = Stage::kIdle;
  loop_ = loop;
  cancelling_ = false;
  queue_index_ = 0;
  chunks_.clear();
  published_paths_.clear();
  process_output_buffer_.clear();
  log_->clear();
  status_->setText(job == Job::kPreview ? "Starting preview…" : "Encoding highlight intervals…");
  updateControls();
  runNextClip();
}

QString HighlightsDialog::routeSink(const QString& route) {
  if (route == "program")
    return "ENCODE_FILE";
  if (route == "program_4k")
    return "ENCODE_PROGRAM_4K_FILE";
  return "ENCODE_STITCHED_FILE";
}

QString HighlightsDialog::routeName(int sink_id) {
  if (sink_id == 2)
    return "program";
  if (sink_id == 5)
    return "stitched";
  if (sink_id == 6)
    return "program_4k";
  return {};
}

QString HighlightsDialog::routeOutputPath(int clip_index, const QString& route) const {
  return QDir(work_dir_).filePath(QString("clip-%1-%2.mkv").arg(clip_index, 4, 10, QLatin1Char('0')).arg(route));
}

QStringList HighlightsDialog::cliArguments(const HighlightInterval& interval, const QStringList& routes) const {
  QStringList args;
  bool skip_next = false;
  for (const QString& arg : base_runner_args_) {
    if (skip_next) {
      skip_next = false;
      continue;
    }
    if (arg == "-t" || arg == "--time-limit" || arg == "--start-time" || arg == "--enable-sinks") {
      skip_next = true;
      continue;
    }
    if (arg == "--show" || arg.startsWith("--start-time=") || arg.startsWith("--time-limit=") ||
        arg.startsWith("--clip-end-time=") || arg.startsWith("--enable-sinks=") || arg.startsWith("--ui-preview-") ||
        arg.startsWith("--show-scaled=") ||
        arg.startsWith("--options=pipeline.ds-playtracker.private-properties.telemetry-") ||
        arg.startsWith("--options=pipeline.sink2.output-file=") ||
        arg.startsWith("--options=pipeline.sink5.output-file=") ||
        arg.startsWith("--options=pipeline.sink6.output-file="))
      continue;
    args << arg;
  }
  QStringList sink_names;
  for (const QString& route : routes)
    sink_names << routeSink(route);
  if (routes.isEmpty()) {
    sink_names << "RENDER";
    args << "--show";
  }
  args << "--enable-sinks=" + sink_names.join(',');
  args << "--start-time=" + FormatHighlightTime(interval.start_ms);
  args << "--clip-end-time=" + FormatHighlightTime(interval.end_ms);
  args << "-t" << QString::number((interval.end_ms - interval.start_ms + 999) / 1000 + 2);
  for (const QString& route : routes) {
    const QString sink_id = route == "program" ? "2" : route == "stitched" ? "5" : "6";
    args << QString("--options=pipeline.sink%1.output-file=%2").arg(sink_id, routeOutputPath(queue_index_, route));
  }
  return args;
}

void HighlightsDialog::runNextClip() {
  if (cancelling_)
    return;
  if (queue_index_ >= queue_.size()) {
    if (job_ == Job::kPreview) {
      if (loop_) {
        queue_index_ = 0;
      } else {
        finishJob(true, "Preview complete.");
        return;
      }
    } else {
      probe_clip_index_ = 0;
      probe_route_index_ = 0;
      probe_baseline_.clear();
      startProbe();
      return;
    }
  }
  stage_ = Stage::kCli;
  process_output_buffer_.clear();
  current_expected_path_.clear();
  current_cli_result_.clear();
  const auto& interval = queue_[queue_index_];
  status_->setText(QString("%1 clip %2 of %3: %4")
                       .arg(job_ == Job::kPreview ? "Previewing" : "Encoding")
                       .arg(queue_index_ + 1)
                       .arg(queue_.size())
                       .arg(interval.label));
  appendLog(QString("%1 %2–%3")
                .arg(interval.label, FormatHighlightTime(interval.start_ms), FormatHighlightTime(interval.end_ms)));
  process_.setWorkingDirectory(working_dir_);
  process_.setProcessEnvironment(env_);
  process_.start(runner_, cliArguments(interval, job_ == Job::kExport ? routes_ : QStringList{}));
}

void HighlightsDialog::readProcessOutput() {
  const QString stdout_text = QString::fromLocal8Bit(process_.readAllStandardOutput());
  const QString stderr_text = QString::fromLocal8Bit(process_.readAllStandardError());
  if (stage_ == Stage::kProbe) {
    probe_output_ += stdout_text;
    if (!stderr_text.trimmed().isEmpty())
      appendLog(stderr_text.trimmed());
    return;
  }
  process_output_buffer_ += stdout_text;
  while (true) {
    const int newline = process_output_buffer_.indexOf('\n');
    if (newline < 0)
      break;
    const QString line = process_output_buffer_.left(newline).trimmed();
    process_output_buffer_.remove(0, newline + 1);
    if (stage_ == Stage::kCli && line.startsWith("HSTREAM_CLIP_RESULT reason="))
      current_cli_result_ = line.mid(QString("HSTREAM_CLIP_RESULT reason=").size());
    if (stage_ == Stage::kCli && job_ == Job::kExport && line.startsWith("HSTREAM_OUTPUT type=archive ")) {
      const auto sink_match = QRegularExpression("\\bsink=(\\d+)").match(line);
      const auto path_match = QRegularExpression("\\bpath=(.*)$").match(line);
      if (sink_match.hasMatch() && path_match.hasMatch()) {
        const QString route = routeName(sink_match.captured(1).toInt());
        if (routes_.contains(route)) {
          if (chunks_.size() == queue_index_) {
            Chunk chunk;
            chunk.interval = queue_[queue_index_];
            for (int i = 0; i < routes_.size(); ++i) {
              chunk.paths << QString();
              chunk.start_time_ms << 0;
              chunk.effective_duration_ms << chunk.interval.end_ms - chunk.interval.start_ms;
            }
            chunks_.append(chunk);
          }
          chunks_[queue_index_].paths[routes_.indexOf(route)] = path_match.captured(1);
        }
      }
    }
    if (line.startsWith("HSTREAM_OUTPUT") || line.contains("ERROR", Qt::CaseInsensitive))
      appendLog(line);
  }
  if (!stderr_text.trimmed().isEmpty())
    appendLog(stderr_text.right(3000).trimmed());
}

void HighlightsDialog::processFinished(int code, QProcess::ExitStatus status) {
  if (job_ == Job::kNone)
    return;
  readProcessOutput();
  if (cancelling_) {
    finishJob(false, "Highlights job stopped.");
    return;
  }
  if (status != QProcess::NormalExit || code != 0) {
    finishJob(
        false,
        QString("%1 failed with exit code %2. See the log below.")
            .arg(
                stage_ == Stage::kCli         ? "Playback"
                    : stage_ == Stage::kProbe ? "ffprobe"
                                              : "ffmpeg")
            .arg(code));
    return;
  }
  switch (stage_) {
    case Stage::kCli: {
      if (current_cli_result_ != "end-boundary" && current_cli_result_ != "source-eos") {
        finishJob(false, "The clip did not reach its end or a natural source end.");
        return;
      }
      if (job_ == Job::kExport) {
        if (chunks_.size() <= queue_index_) {
          finishJob(false, "No video was produced; this interval may start beyond the source end.");
          return;
        }
        chunks_[queue_index_].source_eos = current_cli_result_ == "source-eos";
        for (int route_index = 0; route_index < routes_.size(); ++route_index) {
          const QString path = chunks_[queue_index_].paths[route_index];
          if (path.isEmpty() || !QFileInfo(path).isFile() || QFileInfo(path).size() <= 0 ||
              !QDir::cleanPath(path).startsWith(QDir::cleanPath(work_dir_) + '/')) {
            finishJob(
                false, "A clip archive is missing or empty; this interval may start beyond the source end: " + path);
            return;
          }
        }
      }
      ++queue_index_;
      runNextClip();
      return;
    }
    case Stage::kProbe: {
      QJsonParseError error;
      const auto document = QJsonDocument::fromJson(probe_output_.toUtf8(), &error);
      if (error.error != QJsonParseError::NoError || !document.isObject()) {
        finishJob(false, "Could not read ffprobe stream metadata for " + current_expected_path_);
        return;
      }
      const auto json = document.object();
      const auto streams = json.value("streams").toArray();
      QStringList signatures;
      bool video = false;
      bool audio = false;
      for (const auto& entry : streams) {
        const auto stream = entry.toObject();
        const QString type = stream.value("codec_type").toString();
        if (type == "video") {
          video = true;
          if (route_video_codecs_[probe_route_index_].isEmpty())
            route_video_codecs_[probe_route_index_] = stream.value("codec_name").toString();
          signatures << streamSignature(stream);
        } else if (type == "audio") {
          audio = true;
          signatures << streamSignature(stream);
        }
      }
      if (!video) {
        finishJob(false, "A clip is missing video: " + current_expected_path_);
        return;
      }
      if (probe_clip_index_ == 0)
        route_has_audio_[probe_route_index_] = audio;
      const QString signature = signatures.join('|');
      if (probe_baseline_.isEmpty())
        probe_baseline_ = signature;
      else if (probe_baseline_ != signature) {
        finishJob(
            false, "Clip streams differ; cannot join without re-encoding. Work files are retained in " + work_dir_);
        return;
      }
      const auto format = json.value("format").toObject();
      const double duration = format.value("duration").toString().toDouble();
      const double start_time = format.value("start_time").toString().toDouble();
      const double expected =
          static_cast<double>(
              chunks_[probe_clip_index_].interval.end_ms - chunks_[probe_clip_index_].interval.start_ms) /
          1000.0;
      if (!std::isfinite(duration) || duration <= 0) {
        finishJob(false, "A clip has no measurable duration: " + current_expected_path_);
        return;
      }
      if (duration + 0.5 < expected && !chunks_[probe_clip_index_].source_eos) {
        finishJob(false, "A clip ended before its requested interval: " + current_expected_path_);
        return;
      }
      if (duration + 0.5 < expected)
        appendLog(QString("Clip %1 ended at source EOS after %2 seconds; joining the available video.")
                      .arg(probe_clip_index_ + 1)
                      .arg(duration, 0, 'f', 3));
      chunks_[probe_clip_index_].effective_duration_ms[probe_route_index_] = std::min<qint64>(
          chunks_[probe_clip_index_].effective_duration_ms[probe_route_index_],
          static_cast<qint64>(std::llround(duration * 1000.0)));
      chunks_[probe_clip_index_].start_time_ms[probe_route_index_] =
          std::isfinite(start_time) ? static_cast<qint64>(std::llround(start_time * 1000.0)) : 0;
      ++probe_clip_index_;
      if (probe_clip_index_ >= chunks_.size()) {
        probe_clip_index_ = 0;
        ++probe_route_index_;
        probe_baseline_.clear();
      }
      startProbe();
      return;
    }
    case Stage::kConcat:
      publishConcat();
      return;
    case Stage::kIdle:
      return;
  }
}

void HighlightsDialog::startProbe() {
  if (probe_route_index_ >= routes_.size()) {
    concat_route_index_ = 0;
    startConcat();
    return;
  }
  const QString ffprobe = env_.value("HSTREAM_UI_FFPROBE", "ffprobe");
  stage_ = Stage::kProbe;
  probe_output_.clear();
  current_expected_path_ = chunks_[probe_clip_index_].paths[probe_route_index_];
  status_->setText(QString("Checking %1 clip %2 of %3")
                       .arg(routes_[probe_route_index_])
                       .arg(probe_clip_index_ + 1)
                       .arg(chunks_.size()));
  process_.setWorkingDirectory(work_dir_);
  process_.setProcessEnvironment(env_);
  process_.start(ffprobe, {"-v", "error", "-show_streams", "-show_format", "-of", "json", current_expected_path_});
}

void HighlightsDialog::startConcat() {
  if (concat_route_index_ >= routes_.size()) {
    finishJob(true, "Highlights saved: " + published_paths_.join(", "));
    return;
  }
  current_route_ = routes_[concat_route_index_];
  const QString manifest_path = QDir(work_dir_).filePath("concat-" + current_route_ + ".ffconcat");
  QSaveFile manifest(manifest_path);
  if (!manifest.open(QIODevice::WriteOnly | QIODevice::Text)) {
    finishJob(false, "Could not create concat manifest: " + manifest.errorString());
    return;
  }
  QByteArray content = "ffconcat version 1.0\n";
  for (int clip_index = 0; clip_index < chunks_.size(); ++clip_index) {
    const QString source = chunks_[clip_index].paths[concat_route_index_];
    const QString controlled_name = QString("input-%1-%2.mkv").arg(clip_index).arg(current_route_);
    const QString link_path = QDir(work_dir_).filePath(controlled_name);
    QFile::remove(link_path);
#ifdef Q_OS_UNIX
    const bool staged = ::link(QFile::encodeName(source).constData(), QFile::encodeName(link_path).constData()) == 0;
#else
    const bool staged = QFile::copy(source, link_path);
#endif
    if (!staged) {
      finishJob(false, "Could not stage clip for joining: " + source);
      return;
    }
    const qint64 duration_ms = chunks_[clip_index].effective_duration_ms[concat_route_index_];
    content += "file '" + controlled_name.toUtf8() + "'\n";
    const qint64 start_ms = chunks_[clip_index].start_time_ms[concat_route_index_];
    content += "outpoint " + ffconcatSeconds(start_ms + duration_ms).toUtf8() + "\n";
    content += "duration " + ffconcatSeconds(duration_ms).toUtf8() + "\n";
  }
  if (manifest.write(content) != content.size() || !manifest.commit()) {
    finishJob(false, "Could not write concat manifest: " + manifest.errorString());
    return;
  }
  final_partial_path_ = QDir(publication_work_dir_).filePath("joined-" + current_route_ + ".mp4");
  stage_ = Stage::kConcat;
  status_->setText("Joining " + current_route_ + " clips…");
  process_.setWorkingDirectory(work_dir_);
  process_.setProcessEnvironment(env_);
  QStringList args = {
      "-hide_banner", "-nostdin", "-n", "-f", "concat", "-safe", "1", "-i", manifest_path, "-map", "0:v:0"};
  if (route_has_audio_[concat_route_index_])
    args << "-map" << "0:a:0";
  args << "-c" << "copy" << "-movflags" << "+faststart";
  if (route_video_codecs_[concat_route_index_] == "hevc")
    args << "-tag:v" << "hvc1";
  args << final_partial_path_;
  process_.start(env_.value("HSTREAM_UI_FFMPEG", "ffmpeg"), args);
}

QString HighlightsDialog::finalOutputPath(const QString& route) const {
  const QString game = safeFileComponent(game_id_);
  const QString prefix = QString("%1-%2-%3").arg(game, safeFileComponent(frozen_plan_.base_name), route);
  for (int generation = 1; generation <= 10000; ++generation) {
    const QString path = QDir(game_dir_).filePath(QString("%1-%2.mp4").arg(prefix).arg(generation));
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
      return path;
  }
  return {};
}

void HighlightsDialog::publishConcat() {
  const QFileInfo partial(final_partial_path_);
  if (!partial.isFile() || partial.size() <= 0) {
    finishJob(false, "FFmpeg reported success but produced no joined video.");
    return;
  }
  QString destination = finalOutputPath(current_route_);
  if (destination.isEmpty()) {
    finishJob(false, "No unused highlights filename is available.");
    return;
  }
#ifdef Q_OS_UNIX
  const QByteArray source = QFile::encodeName(final_partial_path_);
  const int fd = ::open(source.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 || ::fsync(fd) != 0) {
    if (fd >= 0)
      ::close(fd);
    finishJob(false, "Could not make the joined video durable before publication.");
    return;
  }
  ::close(fd);
  int published = -1;
  for (int attempt = 0; attempt < 10000; ++attempt) {
    destination = finalOutputPath(current_route_);
    if (destination.isEmpty())
      break;
    published = ::link(source.constData(), QFile::encodeName(destination).constData());
    if (published == 0)
      break;
    if (errno != EEXIST)
      break;
  }
  if (published != 0) {
    finishJob(
        false, QString("Could not publish highlights video: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  const int directory_fd = ::open(QFile::encodeName(game_dir_).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  const bool durable = directory_fd >= 0 && ::fsync(directory_fd) == 0;
  if (directory_fd >= 0)
    ::close(directory_fd);
  if (!durable) {
    finishJob(false, "Video was published at " + destination + ", but its directory could not be synchronized.");
    return;
  }
#else
  if (!QFile::copy(final_partial_path_, destination)) {
    finishJob(false, "Could not publish highlights video: " + destination);
    return;
  }
#endif
  published_paths_ << destination;
  appendLog("Saved " + destination);
  ++concat_route_index_;
  startConcat();
}

void HighlightsDialog::finishJob(bool success, const QString& message) {
  const Job completed = job_;
  job_ = Job::kNone;
  stage_ = Stage::kIdle;
  cancelling_ = false;
  loop_ = false;
  QString detail = message;
  if (!success && completed == Job::kExport && !work_dir_.isEmpty()) {
    detail += " Work files retained in " + work_dir_;
    if (!publication_work_dir_.isEmpty())
      detail += "; publication files retained in " + publication_work_dir_;
    if (!published_paths_.isEmpty())
      detail += "; completed outputs: " + published_paths_.join(", ");
  }
  status_->setText(detail);
  appendLog(message);
  if (success && completed == Job::kExport && !work_dir_.isEmpty()) {
    QDir(work_dir_).removeRecursively();
    QDir(publication_work_dir_).removeRecursively();
  }
  work_dir_.clear();
  publication_work_dir_.clear();
  updateControls();
  if (close_when_stopped_) {
    close_when_stopped_ = false;
    QTimer::singleShot(0, this, [this] { close(); });
  }
}

void HighlightsDialog::stop() {
  if (!isBusy())
    return;
  cancelling_ = true;
  loop_ = false;
  status_->setText("Stopping highlights job…");
  if (process_.state() == QProcess::NotRunning) {
    finishJob(false, "Highlights job stopped.");
    return;
  }
  process_.terminate();
  QTimer::singleShot(4000, this, [this] {
    if (cancelling_ && process_.state() != QProcess::NotRunning)
      process_.kill();
  });
}

} // namespace hm::ui
