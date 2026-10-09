#include "src/apps/hstream-ui/HighlightsDialog.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/AnsiLogFormat.h"
#include "src/apps/hstream-ui/HighlightItemEditor.h"
#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QTextDocument>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace hm::ui {

class HighlightsVideoTarget : public QWidget {
 public:
  explicit HighlightsVideoTarget(QWidget* parent) : QWidget(parent) {
    if (QGuiApplication::platformName() == "xcb")
      setAttribute(Qt::WA_NativeWindow);
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPalette background = palette();
    background.setColor(QPalette::Window, Qt::black);
    setPalette(background);
    setAutoFillBackground(true);
  }

  void setRendererActive(bool active) {
    const bool direct = active && QGuiApplication::platformName() == "xcb";
    setAttribute(Qt::WA_PaintOnScreen, direct);
    setAttribute(Qt::WA_NoSystemBackground, direct);
    setAutoFillBackground(!active);
    update();
  }

  QPaintEngine* paintEngine() const override {
    return testAttribute(Qt::WA_PaintOnScreen) ? nullptr : QWidget::paintEngine();
  }

  std::function<void()> toggleFocus;

 protected:
  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && toggleFocus) {
      toggleFocus();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }
};

namespace {

QString safeFileComponent(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
  value.remove(QRegularExpression("^_+|_+$"));
  return value.left(64);
}

QString archiveChoiceLabel(const ArchiveEntry& entry) {
  QStringList parts{ArchiveKindDisplayName(entry.kind)};
  if (entry.generation > 0)
    parts << QString("gen %1").arg(entry.generation);
  if (entry.width > 0 && entry.height > 0)
    parts << QString("%1×%2").arg(entry.width).arg(entry.height);
  if (entry.duration_ms > 0)
    parts << FormatHighlightTime(entry.duration_ms);
  parts << (entry.start_time_known ? "starts " + FormatHighlightTime(entry.start_time_ms) : "start unknown");
  return parts.join(" · ");
}

} // namespace

