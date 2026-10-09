#include "src/apps/hstream-ui/HighlightItemEditor.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSplitter>
#include <iostream>
#include "src/apps/hstream-ui/HighlightScene.h"
using namespace hm::ui;
namespace {
// The controls are the point of the dialog, so they must open at a width that
// shows their labels and fields. A QScrollArea's own minimum is a bare
// scrollbar, and the splitter used to hand the preview everything else.
bool usable_controls(QDialog* dialog) {
  auto* splitter = dialog->findChild<QSplitter*>();
  auto* scroll = dialog->findChild<QScrollArea*>();
  if (!splitter || !scroll || !scroll->widget())
    return false;
  const int wanted = scroll->widget()->minimumSizeHint().width();
  if (wanted <= 0 || splitter->sizes().value(0) < wanted || scroll->minimumWidth() < wanted) {
    std::cout << "controls opened at " << splitter->sizes().value(0) << "/" << scroll->minimumWidth() << " of "
              << wanted << " needed\n";
    return false;
  }
  return true;
}
} // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir dir;
  HighlightInterval card;
  card.is_card = true;
  card.card.matchup = true;
  card.card.team_a = "Home";
  card.card.team_b = "Visitors";
  bool valid = true;
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) {
      valid = false;
      return;
    }
    valid &= usable_controls(dialog);
    auto* date = dialog->findChild<QLineEdit*>("highlightGameDate");
    auto* buttons = dialog->findChild<QDialogButtonBox*>("highlightItemButtons");
    if (!date->text().isEmpty()) {
      valid = false;
      dialog->reject();
      return;
    }
    buttons->button(QDialogButtonBox::Save)->click();
    if (!dialog->isVisible() || !dialog->findChild<QLabel*>("highlightItemStatus")->text().contains("YYYY-MM-DD")) {
      valid = false;
      dialog->reject();
      return;
    }
    date->setText("2026-10-08");
    buttons->button(QDialogButtonBox::Save)->click();
  });
  if (!EditHighlightItem(&card, dir.path(), "unknown-game", {}, {}, 0, {}, nullptr) || !valid ||
      card.card.date != "2026-10-08")
    return 1;
  HighlightInterval clip;
  clip.event_mode = false;
  clip.start_ms = 10500;
  clip.end_ms = 20000;
  HighlightAnnotation cue;
  cue.start_ms = 10000;
  cue.end_ms = 12000;
  clip.annotations = {cue, cue};
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) {
      valid = false;
      return;
    }
    valid &= usable_controls(dialog);
    valid &= dialog->findChild<QLineEdit*>("highlightTelemetryDatabase")->text() == dir.filePath("game_telemetry-7.db");
    dialog->findChild<QLineEdit*>("highlightCueText")->setText("Holding penalty");
    dialog->findChild<QListWidget*>("highlightAnnotations")->setCurrentRow(1);
    dialog->findChild<QLineEdit*>("highlightCueText")->setText("WHISTLE!");
    dialog->findChild<QDialogButtonBox*>("highlightItemButtons")->button(QDialogButtonBox::Save)->click();
  });
  if (!EditHighlightItem(
          &clip, dir.path(), "game", dir.filePath("game-tracking_output-with-audio-7.mp4"), {}, 0, {}, nullptr) ||
      !valid || clip.annotations[0].text != "Holding penalty" || clip.annotations[1].text != "WHISTLE!")
    return 1;
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    auto* movement = dialog->findChild<QComboBox*>("highlightCueMotion");
    auto* positions = dialog->findChild<QPlainTextEdit*>("highlightCuePositions");
    auto* status = dialog->findChild<QLabel*>("highlightItemStatus");
    auto* save = dialog->findChild<QDialogButtonBox*>("highlightItemButtons")->button(QDialogButtonBox::Save);
    movement->setCurrentText("keyframes");
    positions->clear();
    for (auto* button : dialog->findChildren<QPushButton*>())
      if (button->text() == "Inspect / refresh frame")
        button->click();
    valid &= !status->text().contains("Moving cues need saved positions");
    save->click();
    valid &= dialog->isVisible() && status->text().contains("Moving cues need saved positions");
    positions->setPlainText("00:00:01, 0.5, 0.3");
    save->click();
  });
  if (!EditHighlightItem(&clip, dir.path(), "game", {}, {}, 0, {}, nullptr) || !valid ||
      clip.annotations[0].motion != "keyframes" || clip.annotations[0].positions.size() != 1)
    return 1;
  // A logo named by hand, not chosen through the file dialog, is still copied
  // into the game so the card survives the original being deleted.
  QTemporaryDir pictures;
  const QString source = pictures.filePath("team.png");
  QImage art(48, 32, QImage::Format_RGBA8888);
  art.fill(Qt::red);
  if (!art.save(source))
    return 1;
  HighlightInterval logo;
  logo.is_card = true;
  logo.card.matchup = true;
  logo.card.team_a = "Home";
  logo.card.team_b = "Visitors";
  logo.card.date = "2026-10-08";
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    auto* field = dialog->findChild<QLineEdit*>("highlightLogoA");
    field->setText(source);
    dialog->findChild<QDialogButtonBox*>("highlightItemButtons")->button(QDialogButtonBox::Save)->click();
    if (dialog->isVisible()) {
      valid = false;
      std::cout << "card refused: " << dialog->findChild<QLabel*>("highlightItemStatus")->text().toStdString() << "\n";
      dialog->reject();
    }
  });
  if (!EditHighlightItem(&logo, dir.path(), "game", {}, {}, 0, {}, nullptr) || !valid ||
      !logo.card.logo_a.startsWith("highlight-assets/") || QFileInfo(logo.card.logo_a).isAbsolute())
    return 1;
  if (!QFile::remove(source))
    return 1;
  QImage rendered;
  QString error;
  if (!RasterHighlightCard(logo.card, dir.path(), &rendered, &error) || rendered.isNull()) {
    std::cout << "card lost its artwork: " << error.toStdString() << "\n";
    return 1;
  }
  // Reopening must keep the stored copy rather than re-importing it each time.
  const QString adopted = logo.card.logo_a;
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    dialog->findChild<QDialogButtonBox*>("highlightItemButtons")->button(QDialogButtonBox::Save)->click();
  });
  if (!EditHighlightItem(&logo, dir.path(), "game", {}, {}, 0, {}, nullptr) || logo.card.logo_a != adopted)
    return 1;
  return 0;
}
