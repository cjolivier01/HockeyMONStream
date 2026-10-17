#include "src/apps/hstream-ui/CalibrationCancelPrompt.h"

#include <QtCore/QString>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include <utility>

#include "src/apps/hstream-ui/ActionIcons.h"

void open_calibration_cancel_prompt(
    QWidget* parent,
    QPointer<QMessageBox>& owner,
    const QString& object_name,
    const QString& scope,
    const QString& alternative,
    std::function<void()> confirmed) {
  auto* prompt = new QMessageBox(
      QMessageBox::Warning,
      "Cancel stitching calibration",
      "Stop the whole stitching calibration run?",
      QMessageBox::NoButton,
      parent);
  prompt->setObjectName(object_name);
  prompt->setAttribute(Qt::WA_DeleteOnClose);
  prompt->setInformativeText(
      QString(
          "This cancels all of the calibration, not just %1. No alignment, maps, seam or rink mask are "
          "published, and the game is left without a complete calibration until stitching calibration runs "
          "again.\n\n%2")
          .arg(scope, alternative));
  auto* stop_calibration = prompt->addButton("Cancel calibration", QMessageBox::DestructiveRole);
  auto* keep_calibrating = prompt->addButton("Continue calibration", QMessageBox::RejectRole);
  stop_calibration->setIcon(action_icon(ActionIcon::Cancel));
  keep_calibrating->setIcon(action_icon(ActionIcon::Next));
  prompt->setDefaultButton(keep_calibrating);
  prompt->setEscapeButton(keep_calibrating);
  owner = prompt;
  // QMessageBox reports no clicked button when something closes it instead, so
  // a dismissed prompt cannot be mistaken for a confirmed cancellation.
  QObject::connect(
      prompt, &QDialog::finished, parent, [prompt, stop_calibration, &owner, confirmed = std::move(confirmed)]() {
        owner = nullptr;
        if (prompt->clickedButton() == stop_calibration)
          confirmed();
      });
  prompt->open();
}
