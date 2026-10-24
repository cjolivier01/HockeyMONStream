#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "absl/status/statusor.h"

namespace hm::program {
// Owns a duplicate of the input descriptor without changing its flags. Call
// Service only between complete audio/video frame submissions, on the same
// thread as the outputs. No operation deadline may span a prepared SIGSTOP.
class ProgramRuntimeControl {
 public:
  struct Callbacks {
    std::function<absl::Status(bool)> pause;
    std::function<absl::StatusOr<size_t>(bool)> mute;
    std::function<void(const std::string&)> emit;
    std::function<bool()> cancelled;
    // Callbacks own acknowledgment/validation of their application state.
    // A failed status retires playback. Rejected requests return OK after their
    // failure acknowledgment, without partially modifying the graph.
    std::function<absl::Status(uint64_t)> reset_progress;
    std::function<absl::Status(uint64_t, uint64_t)> seek;
  };
  static absl::StatusOr<std::unique_ptr<ProgramRuntimeControl>> Create(
      int input_fd,
      size_t maximum_line_bytes,
      uint64_t cancellation_poll_ns,
      Callbacks callbacks);
  ~ProgramRuntimeControl();
  // Processes bounded pending input; while paused, waits for resume/cancel.
  // Malformed/stale commands report failure without changing runtime state.
  // EOF while paused cancels the run; ordinary EOF leaves playback running.
  absl::Status Service();

 private:
  ProgramRuntimeControl(int fd, size_t maximum, int poll_ms, Callbacks callbacks);
  absl::Status Command(const std::string& line);
  int fd_, poll_ms_;
  size_t maximum_;
  Callbacks callbacks_;
  std::string line_;
  uint64_t generation_{0};
  uint64_t seek_generation_{0};
  bool paused_{false}, eof_{false}, dropping_{false};
};
} // namespace hm::program