HighlightsDialog::HighlightsDialog(
    QString game_id,
    QString game_dir,
    QProcessEnvironment env,
    QVector<ArchiveEntry> archives,
    QWidget* parent)
    : QDialog(parent, Qt::Window),
      game_id_(std::move(game_id)),
      game_dir_(std::move(game_dir)),
      env_(std::move(env)),
      archives_(std::move(archives)),
      plan_path_(QDir(game_dir_).filePath("highlights.json")) {
  setObjectName("highlightsDialog");
  setWindowTitle("Highlights — " + game_id_);
  configure_preview_dialog_window(this);
  resize(1220, 700);
  auto* root = new QVBoxLayout(this);
  auto* intro = new QLabel(
      "Highlights are cut from a published archive of this game. Add event times or exact ranges in game time. "
      "Enter seconds, MM:SS, or HH:MM:SS (optional .mmm). Event clips use 75% of the duration before the event.");
  intro->setWordWrap(true);
  auto* heading = new QHBoxLayout();
  heading->addWidget(intro, 1);
  auto* maximize = new PreviewDialogWindowSizeButton(this);
  maximize->setObjectName("maximizeHighlightsWindowButton");
  heading->addWidget(maximize, 0, Qt::AlignTop);
  root->addLayout(heading);

  auto* source_row = new QHBoxLayout();
  archive_label_ = new QLabel("Source", this);
  source_row->addWidget(archive_label_);
  archive_combo_ = new QComboBox(this);
  archive_combo_->setObjectName("highlightArchiveCombo");
  archive_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  source_row->addWidget(archive_combo_, 1);
  source_row->addWidget(new QLabel("Archive starts at", this));
  archive_offset_edit_ = new QLineEdit(this);
  archive_offset_edit_->setObjectName("highlightArchiveOffsetEdit");
  archive_offset_edit_->setMaximumWidth(130);
  archive_offset_edit_->setPlaceholderText("00:00:00");
  source_row->addWidget(archive_offset_edit_);
  root->addLayout(source_row);
  archive_detail_ = new QLabel(this);
  archive_detail_->setObjectName("highlightArchiveDetail");
  archive_detail_->setWordWrap(true);
  root->addWidget(archive_detail_);
  connect(archive_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (isBusy() || index < 0 || index == archive_index_)
      return;
    const int previous_index = archive_index_;
    const QString previous_path = plan_.archive_path;
    archive_index_ = index;
    plan_.archive_path = archives_[index].path;
    if (!savePlan()) {
      archive_index_ = previous_index;
      plan_.archive_path = previous_path;
      const QSignalBlocker blocker(archive_combo_);
      archive_combo_->setCurrentIndex(previous_index);
      return;
    }
    applyArchiveSelection();
  });
  connect(archive_offset_edit_, &QLineEdit::editingFinished, this, &HighlightsDialog::commitArchiveOffset);

  preview_splitter_ = new QSplitter(Qt::Horizontal, this);
  preview_splitter_->setObjectName("highlightPreviewSplitter");
  preview_splitter_->setChildrenCollapsible(false);
  table_ = new QTableWidget(0, 6, preview_splitter_);
  table_->setObjectName("highlightsTable");
  table_->setHorizontalHeaderLabels({"Name", "Input", "Start", "End", "Length", "In archive"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column < 6; ++column)
    table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  preview_splitter_->addWidget(table_);
  auto* preview_panel = new QWidget(preview_splitter_);
  preview_panel->setObjectName("highlightPreviewPanel");
  auto* preview_layout = new QVBoxLayout(preview_panel);
  auto* preview_header = new QHBoxLayout();
  preview_header->addWidget(new QLabel("Archive preview", preview_panel));
  preview_header->addStretch();
  stop_button_ = new QPushButton(action_icon(ActionIcon::Stop), "Stop", preview_panel);
  stop_button_->setObjectName("highlightStopButton");
  preview_header->addWidget(stop_button_);
  expand_preview_button_ = new PreviewFocusButton(preview_panel);
  expand_preview_button_->setObjectName("highlightExpandPreviewButton");
  preview_header->addWidget(expand_preview_button_);
  preview_layout->addLayout(preview_header);
  video_ = new HighlightsVideoTarget(preview_panel);
  video_->setObjectName("highlightVideo");
  preview_layout->addWidget(video_, 1);
  preview_splitter_->addWidget(preview_panel);
  preview_splitter_->setStretchFactor(0, 3);
  preview_splitter_->setStretchFactor(1, 2);
  root->addWidget(preview_splitter_, 1);
  video_->toggleFocus = [this] { setPreviewFocused(!preview_focused_); };
  connect(expand_preview_button_, &QToolButton::clicked, this, video_->toggleFocus);
  connect(stop_button_, &QPushButton::clicked, this, &HighlightsDialog::stop);
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
  first_edit_->setPlaceholderText("e.g. 32 or 14:12");
  editor->addRow("Event / start", first_edit_);
  second_edit_ = new QLineEdit(this);
  second_edit_->setObjectName("highlightSecondTimeEdit");
  second_edit_->setPlaceholderText("e.g. 32 or 14:12");
  editor->addRow("Duration / end", second_edit_);
  root->addLayout(editor);
  connect(mode_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    first_edit_->setPlaceholderText(mode_combo_->currentData().toBool() ? "Event: 32 or 14:12" : "Start: 32 or 14:12");
    second_edit_->setPlaceholderText(
        mode_combo_->currentData().toBool() ? "Duration: 32 or 14:12" : "End: 32 or 14:12");
  });

  auto* edit_actions = new QHBoxLayout();
  add_button_ = new QPushButton("Add", this);
  add_button_->setIcon(action_icon(ActionIcon::Add));
  add_button_->setObjectName("highlightAddButton");
  update_button_ = new QPushButton("Apply changes", this);
  update_button_->setIcon(action_icon(ActionIcon::Apply));
  update_button_->setObjectName("highlightUpdateButton");
  remove_button_ = new QPushButton("Remove", this);
  remove_button_->setIcon(action_icon(ActionIcon::Remove));
  remove_button_->setObjectName("highlightRemoveButton");
  up_button_ = new QPushButton("Move up", this);
  up_button_->setIcon(action_icon(ActionIcon::Up));
  up_button_->setObjectName("highlightUpButton");
  down_button_ = new QPushButton("Move down", this);
  down_button_->setIcon(action_icon(ActionIcon::Down));
  down_button_->setObjectName("highlightDownButton");
  for (auto* button : {add_button_, update_button_, remove_button_, up_button_, down_button_})
    edit_actions->addWidget(button);
  card_button_ = new QPushButton(action_icon(ActionIcon::Add), "Add card", this);
  card_button_->setObjectName("highlightAddCardButton");
  edit_item_button_ = new QPushButton(action_icon(ActionIcon::Apply), "Annotations / card", this);
  edit_item_button_->setObjectName("highlightEditItemButton");
  duplicate_button_ = new QPushButton(action_icon(ActionIcon::Add), "Duplicate", this);
  duplicate_button_->setObjectName("highlightDuplicateButton");
  edit_actions->addWidget(card_button_);
  edit_actions->addWidget(edit_item_button_);
  edit_actions->addWidget(duplicate_button_);
  connect(card_button_, &QPushButton::clicked, this, &HighlightsDialog::addCard);
  connect(edit_item_button_, &QPushButton::clicked, this, &HighlightsDialog::editItem);
  connect(duplicate_button_, &QPushButton::clicked, this, &HighlightsDialog::duplicateItem);
  edit_actions->addStretch();
  root->addLayout(edit_actions);
  connect(add_button_, &QPushButton::clicked, this, &HighlightsDialog::addInterval);
  connect(update_button_, &QPushButton::clicked, this, &HighlightsDialog::updateInterval);
  connect(remove_button_, &QPushButton::clicked, this, &HighlightsDialog::removeInterval);
  connect(up_button_, &QPushButton::clicked, this, [this] { moveInterval(-1); });
  connect(down_button_, &QPushButton::clicked, this, [this] { moveInterval(1); });

  auto* playback = new QHBoxLayout();
  preview_selected_button_ = new QPushButton("Preview selected", this);
  preview_selected_button_->setIcon(action_icon(ActionIcon::Play));
  preview_selected_button_->setObjectName("highlightPreviewSelectedButton");
  preview_all_button_ = new QPushButton("Preview all", this);
  preview_all_button_->setIcon(action_icon(ActionIcon::Play));
  preview_all_button_->setObjectName("highlightPreviewAllButton");
  loop_selected_button_ = new QPushButton("Loop selected", this);
  loop_selected_button_->setIcon(action_icon(ActionIcon::Refresh));
  loop_selected_button_->setObjectName("highlightLoopSelectedButton");
  loop_button_ = new QPushButton("Loop all", this);
  loop_button_->setIcon(action_icon(ActionIcon::Refresh));
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
  export_selected_button_ = new QPushButton("Export selected", this);
  export_selected_button_->setIcon(action_icon(ActionIcon::Save));
  export_selected_button_->setObjectName("highlightExportSelectedButton");
  export_all_button_ = new QPushButton("Export all", this);
  export_all_button_->setIcon(action_icon(ActionIcon::Save));
  export_all_button_->setObjectName("highlightExportAllButton");
  export_row->addWidget(export_selected_button_);
  export_row->addWidget(export_all_button_);
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
  connect(export_selected_button_, &QPushButton::clicked, this, [this] { beginExport(true); });
  connect(export_all_button_, &QPushButton::clicked, this, [this] { beginExport(false); });

  status_ = new QLabel(this);
  status_->setObjectName("highlightStatus");
  status_->setWordWrap(true);
  root->addWidget(status_);
  log_ = new QTextEdit(this);
  log_->setObjectName("highlightLog");
  log_->setReadOnly(true);
  log_->setAcceptRichText(true);
  log_->document()->setMaximumBlockCount(400);
  log_->setMaximumHeight(145);
  root->addWidget(log_);
  auto* log_scroll = log_->verticalScrollBar();
  connect(log_scroll, &QScrollBar::valueChanged, this, [this](int value) {
    if (log_scroll_is_programmatic_)
      return;
    auto* bar = log_->verticalScrollBar();
    log_follows_tail_ = value >= bar->maximum() - std::max(2, bar->singleStep());
  });
  connect(log_scroll, &QScrollBar::rangeChanged, this, [this] {
    if (!log_follows_tail_)
      return;
    const bool previous = log_scroll_is_programmatic_;
    log_scroll_is_programmatic_ = true;
    auto* bar = log_->verticalScrollBar();
    bar->setValue(bar->maximum());
    log_scroll_is_programmatic_ = previous;
  });

  process_.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this, &HighlightsDialog::readProcessOutput);
  connect(&process_, &QProcess::readyReadStandardError, this, &HighlightsDialog::readProcessOutput);
  connect(&process_, &QProcess::started, this, [this] {
    if (cancelling_)
      requestActiveProcessStop();
  });
  connect(
      &process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &HighlightsDialog::processFinished);
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart)
      finishJob(false, "Could not start " + process_.program() + ": " + process_.errorString());
  });

  preview_timer_ = new QTimer(this);
  preview_timer_->setInterval(100);
  connect(preview_timer_, &QTimer::timeout, this, &HighlightsDialog::pollPreview);
  // Probing the renderer costs a GStreamer registry scan, so only ask on a
  // platform that could host it at all.
  preview_supported_ = embeddedPreviewAvailable() && HighlightReelPipeline::PreviewAvailable();

  QString load_error;
  if (!LoadHighlightPlan(plan_path_, &plan_, &load_error)) {
    plan_load_error_ = load_error;
    status_->setText("Could not load highlights: " + load_error + ". Fix the file and reopen Highlights.");
  } else {
    status_->setText("Ready. Intervals are saved with this game.");
  }
  base_name_edit_->setText(plan_.base_name);
  int saved_index = -1;
  for (int i = 0; i < archives_.size(); ++i)
    if (archives_[i].path == plan_.archive_path)
      saved_index = i;
  if (!plan_.archive_path.isEmpty() && saved_index < 0) {
    ArchiveEntry missing;
    missing.path = plan_.archive_path;
    missing.game_id = game_id_;
    missing.kind = "program";
    archives_.append(missing);
    saved_index = archives_.size() - 1;
  }
  refreshArchiveChoices();
  if (!archives_.isEmpty()) {
    archive_index_ = saved_index >= 0 ? saved_index : 0;
    plan_.archive_path = archives_[archive_index_].path;
    {
      const QSignalBlocker blocker(archive_combo_);
      archive_combo_->setCurrentIndex(archive_index_);
    }
    applyArchiveSelection();
  } else {
    archive_detail_->setText(
        "No published archive was found for this game. Publish a run before previewing or "
        "exporting highlights.");
  }
  refreshTable();
  updateControls();
}

