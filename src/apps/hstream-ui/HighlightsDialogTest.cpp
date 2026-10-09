#include "src/apps/hstream-ui/HighlightsDialog.h"

#include "src/apps/hstream-ui/ArchiveCatalog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizeGrip>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QToolButton>

#include <cmath>
#include <iostream>

namespace {

QApplication* application = nullptr;

bool run(const QString& program, const QStringList& args, QByteArray* output = nullptr) {
  QProcess process;
  process.start(program, args);
  if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    std::cerr << process.readAllStandardError().constData() << std::endl;
    return false;
  }
  if (output)
    *output = process.readAllStandardOutput();
  return true;
}

// A published archive is two visibly different halves, so a cut can be checked
// by sampling a pixel rather than by trusting the reported duration.
bool publishArchive(const QString& path, const QString& first_color, const QString& second_color, int size) {
  const QString geometry = QString("%1x%2").arg(size).arg(size * 3 / 4);
  return run(
      "ffmpeg",
      {"-hide_banner",
       "-loglevel",
       "error",
       "-y",
       "-f",
       "lavfi",
       "-i",
       "color=c=" + first_color + ":s=" + geometry + ":r=30:d=3",
       "-f",
       "lavfi",
       "-i",
       "sine=frequency=440:sample_rate=48000:duration=3",
       "-f",
       "lavfi",
       "-i",
       "color=c=" + second_color + ":s=" + geometry + ":r=30:d=3",
       "-f",
       "lavfi",
       "-i",
       "sine=frequency=660:sample_rate=48000:duration=3",
       "-filter_complex",
       "[0:v][1:a][2:v][3:a]concat=n=2:v=1:a=1[v][a]",
       "-map",
       "[v]",
       "-map",
       "[a]",
       "-c:v",
       "libx264",
       "-pix_fmt",
       "yuv420p",
       "-g",
       "30",
       "-c:a",
       "aac",
       path});
}

bool writeSidecar(const QString& archive_path, const QString& game_id, const QString& kind, qint64 start_time_ms) {
  hm::ui::ArchiveEntry entry;
  entry.path = archive_path;
  entry.game_id = game_id;
  entry.kind = kind;
  entry.start_time_ms = start_time_ms;
  entry.start_time_known = true;
  QString error;
  if (hm::ui::SaveArchiveSidecar(entry, &error))
    return true;
  std::cerr << error.toStdString() << std::endl;
  return false;
}

bool settle(hm::ui::HighlightsDialog* dialog, int milliseconds) {
  for (int elapsed = 0; elapsed < milliseconds && dialog->isBusy(); ++elapsed) {
    application->processEvents();
    QThread::msleep(1);
  }
  application->processEvents();
  if (!dialog->isBusy())
    return true;
  std::cerr << "Dialog stayed busy: " << dialog->findChild<QLabel*>("highlightStatus")->text().toStdString()
            << std::endl;
  return false;
}

QString statusOf(hm::ui::HighlightsDialog* dialog) {
  return dialog->findChild<QLabel*>("highlightStatus")->text();
}

bool addRange(hm::ui::HighlightsDialog* dialog, const QString& name, const QString& start, const QString& end) {
  auto* table = dialog->findChild<QTableWidget*>("highlightsTable");
  const int before = table->rowCount();
  dialog->findChild<QComboBox*>("highlightModeCombo")->setCurrentIndex(1);
  dialog->findChild<QLineEdit*>("highlightLabelEdit")->setText(name);
  dialog->findChild<QLineEdit*>("highlightFirstTimeEdit")->setText(start);
  dialog->findChild<QLineEdit*>("highlightSecondTimeEdit")->setText(end);
  dialog->findChild<QPushButton*>("highlightAddButton")->click();
  if (table->rowCount() == before + 1)
    return true;
  std::cerr << "Could not add " << name.toStdString() << ": " << statusOf(dialog).toStdString() << std::endl;
  return false;
}

QString cell(hm::ui::HighlightsDialog* dialog, int row, int column) {
  auto* item = dialog->findChild<QTableWidget*>("highlightsTable")->item(row, column);
  return item ? item->text() : QString();
}

