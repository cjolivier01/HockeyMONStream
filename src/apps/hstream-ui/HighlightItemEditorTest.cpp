#include "src/apps/hstream-ui/HighlightItemEditor.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <iostream>
using namespace hm::ui;
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
    dialog->findChild<QLineEdit*>("highlightCueText")->setText("Holding penalty");
    dialog->findChild<QListWidget*>("highlightAnnotations")->setCurrentRow(1);
    dialog->findChild<QLineEdit*>("highlightCueText")->setText("WHISTLE!");
    dialog->findChild<QDialogButtonBox*>("highlightItemButtons")->button(QDialogButtonBox::Save)->click();
  });
  if (!EditHighlightItem(&clip, dir.path(), "game", {}, {}, 0, {}, nullptr) || !valid ||
      clip.annotations[0].text != "Holding penalty" || clip.annotations[1].text != "WHISTLE!")
    return 1;
  return 0;
}