HighlightsDialog::~HighlightsDialog() {
  // The renderer holds this dialog's native window, so it has to let go before
  // the widget tree is destroyed.
  player_.reset();
  if (process_.state() != QProcess::NotRunning) {
    cancelling_ = true;
    loop_ = false;
    close_when_stopped_ = false;
    process_.terminate();
    if (!process_.waitForFinished(4000)) {
      process_.kill();
      process_.waitForFinished(1000);
    }
  }
  // finishJob() normally clears this; take the abandoned parts with us if the
  // process never reported in.
  if (!work_dir_.isEmpty())
    QDir(work_dir_).removeRecursively();
}

bool HighlightsDialog::isBusy() const {
  return job_ != Job::kNone;
}

void HighlightsDialog::closeEvent(QCloseEvent* event) {
  if (isBusy()) {
    event->ignore();
    done(QDialog::Rejected);
    return;
  }
  QDialog::closeEvent(event);
}

void HighlightsDialog::done(int result) {
  if (isBusy()) {
    close_when_stopped_ = true;
    close_result_ = result;
    stop();
    return;
  }
  player_.reset();
  QDialog::done(result);
}

void HighlightsDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && preview_focused_) {
    setPreviewFocused(false);
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
}

void HighlightsDialog::setPreviewFocused(bool focused) {
  if (focused == preview_focused_)
    return;
  const bool remap_video = video_->isVisible();
  if (remap_video)
    video_->hide();
  if (focused) {
    preview_splitter_sizes_ = preview_splitter_->sizes();
    preview_focus_hidden_.clear();
    for (QWidget* widget : findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
      if (widget != preview_splitter_ && widget->isVisible()) {
        preview_focus_hidden_.push_back(widget);
        widget->hide();
      }
    }
    if (table_->isVisible()) {
      preview_focus_hidden_.push_back(table_);
      table_->hide();
    }
  } else {
    for (QWidget* widget : preview_focus_hidden_)
      widget->show();
    preview_focus_hidden_.clear();
  }
  preview_focused_ = focused;
  expand_preview_button_->setFocused(focused);
  layout()->activate();
  if (!focused)
    preview_splitter_->setSizes(preview_splitter_sizes_);
  QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  if (remap_video) {
    video_->show();
    video_->raise();
  }
  updateControls();
}