// The "In archive" cell is a dash when the interval does not fit, and the two
// archive timestamps otherwise. Checking the ends keeps the test off the exact
// separator character.
bool mapsTo(hm::ui::HighlightsDialog* dialog, int row, const QString& start, const QString& end) {
  const QString text = cell(dialog, row, 5);
  return text.startsWith(start) && text.endsWith(end);
}

bool unmapped(hm::ui::HighlightsDialog* dialog, int row) {
  return !cell(dialog, row, 5).contains(':');
}

// Mean red and blue of one decoded frame, so clip order and content can be
// asserted without decoding the whole output.
bool samplePixel(const QString& path, const QString& at, int* red, int* blue) {
  QByteArray pixel;
  if (!run(
          "ffmpeg",
          {"-hide_banner", "-loglevel", "error", "-ss", at, "-i", path, "-frames:v", "1", "-vf", "scale=1:1",
           "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"},
          &pixel) ||
      pixel.size() != 3)
    return false;
  *red = static_cast<unsigned char>(pixel[0]);
  *blue = static_cast<unsigned char>(pixel[2]);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  application = &app;
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()) {
    std::cerr << "ffmpeg and ffprobe are required" << std::endl;
    return 1;
  }
  QTemporaryDir temporary(QDir::tempPath() + "/hstream-highlights-test-XXXXXX");
  if (!temporary.isValid())
    return 1;
  const QString game_dir = temporary.filePath("test-game");
  if (!QDir().mkpath(game_dir))
    return 1;
  const QString program_archive = QDir(game_dir).filePath("test-game-tracking_output-with-audio-1.mp4");
  const QString reduced_archive = QDir(game_dir).filePath("test-game-program_4k_output-with-audio-1.mp4");
  if (!publishArchive(program_archive, "red", "blue", 320) ||
      !publishArchive(reduced_archive, "green", "white", 160) ||
      !writeSidecar(program_archive, "test-game", "program", 600000) ||
      !writeSidecar(reduced_archive, "test-game", "program_4k", 1200000))
    return 1;

  QVector<hm::ui::ArchiveEntry> archives = hm::ui::DiscoverArchives(game_dir, "test-game");
  if (archives.size() != 2 || archives.at(0).kind != "program" || archives.at(1).kind != "program_4k") {
    std::cerr << "Published archives were not discovered in preference order" << std::endl;
    return 1;
  }

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  // Keep the test off the GPU and off whatever encoders this host happens to
  // have; the encoder choice itself is covered by its own unit test.
  env.insert("HSTREAM_UI_HIGHLIGHTS_VIDEO_ENCODER", "libx264");
  env.insert("HSTREAM_UI_HIGHLIGHTS_VIDEO_PRESET", "ultrafast");

  hm::ui::HighlightsDialog dialog("test-game", game_dir, env, archives);
  dialog.show();
  if (!settle(&dialog, 30000))
    return 1;

  auto* maximize_window = dialog.findChild<QToolButton*>("maximizeHighlightsWindowButton");
  auto* resize_grip = dialog.findChild<QSizeGrip*>();
  if (!dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint) || !dialog.isSizeGripEnabled() || !resize_grip ||
      !resize_grip->isVisible() || !maximize_window) {
    std::cerr << "Highlights window must support maximize and resize" << std::endl;
    return 1;
  }
  const QSize resized = dialog.size() + QSize(80, 60);
  dialog.resize(resized);
  for (int attempt = 0; attempt < 200 && dialog.size() != resized; ++attempt) {
    app.processEvents();
    QThread::msleep(10);
  }
  if (dialog.size() != resized) {
    std::cerr << "Highlights window did not resize" << std::endl;
    return 1;
  }
  maximize_window->click();
  for (int attempt = 0; attempt < 200 && !dialog.isMaximized(); ++attempt) {
    app.processEvents();
    QThread::msleep(10);
  }
  if (!dialog.isMaximized() || maximize_window->text() != "Restore window") {
    std::cerr << "Highlights window did not maximize" << std::endl;
    return 1;
  }
  maximize_window->click();
  for (int attempt = 0; attempt < 200 && dialog.isMaximized(); ++attempt) {
    app.processEvents();
    QThread::msleep(10);
  }
  if (dialog.isMaximized() || maximize_window->text() != "Maximize window") {
    std::cerr << "Highlights window did not restore" << std::endl;
    return 1;
  }
  auto* expand_preview = dialog.findChild<QToolButton*>("highlightExpandPreviewButton");
  if (!dialog.findChild<QWidget*>("highlightVideo") || !expand_preview || expand_preview->icon().isNull() ||
      !expand_preview->text().isEmpty() || expand_preview->accessibleName() != "Maximize preview")
    return 1;
  expand_preview->click();
  app.processEvents();
  if (dialog.findChild<QTableWidget*>("highlightsTable")->isVisible() ||
      expand_preview->accessibleName() != "Restore preview" ||
      !dialog.findChild<QWidget*>("highlightVideo")->isVisible()) {
    std::cerr << "Expanded preview did not focus the video" << std::endl;
    return 1;
  }
  expand_preview->click();
  app.processEvents();
  if (!dialog.findChild<QTableWidget*>("highlightsTable")->isVisible())
    return 1;
  for (const char* name :
       {"highlightAddButton",
        "highlightUpdateButton",
        "highlightRemoveButton",
        "highlightUpButton",
        "highlightDownButton",
        "highlightPreviewSelectedButton",
        "highlightPreviewAllButton",
        "highlightLoopSelectedButton",
        "highlightLoopButton",
        "highlightExportSelectedButton",
        "highlightExportAllButton",
        "highlightStopButton"}) {
    auto* button = dialog.findChild<QPushButton*>(name);
    if (!button || button->icon().isNull()) {
      std::cerr << "Missing colored action icon: " << name << std::endl;
      return 1;
    }
  }

  auto* archive_combo = dialog.findChild<QComboBox*>("highlightArchiveCombo");
  auto* archive_offset = dialog.findChild<QLineEdit*>("highlightArchiveOffsetEdit");
  auto* archive_detail = dialog.findChild<QLabel*>("highlightArchiveDetail");
  if (!archive_combo || !archive_offset || !archive_detail || archive_combo->count() != 2 ||
      !archive_combo->isVisible()) {
    std::cerr << "Two archives must be offered for selection" << std::endl;
    return 1;
  }
  if (!archive_combo->itemText(0).contains("Program") || !archive_combo->itemText(0).contains("320") ||
      !archive_combo->itemText(0).contains("240") || !archive_combo->itemText(0).contains("starts 00:10:00") ||
      !archive_combo->itemText(1).contains("4K Program")) {
    std::cerr << "Archive labels did not describe the sources: " << archive_combo->itemText(0).toStdString()
              << std::endl;
    return 1;
  }
  if (archive_offset->text() != "00:10:00" || !archive_detail->text().contains("covers 00:10:00 to 00:10:06") ||
      !archive_detail->text().contains("H264") || !archive_detail->text().contains("with audio")) {
    std::cerr << "Inspection did not describe the selected archive: " << archive_detail->text().toStdString()
              << std::endl;
    return 1;
  }

  // Intervals are kept in game time; the archive's own timeline starts at the
  // sidecar's offset.
  if (!addRange(&dialog, "First", "00:10:00.500", "00:10:02.500") ||
      !addRange(&dialog, "Second", "00:10:03.500", "00:10:05.500"))
    return 1;
  if (!mapsTo(&dialog, 0, "00:00:00.500", "00:00:02.500") || !mapsTo(&dialog, 1, "00:00:03.500", "00:00:05.500")) {
    std::cerr << "Game times were not mapped onto the archive: " << cell(&dialog, 0, 5).toStdString() << std::endl;
    return 1;
  }

  // Preview needs the GPU renderer and an X11 display; this test runs offscreen.
  if (dialog.findChild<QPushButton*>("highlightPreviewAllButton")->isEnabled() ||
      dialog.findChild<QPushButton*>("highlightLoopButton")->isEnabled() ||
      !dialog.findChild<QPushButton*>("highlightExportAllButton")->isEnabled()) {
    std::cerr << "Preview must be disabled without a renderer while export stays available" << std::endl;
    return 1;
  }

  dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  if (!settle(&dialog, 180000))
    return 1;
  const QString output = QDir(game_dir).filePath("test-game-highlights-program-1.mp4");
  if (!QFileInfo::exists(output) || !statusOf(&dialog).contains("Highlights saved")) {
    std::cerr << "Export did not publish: " << statusOf(&dialog).toStdString() << std::endl;
    std::cerr << dialog.findChild<QTextEdit*>("highlightLog")->toPlainText().toStdString() << std::endl;
    return 1;
  }
  QByteArray probe;
  if (!run("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", output}, &probe))
    return 1;
  const QJsonObject root = QJsonDocument::fromJson(probe).object();
  const double duration = root.value("format").toObject().value("duration").toString().toDouble();
  if (std::abs(duration - 4.0) > 0.1 || root.value("streams").toArray().size() != 2) {
    std::cerr << "Unexpected joined streams or duration: " << probe.constData() << std::endl;
    return 1;
  }
  int red = 0;
  int blue = 0;
  if (!samplePixel(output, "0.5", &red, &blue) || red < blue + 50) {
    std::cerr << "The first clip did not come from the first half of the archive" << std::endl;
    return 1;
  }
  if (!samplePixel(output, "3.0", &red, &blue) || blue < red + 50) {
    std::cerr << "The second clip did not come from the second half of the archive" << std::endl;
    return 1;
  }
  // The work directory lives inside the game directory so publication can hard
  // link out of it; a successful export must not leave it behind.
  if (!QDir(game_dir).entryList({".highlights-export-*"}, QDir::Dirs | QDir::Hidden).isEmpty()) {
    std::cerr << "A successful export left its work directory behind" << std::endl;
    return 1;
  }

  auto* log = dialog.findChild<QTextEdit*>("highlightLog");
  log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
  dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  if (!settle(&dialog, 180000))
    return 1;
  if (!QFileInfo::exists(QDir(game_dir).filePath("test-game-highlights-program-2.mp4"))) {
    std::cerr << "A second export did not take the next generation" << std::endl;
    return 1;
  }
  if (log->verticalScrollBar()->maximum() == 0 ||
      log->verticalScrollBar()->value() != log->verticalScrollBar()->maximum()) {
    std::cerr << "Highlights log did not follow the newest line" << std::endl;
    return 1;
  }

  // Out-of-range intervals are named, not clamped or silently skipped.
  if (!addRange(&dialog, "Late", "00:10:05", "00:10:08"))
    return 1;
  if (!unmapped(&dialog, 2)) {
    std::cerr << "An interval past the archive end must not report a mapping" << std::endl;
    return 1;
  }
  dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  app.processEvents();
  if (dialog.isBusy() || !statusOf(&dialog).contains("\"Late\" ends after this archive, which stops at 00:10:06")) {
    std::cerr << "Export did not refuse an interval past the archive end: " << statusOf(&dialog).toStdString()
              << std::endl;
    return 1;
  }
  dialog.findChild<QTableWidget*>("highlightsTable")->setCurrentCell(2, 0);
  dialog.findChild<QPushButton*>("highlightRemoveButton")->click();
  if (!addRange(&dialog, "Early", "00:09:59", "00:10:01"))
    return 1;
  dialog.findChild<QPushButton*>("highlightExportAllButton")->click();
  app.processEvents();
  if (dialog.isBusy() || !statusOf(&dialog).contains("\"Early\" starts 00:00:01 before this archive begins")) {
    std::cerr << "Export did not refuse an interval before the archive start: " << statusOf(&dialog).toStdString()
              << std::endl;
    return 1;
  }
  dialog.findChild<QTableWidget*>("highlightsTable")->setCurrentCell(2, 0);
  dialog.findChild<QPushButton*>("highlightRemoveButton")->click();

  // An edited offset re-reads the same game times against the archive and
  // survives a round trip through the other archive.
  archive_offset->setText("00:00:00");
  emit archive_offset->editingFinished();
  app.processEvents();
  if (!unmapped(&dialog, 0) || !statusOf(&dialog).contains("archive starting at 00:00:00")) {
    std::cerr << "Editing the offset did not re-read the intervals: " << statusOf(&dialog).toStdString() << std::endl;
    return 1;
  }
  archive_combo->setCurrentIndex(1);
  if (!settle(&dialog, 30000))
    return 1;
  if (archive_offset->text() != "00:20:00" || !archive_detail->text().contains("covers 00:20:00 to 00:20:06") ||
      !archive_detail->text().contains("program_4k_output")) {
    std::cerr << "Selecting the other archive did not re-inspect it: " << archive_detail->text().toStdString()
              << std::endl;
    return 1;
  }
  archive_combo->setCurrentIndex(0);
  if (!settle(&dialog, 30000))
    return 1;
  if (archive_offset->text() != "00:00:00") {
    std::cerr << "The edited offset was lost when the archive was reselected" << std::endl;
    return 1;
  }
  archive_offset->setText("00:10:00");
  emit archive_offset->editingFinished();
  app.processEvents();
  if (!mapsTo(&dialog, 0, "00:00:00.500", "00:00:02.500"))
    return 1;
  archive_offset->setText("nonsense");
  emit archive_offset->editingFinished();
  app.processEvents();
  if (archive_offset->text() != "00:10:00" || !statusOf(&dialog).contains("Invalid archive start time")) {
    std::cerr << "An unreadable offset must be rejected and reverted" << std::endl;
    return 1;
  }

  // Escape closes the dialog when nothing is running.
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&dialog, &escape);
  app.processEvents();
  if (dialog.isVisible()) {
    std::cerr << "Escape did not close an idle Highlights dialog" << std::endl;
    return 1;
  }

  // An unreadable plan blocks every edit, and inspection still reports the
  // archive without overwriting the explanation.
  const QString incompatible_dir = temporary.filePath("incompatible-game");
  if (!QDir().mkpath(incompatible_dir))
    return 1;
  const QString incompatible_plan_path = QDir(incompatible_dir).filePath("highlights.json");
  const QByteArray incompatible_plan = R"({"schema":2,"base_name":"future","intervals":[]})";
  QFile incompatible_file(incompatible_plan_path);
  if (!incompatible_file.open(QIODevice::WriteOnly) ||
      incompatible_file.write(incompatible_plan) != incompatible_plan.size())
    return 1;
  incompatible_file.close();
  const QString incompatible_archive =
      QDir(incompatible_dir).filePath("incompatible-game-tracking_output-with-audio-1.mp4");
  if (!QFile::copy(program_archive, incompatible_archive))
    return 1;
  hm::ui::HighlightsDialog incompatible_dialog(
      "incompatible-game",
      incompatible_dir,
      env,
      hm::ui::DiscoverArchives(incompatible_dir, "incompatible-game"));
  if (!settle(&incompatible_dialog, 30000))
    return 1;
  if (incompatible_dialog.findChild<QPushButton*>("highlightAddButton")->isEnabled() ||
      incompatible_dialog.findChild<QLineEdit*>("highlightBaseNameEdit")->isEnabled() ||
      !statusOf(&incompatible_dialog).contains("Unsupported highlights schema")) {
    std::cerr << "An unreadable highlight plan must block edits: "
              << statusOf(&incompatible_dialog).toStdString() << std::endl;
    return 1;
  }
  incompatible_dialog.findChild<QPushButton*>("highlightAddButton")->click();
  if (!incompatible_file.open(QIODevice::ReadOnly) || incompatible_file.readAll() != incompatible_plan) {
    std::cerr << "Editing an unreadable plan must not replace it" << std::endl;
    return 1;
  }
  incompatible_file.close();

  // A game with nothing published must explain itself rather than crash.
  const QString empty_dir = temporary.filePath("empty-game");
  if (!QDir().mkpath(empty_dir))
    return 1;
  hm::ui::HighlightsDialog empty_dialog("empty-game", empty_dir, env, {});
  app.processEvents();
  if (empty_dialog.isBusy() ||
      !empty_dialog.findChild<QLabel*>("highlightArchiveDetail")->text().contains("No published archive") ||
      empty_dialog.findChild<QPushButton*>("highlightExportAllButton")->isEnabled()) {
    std::cerr << "A game with no archive must disable cutting and say why" << std::endl;
    return 1;
  }
  return 0;
}
