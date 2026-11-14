#include "src/apps/hstream-ui/GameOwnershipDialog.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
QPushButton* button(GameOwnershipDialog& dialog, const char* name) {
  auto* result = dialog.findChild<QPushButton*>(name);
  require(result && !result->icon().isNull(), "Ownership actions retain icons");
  return result;
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary game available");
    const std::filesystem::path game(directory.path().toStdString());
    auto owner = hm::game_operation::LocalOwner("pipeline", "test-run", "dialog-token");
    owner.host = "remote-host";
    {
      auto lock = hm::game_operation::Lock::Acquire(game);
      require(lock.ok() && (*lock)->Claim(owner).ok(), "Claim fixture");
    }
    GameOwnershipDialog dialog(game);
    dialog.show();
    require(
        dialog.findChild<QPlainTextEdit*>("ownershipDetails")->toPlainText().contains("remote-host"),
        "Recorded remote host is visible");
    require(!button(dialog, "recoverOwnershipButton")->isEnabled(), "Remote owner is not verified stopped");
    auto* force = button(dialog, "forceReleaseOwnershipButton");
    QTimer::singleShot(0, [&] {
      auto* confirm = dialog.findChild<QMessageBox*>("forceReleaseOwnershipConfirmation");
      require(confirm && confirm->defaultButton()->text() == "Cancel", "Force release defaults to cancellation");
      confirm->reject();
    });
    force->click();
    require(hm::game_operation::Inspect(game)->has_value(), "Cancelling confirmation retains ownership");
    QTimer::singleShot(0, [&] {
      {
        auto lock = hm::game_operation::Lock::Acquire(game);
        require(
            lock.ok() && (*lock)->RequestCancel(owner.token, owner.revision).ok(),
            "Change revision during confirmation");
      }
      auto* confirm = dialog.findChild<QMessageBox*>("forceReleaseOwnershipConfirmation");
      for (auto* action : confirm->buttons())
        if (confirm->buttonRole(action) == QMessageBox::DestructiveRole)
          action->click();
    });
    force->click();
    require(hm::game_operation::Inspect(game)->has_value() && dialog.isVisible(), "Stale confirmation preserves owner");
    require(
        dialog.findChild<QLabel*>("ownershipStatus")->text().contains("changed"), "Stale confirmation is explained");
    require(!button(dialog, "stopOwnerButton")->isEnabled(), "Repeated cancellation is disabled");
    const auto screenshot = qEnvironmentVariable("HSTREAM_OWNERSHIP_SCREENSHOT");
    if (!screenshot.isEmpty()) {
      button(dialog, "refreshOwnershipButton")->click();
      app.processEvents();
      require(dialog.grab().save(screenshot), "Save ownership dialog screenshot");
    }
    QTimer::singleShot(0, [&] {
      auto* confirm = dialog.findChild<QMessageBox*>("forceReleaseOwnershipConfirmation");
      for (auto* action : confirm->buttons())
        if (confirm->buttonRole(action) == QMessageBox::DestructiveRole)
          action->click();
    });
    force->click();
    require(
        dialog.result() == QDialog::Accepted && !hm::game_operation::Inspect(game)->has_value(),
        "Confirmed release clears only the displayed owner");
    require(
        std::filesystem::exists(game / "_hockeymon/game-operation-revoked-dialog-token.yaml"),
        "Recovery audit is retained");
    auto local = hm::game_operation::LocalOwner("pipeline", "local-run", "local-token");
    {
      auto lock = hm::game_operation::Lock::Acquire(game);
      require(lock.ok() && (*lock)->Claim(local).ok(), "Claim local callback fixture");
    }
    int stops = 0;
    GameOwnershipDialog local_dialog(game, nullptr, [&] { ++stops; }, false);
    button(local_dialog, "stopOwnerButton")->click();
    require(stops == 1, "Local owner uses the normal Stop callback");
    require(
        !button(local_dialog, "forceReleaseOwnershipButton")->isEnabled(),
        "A supervising window retains its ordinary cleanup path");
    auto successor = hm::game_operation::LocalOwner("pipeline", "successor", "successor-token");
    {
      auto lock = hm::game_operation::Lock::Acquire(game);
      require(
          lock.ok() && (*lock)->Release(local.token, local.revision, true).ok() && (*lock)->Claim(successor).ok(),
          "Replace the displayed local owner");
    }
    button(local_dialog, "refreshOwnershipButton")->click();
    button(local_dialog, "stopOwnerButton")->click();
    require(
        stops == 1 && (**hm::game_operation::Inspect(game)).cancel_requested,
        "Refreshed successor receives its own cancellation instead of the old local callback");
    for (const auto* kind : {"int8", "experiment", "workflow", "mutation"}) {
      {
        auto lock = hm::game_operation::Lock::Acquire(game);
        auto current = (*lock)->Read();
        require(
            lock.ok() && current.ok() && current->has_value() &&
                (*lock)->Release((**current).token, (**current).revision, true).ok(),
            "Release previous fixture");
        auto unsupported = hm::game_operation::LocalOwner(kind, kind, std::string(kind) + "-token");
        require((*lock)->Claim(unsupported).ok(), "Claim unsupported remote stop fixture");
      }
      GameOwnershipDialog unsupported(game);
      require(!button(unsupported, "stopOwnerButton")->isEnabled(), "Unsupported owner has no ineffective Stop action");
    }
    std::cout << "Ownership dialog checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