void HighlightsDialog::appendLog(const QString& line) {
  if (!line.trimmed().isEmpty()) {
    const bool previous = log_scroll_is_programmatic_;
    log_scroll_is_programmatic_ = true;
    const bool dark_background = log_->palette().color(QPalette::Base).lightness() < 128;
    QString normalized = line;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');
    const QStringList lines = normalized.split('\n');
    for (int index = 0; index < lines.size(); ++index) {
      if (index + 1 == lines.size() && lines[index].isEmpty())
        break;
      log_->append("<span style=\"white-space:pre-wrap\">" + ansi_to_html(lines[index], dark_background) + "</span>");
    }
    log_scroll_is_programmatic_ = previous;
    if (log_follows_tail_) {
      log_scroll_is_programmatic_ = true;
      auto* bar = log_->verticalScrollBar();
      bar->setValue(bar->maximum());
      log_scroll_is_programmatic_ = previous;
    }
  }
}

bool HighlightsDialog::embeddedPreviewAvailable() const {
#if defined(__x86_64__) && !defined(IS_TEGRA)
  return QGuiApplication::platformName().compare("xcb", Qt::CaseInsensitive) == 0;
#else
  return false;
#endif
}

const ArchiveEntry* HighlightsDialog::selectedArchive() const {
  if (archive_index_ < 0 || archive_index_ >= archives_.size())
    return nullptr;
  return &archives_[archive_index_];
}

qint64 HighlightsDialog::archiveDurationMs() const {
  const ArchiveEntry* archive = selectedArchive();
  if (!archive)
    return -1;
  if (media_valid_ && media_path_ == archive->path && media_.duration_seconds > 0.0)
    return static_cast<qint64>(std::llround(media_.duration_seconds * 1000.0));
  return archive->duration_ms;
}

void HighlightsDialog::refreshArchiveChoices() {
  const QSignalBlocker blocker(archive_combo_);
  archive_combo_->clear();
  for (const ArchiveEntry& entry : archives_)
    archive_combo_->addItem(archiveChoiceLabel(entry), entry.path);
  if (archive_index_ >= 0 && archive_index_ < archives_.size())
    archive_combo_->setCurrentIndex(archive_index_);
  // With one archive there is nothing to choose between, and the detail line
  // below already names the file.
  const bool choosable = archives_.size() > 1;
  archive_combo_->setVisible(choosable);
  archive_label_->setVisible(choosable);
}

void HighlightsDialog::applyArchiveSelection() {
  const ArchiveEntry* archive = selectedArchive();
  if (!archive)
    return;
  archive_offset_ms_ = archive->start_time_ms;
  {
    const QSignalBlocker blocker(archive_offset_edit_);
    archive_offset_edit_->setText(FormatHighlightTime(archive_offset_ms_));
  }
  media_valid_ = false;
  media_ = ArchiveMediaInfo();
  media_path_.clear();
  media_error_.clear();
  player_.reset();
  refreshTable();
  startInspection();
}

void HighlightsDialog::commitArchiveOffset() {
  if (isBusy())
    return;
  const ArchiveEntry* archive = selectedArchive();
  if (!archive)
    return;
  qint64 offset = 0;
  QString error;
  if (!ParseHighlightTime(archive_offset_edit_->text(), &offset, &error)) {
    status_->setText("Invalid archive start time: " + error);
    const QSignalBlocker blocker(archive_offset_edit_);
    archive_offset_edit_->setText(FormatHighlightTime(archive_offset_ms_));
    return;
  }
  if (offset == archive_offset_ms_)
    return;
  archive_offset_ms_ = offset;
  archives_[archive_index_].start_time_ms = offset;
  archives_[archive_index_].start_time_known = true;
  {
    const QSignalBlocker blocker(archive_offset_edit_);
    archive_offset_edit_->setText(FormatHighlightTime(archive_offset_ms_));
  }
  // Record the correction beside the archive. An archive published before this
  // feature existed has no origin of its own, and retyping it on every visit is
  // the kind of chore that gets it wrong eventually.
  QString sidecar_error;
  if (!SaveArchiveSidecar(archives_[archive_index_], &sidecar_error))
    appendLog("Could not remember this archive's start time: " + sidecar_error);
  refreshArchiveChoices();
  refreshTable();
  status_->setText(
      "Highlight times are now read against an archive starting at " + FormatHighlightTime(archive_offset_ms_) + ".");
}

