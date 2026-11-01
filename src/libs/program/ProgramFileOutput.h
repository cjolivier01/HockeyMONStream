#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "absl/status/statusor.h"

namespace hm::program {
// Linux anonymous output on the destination filesystem. Only Commit makes the
// name visible, without replacing any existing entry. Destruction discards an
// uncommitted output, including after cancellation. Commit requires every writer
// to have finished mux EOS; it does not supervise writers or finalize containers.
class ProgramFileOutput {
 public:
  static absl::StatusOr<std::unique_ptr<ProgramFileOutput>> Create(const std::filesystem::path& destination);
  ~ProgramFileOutput();
  ProgramFileOutput(const ProgramFileOutput&) = delete;
  ProgramFileOutput& operator=(const ProgramFileOutput&) = delete;
  int fd() const;
  // Cancellation is checked before and after flushing the file. The successful
  // link is the commit boundary: cancellation after that point does not retract
  // a completed file. The callback may be empty when cancellation is not needed.
  // Standalone launches supply their game to inventory completed in-game files.
  // External exports are unchanged. Managed launches leave game empty because
  // their publication finalizer owns the inventory. An inventory failure does
  // not retract an already durable output.
  absl::Status Commit(const std::function<bool()>& cancelled, const std::filesystem::path& game = {});

 private:
  struct Impl;
  explicit ProgramFileOutput(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::program
