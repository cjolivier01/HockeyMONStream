#include "src/apps/hstream-ui/GameOwnershipDialog.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include "src/apps/hstream-ui/ActionIcons.h"

namespace {
QString timestamp(int64_t ms) {
  return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString(Qt::ISODateWithMs) : "Unknown (legacy record)";
}
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
} // namespace

GameOwnershipDialog::GameOwnershipDialog(
    const std::filesystem::path& game,
    QWidget* parent,
    std::function<void()> stop_owned_run,
    bool allow_force)
    : QDialog(parent), game_(game), stop_owned_run_(std::move(stop_owned_run)), allow_force_(allow_force) {
  setObjectName("gameOwnershipDialog");
  setWindowTitle("Game is busy");
  setSizeGripEnabled(true);
  resize(720, 470);
  auto* layout = new QVBoxLayout(this);
  status_ = new QLabel();
  status_->setObjectName("ownershipStatus");
  status_->setTextFormat(Qt::PlainText);
  status_->setWordWrap(true);
  layout->addWidget(status_);
  details_ = new QPlainTextEdit();
  details_->setObjectName("ownershipDetails");
  details_->setReadOnly(true);
  layout->addWidget(details_);
  auto* buttons = new QHBoxLayout();
  auto* refresh_button = new QPushButton(action_icon(ActionIcon::Refresh), "Refresh");
  refresh_button->setObjectName("refreshOwnershipButton");
  stop_ = new QPushButton(action_icon(ActionIcon::Stop), "Request stop");
  stop_->setObjectName("stopOwnerButton");
  recover_ = new QPushButton(action_icon(ActionIcon::Apply), "Release verified owner");
  recover_->setObjectName("recoverOwnershipButton");
  force_ = new QPushButton(action_icon(ActionIcon::Reset), "Force release…");
  force_->setObjectName("forceReleaseOwnershipButton");
  auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close");
  close->setObjectName("closeOwnershipButton");
  for (auto* button : {refresh_button, stop_, recover_, force_, close}) {
    button->setAutoDefault(false);
    buttons->addWidget(button);
  }
  layout->addLayout(buttons);
  connect(refresh_button, &QPushButton::clicked, this, [this] { refresh(); });
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(recover_, &QPushButton::clicked, this, [this] { release(false); });
  connect(force_, &QPushButton::clicked, this, [this] { release(true); });
  connect(stop_, &QPushButton::clicked, this, [this] {
    if (!owner_)
      return;
    if (stop_owned_run_ && owner_->token == stop_owned_token_) {
      stop_owned_run_();
      refresh();
      return;
    }
    auto lock = hm::game_operation::Lock::Acquire(game_, false);
    const auto result = lock.ok() ? (*lock)->RequestCancel(owner_->token, owner_->revision) : lock.status();
    if (lock.ok())
      lock->reset();
    refresh();
    status_->setText(
        result.ok() ? "Stop requested. Ownership remains until the supervisor confirms cleanup. Refresh to check again."
                    : text(result.ToString()));
  });
  refresh();
  if (owner_ && stop_owned_run_)
    stop_owned_token_ = owner_->token;
}

void GameOwnershipDialog::refresh() {
  owner_.reset();
  stop_->setEnabled(false);
  recover_->setEnabled(false);
  force_->setEnabled(false);
  const auto info = hm::game_operation::InspectRecovery(game_);
  if (!info.ok()) {
    status_->setText(text(info.status().ToString()));
    return;
  }
  owner_ = info->owner;
  if (!owner_) {
    details_->clear();
    status_->setText(
        allow_force_ ? "Ownership has been released. Close this dialog and click Play again."
                     : "The durable owner is gone, but this window still needs to finish supervised cleanup.");
    return;
  }
  const auto& owner = *owner_;
  status_->setText(text(info->explanation));
  const auto details =
      QStringList{
          "Game: " + text(game_.string()),
          "Operation: " + text(owner.kind),
          "Operation ID: " + text(owner.id),
          "Host: " + text(owner.host),
          "Recorded PID: " + QString::number(owner.process_id),
          "Ownership started: " + timestamp(owner.started_unix_ms),
          "Record last changed: " + timestamp(info->updated_unix_ms) + " (not a heartbeat)",
          "Boot ID: " + text(owner.boot_id),
          "Process start ticks: " + QString::number(owner.process_start_ticks),
          "Process state: " + (info->process_state.empty() ? "Unknown" : text(info->process_state)),
          "Executable: " + (info->executable.empty() ? "Unknown" : text(info->executable)),
          "Stop requested: " + QString(owner.cancel_requested ? "Yes" : "No"),
          "Revision: " + QString::number(owner.revision),
          "Workflow: " + (owner.workflow_directory.empty() ? "None recorded" : text(owner.workflow_directory)),
          "Processing progress: unavailable from the ownership record.",
          "Quiet logs, paused playback and waiting for input do not establish stale ownership."}
          .join('\n');
  if (details_->toPlainText() != details)
    details_->setPlainText(details);
  stop_->setEnabled(
      !owner.cancel_requested &&
      ((stop_owned_run_ && owner.token == stop_owned_token_) ||
       (owner.started_unix_ms > 0 && owner.kind != "workflow")));
  recover_->setEnabled(info->verified_reboot);
  force_->setEnabled(allow_force_);
  force_->setToolTip(
      allow_force_ ? "Abandon the displayed owner without claiming verified shutdown."
                   : "This window still supervises the run. Use Stop and wait for cleanup.");
}

void GameOwnershipDialog::release(bool force) {
  if (!owner_)
    return;
  // Keep the exact displayed identity through confirmation; never release a
  // different owner that appeared while the user read the dialog.
  const auto expected = *owner_;
  if (force) {
    QMessageBox confirmation(
        QMessageBox::Warning,
        "Force release ownership",
        "This does not prove the old run has stopped. A surviving process could conflict with a new run.\n\n"
        "The ownership record, logs and unfinished outputs will be retained. This action does not publish outputs.\n\n"
        "Force release the displayed owner?",
        QMessageBox::NoButton,
        this);
    confirmation.setObjectName("forceReleaseOwnershipConfirmation");
    auto* cancel = confirmation.addButton("Cancel", QMessageBox::RejectRole);
    cancel->setIcon(action_icon(ActionIcon::Cancel));
    auto* proceed = confirmation.addButton("Force release", QMessageBox::DestructiveRole);
    proceed->setIcon(action_icon(ActionIcon::Reset));
    confirmation.setDefaultButton(cancel);
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != proceed)
      return;
  }
  auto lock = hm::game_operation::Lock::Acquire(game_, false);
  const auto result = lock.ok()
      ? (*lock)->Recover(
            expected.token,
            expected.revision,
            force,
            force ? "Explicit operator force release from Play ownership dialog" : "Verified local host reboot")
      : lock.status();
  if (lock.ok())
    lock->reset();
  if (result.ok())
    accept();
  else {
    refresh();
    status_->setText(text(result.ToString()));
  }
}