QString HighlightsDialog::resolveClip(const HighlightInterval& interval, Clip* clip) const {
  if (interval.is_card) {
    if (clip) {
      clip->interval = interval;
      clip->archive_start_ms = 0;
      clip->archive_end_ms = interval.card.duration_ms;
    }
    return {};
  }
  const qint64 start = interval.start_ms - archive_offset_ms_;
  const qint64 end = interval.end_ms - archive_offset_ms_;
  if (end <= start)
    return "has no length";
  if (start < 0)
    return QString("starts %1 before this archive begins").arg(FormatHighlightTime(-start));
  const qint64 duration = archiveDurationMs();
  if (duration > 0 && end > duration) {
    return QString("ends after this archive, which stops at %1")
        .arg(FormatHighlightTime(archive_offset_ms_ + duration));
  }
  if (clip) {
    clip->interval = interval;
    clip->archive_start_ms = start;
    clip->archive_end_ms = end;
  }
  return {};
}

bool HighlightsDialog::buildQueue(bool selected, QVector<Clip>* clips, QString* error) const {
  clips->clear();
  QVector<HighlightInterval> wanted;
  if (selected) {
    const int row = table_->currentRow();
    if (row < 0 || row >= plan_.intervals.size()) {
      *error = "Select an interval first.";
      return false;
    }
    wanted << plan_.intervals[row];
  } else {
    wanted = plan_.intervals;
  }
  if (wanted.isEmpty()) {
    *error = "Add an interval first.";
    return false;
  }
  for (const HighlightInterval& interval : wanted) {
    if (!interval.is_card && (!selectedArchive() || !media_valid_)) {
      *error = media_error_.isEmpty() ? "Select a readable published archive for video clips." : media_error_;
      return false;
    }
    Clip clip;
    const QString problem = resolveClip(interval, &clip);
    if (!problem.isEmpty()) {
      *error = QString("\"%1\" %2.").arg(interval.label.isEmpty() ? QString("Interval") : interval.label, problem);
      return false;
    }
    clips->push_back(clip);
  }
  return true;
}

