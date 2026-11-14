#pragma once

#include <QtWidgets/QDialog>
#include <filesystem>
#include <functional>
#include <optional>

#include "hstream/src/libs/common/GameOperationOwner.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;

// Play opens this only while admission is blocked. Merely inspecting never
// clears ownership, launches a pipeline, or probes an arbitrary remote host.
class GameOwnershipDialog final : public QDialog {
 public:
  GameOwnershipDialog(
      const std::filesystem::path& game,
      QWidget* parent = nullptr,
      std::function<void()> stop_owned_run = {},
      bool allow_force = true);

 private:
  void refresh();
  void release(bool force);
  std::filesystem::path game_;
  std::function<void()> stop_owned_run_;
  std::string stop_owned_token_;
  bool allow_force_;
  std::optional<hm::game_operation::Owner> owner_;
  QLabel* status_;
  QPlainTextEdit* details_;
  QPushButton* stop_;
  QPushButton* recover_;
  QPushButton* force_;
};