void HighlightsDialog::refreshTable() {
  const int selected = table_->currentRow();
  table_->setRowCount(plan_.intervals.size());
  for (int row = 0; row < plan_.intervals.size(); ++row) {
    const auto& interval = plan_.intervals[row];
    Clip clip;
    const QString problem =
        (interval.is_card || selectedArchive()) ? resolveClip(interval, &clip) : QString("has no archive to cut from");
    const QStringList cells = {
        interval.label,
        interval.is_card          ? (interval.card.matchup ? "Matchup card" : "Text card")
            : interval.event_mode ? "Event " + FormatHighlightTime(interval.event_ms)
                                  : "Range",
        interval.is_card ? QString("—") : FormatHighlightTime(interval.start_ms),
        interval.is_card ? QString("—") : FormatHighlightTime(interval.end_ms),
        FormatHighlightTime(HighlightItemDuration(interval)),
        interval.is_card ? QString("Generated")
            : problem.isEmpty()
            ? FormatHighlightTime(clip.archive_start_ms) + "–" + FormatHighlightTime(clip.archive_end_ms)
            : QString("—")};
    for (int column = 0; column < cells.size(); ++column) {
      auto* item = new QTableWidgetItem(cells[column]);
      if (!problem.isEmpty()) {
        item->setForeground(QBrush(QColor(0xc0, 0x39, 0x2b)));
        item->setToolTip("This interval " + problem + ".");
      }
      table_->setItem(row, column, item);
    }
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
  if (!plan_load_error_.isEmpty()) {
    status_->setText("Could not load highlights: " + plan_load_error_ + ". Fix the file and reopen Highlights.");
    return false;
  }
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
  const bool editable = idle && plan_load_error_.isEmpty();
  const bool selected = table_->currentRow() >= 0 && table_->currentRow() < plan_.intervals.size();
  const bool cuttable = editable;
  for (auto* widget :
       {static_cast<QWidget*>(table_),
        static_cast<QWidget*>(archive_combo_),
        static_cast<QWidget*>(archive_offset_edit_),
        static_cast<QWidget*>(label_edit_),
        static_cast<QWidget*>(mode_combo_),
        static_cast<QWidget*>(first_edit_),
        static_cast<QWidget*>(second_edit_),
        static_cast<QWidget*>(base_name_edit_)})
    widget->setEnabled(editable);
  archive_offset_edit_->setEnabled(editable && selectedArchive() != nullptr);
  add_button_->setEnabled(editable);
  card_button_->setEnabled(editable);
  edit_item_button_->setEnabled(editable && selected);
  duplicate_button_->setEnabled(editable && selected);
  update_button_->setEnabled(editable && selected);
  remove_button_->setEnabled(editable && selected);
  up_button_->setEnabled(editable && selected && table_->currentRow() > 0);
  down_button_->setEnabled(editable && selected && table_->currentRow() + 1 < plan_.intervals.size());
  const bool can_preview = cuttable && preview_supported_;
  preview_selected_button_->setEnabled(can_preview && selected);
  preview_all_button_->setEnabled(can_preview && !plan_.intervals.isEmpty());
  loop_selected_button_->setEnabled(can_preview && selected);
  loop_button_->setEnabled(can_preview && !plan_.intervals.isEmpty());
  export_selected_button_->setEnabled(cuttable && selected);
  export_all_button_->setEnabled(cuttable && !plan_.intervals.isEmpty());
  stop_button_->setEnabled((!idle && job_ != Job::kInspect) || preview_focused_);
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
  if (plan_.intervals[row].is_card) {
    editItem();
    return;
  }
  HighlightInterval interval = plan_.intervals[row];
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

void HighlightsDialog::startInspection() {
  const ArchiveEntry* archive = selectedArchive();
  if (!archive || isBusy())
    return;
  job_ = Job::kInspect;
  stage_ = Stage::kProbe;
  ++job_generation_;
  cancelling_ = false;
  probe_output_.clear();
  process_error_buffer_.clear();
  media_path_ = archive->path;
  archive_detail_->setText("Reading " + archive->path + "…");
  updateControls();
  process_.setWorkingDirectory(game_dir_);
  process_.setProcessEnvironment(env_);
  process_.start(
      env_.value("HSTREAM_UI_FFPROBE", "ffprobe"),
      {"-v", "error", "-show_streams", "-show_format", "-of", "json", archive->path});
}

void HighlightsDialog::beginPreview(bool selected, bool loop) {
  if (isBusy() || !plan_load_error_.isEmpty())
    return;
  QVector<Clip> clips;
  QString error;
  if (!buildQueue(selected, &clips, &error)) {
    status_->setText(error);
    return;
  }
  HighlightReelPipeline::Request request;
  if (const auto* archive = selectedArchive())
    request.archive_path = archive->path;
  request.asset_root = game_dir_;
  request.archive_offset_ms = archive_offset_ms_;
  request.media = media_;
  if (request.media.width <= 0) {
    request.media.width = 1920;
    request.media.height = 1080;
    request.media.frame_rate = 30;
  }
  request.window_id = video_->winId();
  request.loop = loop;
  for (const auto& clip : clips)
    request.items.append(clip.interval);
  player_ = std::make_unique<HighlightReelPipeline>();
  if (!player_->Start(request, &error)) {
    player_.reset();
    status_->setText(error);
    return;
  }
  video_->setRendererActive(true);
  queue_ = clips;
  job_ = Job::kPreview;
  loop_ = loop;
  cancelling_ = false;
  log_->clear();
  appendLog("Native GPU reel preview");
  status_->setText("Starting preview…");
  preview_timer_->start();
  updateControls();
}

void HighlightsDialog::pollPreview() {
  if ((job_ != Job::kPreview && job_ != Job::kExport) || !player_)
    return;
  const auto status = player_->Poll();
  if (status.finished) {
    player_.reset();
    preview_timer_->stop();
    if (cancelling_) {
      finishJob(false, "Highlights job stopped.");
      return;
    }
    if (!status.error.isEmpty()) {
      finishJob(false, status.error);
      return;
    }
    if (job_ == Job::kExport)
      publishEncode();
    else
      finishJob(true, "Preview complete.");
    return;
  }
  if (job_ == Job::kExport) {
    status_->setText(
        QString("Encoding reel… %1%")
            .arg(status.total_ms > 0 ? std::clamp<qint64>(status.position_ms * 100 / status.total_ms, 0, 100) : 0));
  } else
    status_->setText(
        QString("%1 reel · %2").arg(loop_ ? "Looping" : "Previewing", FormatHighlightTime(status.position_ms)));
}

void HighlightsDialog::stopPreview() {
  if (player_)
    player_->Cancel();
}

void HighlightsDialog::editItem() {
  const int row = table_->currentRow();
  if (isBusy() || row < 0 || row >= plan_.intervals.size())
    return;
  auto item = plan_.intervals[row];
  const auto* archive = selectedArchive();
  if (!item.is_card && !media_valid_) {
    status_->setText("Select a readable archive before editing annotations");
    return;
  }
  if (!EditHighlightItem(
          &item,
          game_dir_,
          game_id_,
          archive ? archive->path : QString(),
          archive ? archive->kind : QString(),
          archive_offset_ms_,
          media_,
          this))
    return;
  const auto previous = plan_.intervals[row];
  plan_.intervals[row] = item;
  if (!savePlan()) {
    plan_.intervals[row] = previous;
    return;
  }
  refreshTable();
}

void HighlightsDialog::addCard() {
  if (isBusy())
    return;
  HighlightInterval item;
  item.is_card = true;
  item.label = "Title card";
  const auto* archive = selectedArchive();
  if (!EditHighlightItem(
          &item,
          game_dir_,
          game_id_,
          archive ? archive->path : QString(),
          archive ? archive->kind : QString(),
          archive_offset_ms_,
          media_,
          this))
    return;
  item.label = item.card.heading.isEmpty() ? "Matchup" : item.card.heading;
  const int row = table_->currentRow() < 0 ? plan_.intervals.size() : table_->currentRow() + 1;
  plan_.intervals.insert(row, item);
  if (!savePlan()) {
    plan_.intervals.removeAt(row);
    return;
  }
  refreshTable();
  table_->setCurrentCell(row, 0);
}

void HighlightsDialog::duplicateItem() {
  const int row = table_->currentRow();
  if (isBusy() || row < 0 || row >= plan_.intervals.size())
    return;
  const auto copy = plan_.intervals[row];
  plan_.intervals.insert(row + 1, copy);
  if (!savePlan()) {
    plan_.intervals.removeAt(row + 1);
    return;
  }
  refreshTable();
  table_->setCurrentCell(row + 1, 0);
}

void HighlightsDialog::beginExport(bool selected) {
  if (isBusy() || !plan_load_error_.isEmpty())
    return;
  frozen_plan_ = plan_;
  frozen_plan_.base_name = base_name_edit_->text().trimmed();
  if (safeFileComponent(frozen_plan_.base_name).isEmpty()) {
    status_->setText("Enter a base name for the video.");
    return;
  }
  QVector<Clip> clips;
  QString error;
  if (!buildQueue(selected, &clips, &error)) {
    status_->setText(error);
    return;
  }
  const QString previous_base_name = plan_.base_name;
  plan_.base_name = frozen_plan_.base_name;
  if (!savePlan()) {
    plan_.base_name = previous_base_name;
    return;
  }
  // The published video is hard-linked out of this directory, so it has to sit
  // on the same filesystem as the game directory.
  QTemporaryDir work(QDir(game_dir_).filePath(".highlights-export-XXXXXX"));
  if (!work.isValid()) {
    status_->setText("Could not create a highlights work directory in " + game_dir_);
    return;
  }
  work.setAutoRemove(false);
  work_dir_ = work.path();
  queue_ = clips;
  current_route_ = selectedArchive() ? selectedArchive()->kind : "cards";
  published_path_.clear();
  encode_total_ms_ = 0;
  for (const Clip& clip : queue_)
    encode_total_ms_ += clip.archive_end_ms - clip.archive_start_ms;
  encode_percent_ = -1;
  job_ = Job::kExport;
  stage_ = Stage::kIdle;
  ++job_generation_;
  loop_ = false;
  cancelling_ = false;
  process_error_buffer_.clear();
  log_->clear();
  status_->setText("Preparing to encode highlights…");
  updateControls();
  final_partial_path_ = QDir(work_dir_).filePath("reel.mp4");
  HighlightReelPipeline::Request request;
  if (const auto* archive = selectedArchive())
    request.archive_path = archive->path;
  request.asset_root = game_dir_;
  request.output_path = final_partial_path_;
  request.archive_offset_ms = archive_offset_ms_;
  request.media = media_;
  if (request.media.width <= 0) {
    request.media.width = 1920;
    request.media.height = 1080;
    request.media.frame_rate = 30;
  }
  for (const auto& clip : queue_)
    request.items.append(clip.interval);
  player_ = std::make_unique<HighlightReelPipeline>();
  if (!player_->Start(request, &error)) {
    player_.reset();
    finishJob(false, error);
    return;
  }
  stage_ = Stage::kEncode;
  preview_timer_->start();
  appendLog("Native GPU decode/render/encode → MP4");
}

void HighlightsDialog::readProcessOutput() {
  probe_output_ += QString::fromLocal8Bit(process_.readAllStandardOutput());
  appendProcessError(QString::fromLocal8Bit(process_.readAllStandardError()));
}

void HighlightsDialog::appendProcessError(const QString& output, bool flush) {
  process_error_buffer_ += output;
  while (true) {
    const qsizetype newline = process_error_buffer_.indexOf('\n');
    const qsizetype carriage_return = process_error_buffer_.indexOf('\r');
    const qsizetype boundary = newline < 0 ? carriage_return
        : carriage_return < 0              ? newline
                                           : std::min(newline, carriage_return);
    if (boundary < 0)
      break;
    appendLog(process_error_buffer_.left(boundary));
    process_error_buffer_.remove(0, boundary + 1);
  }
  if (flush && !process_error_buffer_.isEmpty()) {
    appendLog(process_error_buffer_);
    process_error_buffer_.clear();
  }
}

void HighlightsDialog::processFinished(int code, QProcess::ExitStatus status) {
  if (job_ == Job::kNone)
    return;
  readProcessOutput();
  appendProcessError({}, true);
  const Stage finished_stage = stage_;
  if (cancelling_) {
    finishJob(false, job_ == Job::kInspect ? "Reading the archive was stopped." : "Highlights job stopped.");
    return;
  }
  if (status != QProcess::NormalExit || code != 0) {
    if (finished_stage == Stage::kProbe) {
      media_error_ = "Could not read the archive with ffprobe. See the log below.";
      finishJob(false, media_error_);
      return;
    }
    finishJob(false, QString("Archive inspection failed with exit code %1. See the log below.").arg(code));
    return;
  }
  switch (finished_stage) {
    case Stage::kProbe: {
      QString error;
      ArchiveMediaInfo info;
      if (!ParseArchiveMediaInfo(probe_output_.toUtf8(), &info, &error)) {
        media_error_ = error;
        finishJob(false, error);
        return;
      }
      media_ = info;
      media_valid_ = true;
      media_error_.clear();
      if (archive_index_ >= 0 && archive_index_ < archives_.size()) {
        // Only what the probe actually measured. The entry goes back to the
        // sidecar when the start time is edited, and a container that would not
        // give up its duration should not erase the one publication recorded.
        ArchiveEntry& entry = archives_[archive_index_];
        if (media_.width > 0 && media_.height > 0) {
          entry.width = media_.width;
          entry.height = media_.height;
        }
        if (media_.duration_seconds > 0.0)
          entry.duration_ms = static_cast<qint64>(std::llround(media_.duration_seconds * 1000.0));
        refreshArchiveChoices();
      }
      const qint64 duration = archiveDurationMs();
      archive_detail_->setText(
          QString("%1 · %2 · %3%4 · covers %5 to %6%7")
              .arg(QFileInfo(media_path_).fileName())
              .arg(media_.video_codec.toUpper())
              .arg(media_.has_audio ? "with audio" : "no audio")
              .arg(media_.frame_rate > 0.0 ? QString(" · %1 fps").arg(media_.frame_rate, 0, 'f', 2) : QString())
              .arg(FormatHighlightTime(archive_offset_ms_))
              .arg(FormatHighlightTime(archive_offset_ms_ + std::max<qint64>(0, duration)))
              .arg(
                  selectedArchive() && selectedArchive()->start_time_known
                      ? QString()
                      : QString(
                            " (no recorded start time — set \"Archive starts at\" if highlight times are in "
                            "game time)")));
      finishJob(
          true,
          plan_load_error_.isEmpty()
              ? QString("Ready. Intervals are saved with this game.")
              : "Could not load highlights: " + plan_load_error_ + ". Fix the file and reopen Highlights.");
      return;
    }
    case Stage::kEncode:
      return;
    case Stage::kIdle:
      return;
  }
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

void HighlightsDialog::publishEncode() {
  const QFileInfo partial(final_partial_path_);
  if (!partial.isFile() || partial.size() <= 0) {
    finishJob(false, "Native reel pipeline produced no finalized video.");
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
  published_path_ = destination;
  appendLog("Saved " + destination);
  finishJob(true, "Highlights saved: " + destination);
}

void HighlightsDialog::finishJob(bool success, const QString& message) {
  const Job completed = job_;
  // Only a failure with something to look at is worth keeping. Stopping an
  // export is not a failure anyone needs to diagnose and its parts can run to
  // tens of gigabytes for an 8K reel, and a failure that happened before the
  // first part was written has nothing in the directory to show.
  const bool keep_work_files =
      completed == Job::kExport && !success && !cancelling_ && !work_dir_.isEmpty() && !QDir(work_dir_).isEmpty();
  job_ = Job::kNone;
  stage_ = Stage::kIdle;
  cancelling_ = false;
  loop_ = false;
  preview_timer_->stop();
  player_.reset();
  if (completed == Job::kPreview) {
    video_->setRendererActive(false);
  }
  QString detail = message;
  if (keep_work_files)
    detail += " Work files retained in " + work_dir_ + ".";
  status_->setText(detail);
  // A successful inspection has already written its own, longer description of
  // the archive into the detail label.
  if (completed != Job::kInspect || !success)
    appendLog(message);
  if (completed == Job::kInspect && !success)
    archive_detail_->setText(message);
  // QDir("") is the current working directory, so never hand it the empty path.
  if (completed == Job::kExport && !work_dir_.isEmpty()) {
    if (!keep_work_files)
      QDir(work_dir_).removeRecursively();
    work_dir_.clear();
  }
  updateControls();
  if (close_when_stopped_) {
    close_when_stopped_ = false;
    const int result = close_result_;
    QTimer::singleShot(0, this, [this, result] {
      if (!isBusy())
        done(result);
    });
  }
}

void HighlightsDialog::stop() {
  setPreviewFocused(false);
  if (!isBusy() || cancelling_)
    return;
  // Reading the archive is a quick ffprobe that preview and export are both
  // gated on, and the only thing that starts it again is picking an archive --
  // which is not even on screen when the game has exactly one. Leave it be;
  // closing the dialog still cancels it.
  if (job_ == Job::kInspect && !close_when_stopped_)
    return;
  cancelling_ = true;
  loop_ = false;
  if (job_ == Job::kPreview) {
    stopPreview();
    status_->setText("Stopping preview…");
    return;
  }
  status_->setText("Stopping highlights job…");
  if (player_) {
    player_->Cancel();
    return;
  }
  if (process_.state() == QProcess::NotRunning) {
    finishJob(false, "Highlights job stopped.");
    return;
  }
  requestActiveProcessStop();
}

void HighlightsDialog::requestActiveProcessStop() {
  const qint64 stopping_pid = process_.processId();
  if (stopping_pid <= 0)
    return;
  const quint64 generation = job_generation_;
  process_.terminate();
  QTimer::singleShot(4000, this, [this, stopping_pid, generation] {
    if (cancelling_ && job_generation_ == generation && process_.processId() == stopping_pid &&
        process_.state() != QProcess::NotRunning)
      process_.kill();
  });
}

} // namespace hm::ui
