#include "hstream/src/libs/program/ProgramRuntimeControl.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <sstream>
#include <utility>

namespace hm::program {
ProgramRuntimeControl::ProgramRuntimeControl(int fd, size_t maximum, int poll_ms, Callbacks callbacks)
    : fd_(fd), poll_ms_(poll_ms), maximum_(maximum), callbacks_(std::move(callbacks)) {}
ProgramRuntimeControl::~ProgramRuntimeControl() {
  close(fd_);
}
absl::StatusOr<std::unique_ptr<ProgramRuntimeControl>> ProgramRuntimeControl::Create(
    int input_fd,
    size_t maximum,
    uint64_t poll_ns,
    Callbacks callbacks) {
  if (!maximum || !poll_ns || poll_ns > uint64_t(INT64_MAX) || !callbacks.pause || !callbacks.mute || !callbacks.emit ||
      !callbacks.cancelled || !callbacks.reset_progress || !callbacks.seek || !callbacks.preview || !callbacks.overlays)
    return absl::InvalidArgumentError("Runtime control requires explicit bounds and callbacks");
  const int fd = fcntl(input_fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0)
    return absl::UnavailableError("Cannot duplicate runtime input: " + std::string(std::strerror(errno)));
  return std::unique_ptr<ProgramRuntimeControl>(new ProgramRuntimeControl(
      fd, maximum, static_cast<int>(std::min<uint64_t>((poll_ns + 999999) / 1000000, INT_MAX)), std::move(callbacks)));
}
absl::Status ProgramRuntimeControl::Command(const std::string& line) {
  std::istringstream input(line);
  std::string command, value, extra;
  input >> command;
  if (command.empty())
    return absl::OkStatus();
  if (command == "@set-preview-overlays") {
    pipeline_internal::PreviewOverlayCommand parsed;
    const auto start = line.find('@');
    if (pipeline_internal::parse_preview_overlay_command(line.substr(start + 1), &parsed))
      return callbacks_.overlays(parsed);
    callbacks_.emit(
        "HSTREAM_PREVIEW_OVERLAYS status=failed generation=0 players=0 play=0 rink=0 reason=malformed-command\n");
    return absl::OkStatus();
  }
  if (command == "q" || command == "@quit") {
    if (!(input >> extra))
      return absl::CancelledError("Program stopped by runtime command");
  }
  input >> value;
  const bool trailing = bool(input >> extra);
  const auto integer = [](const std::string& text, auto* value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
  };
  if (command == "@set-preview-active") {
    std::string surplus;
    uint64_t generation = 0;
    if (trailing && !(input >> surplus) && integer(extra, &generation) && generation)
      return callbacks_.preview(value, generation);
  }
  if (command == "@seek" || command == "@seek-relative") {
    const bool relative = command == "@seek-relative";
    std::string surplus;
    int64_t position = 0;
    uint64_t generation = 0;
    if (trailing && !(input >> surplus) && integer(value, &position) && integer(extra, &generation) && generation &&
        (relative ? position != 0 : position >= 0)) {
      if (generation <= seek_generation_ || paused_) {
        callbacks_.emit(
            "HSTREAM_SEEK status=failed generation=" + std::to_string(generation) +
            " reason=" + (paused_ ? "playback-paused" : "stale-generation") + "\n");
        return absl::OkStatus();
      }
      seek_generation_ = generation;
      return callbacks_.seek(position, relative, generation);
    }
  }
  if (!trailing && command == "@reset-progress-rate") {
    uint64_t generation = 0;
    if (integer(value, &generation) && generation)
      return callbacks_.reset_progress(generation);
  }
  if (!trailing && (command == "@process-pause" || command == "@process-resume")) {
    uint64_t generation = 0;
    const auto number = std::from_chars(value.data(), value.data() + value.size(), generation);
    if (number.ec == std::errc{} && number.ptr == value.data() + value.size() && generation) {
      const bool prepare = command == "@process-pause";
      if ((prepare && generation > generation_) || (!prepare && generation == generation_)) {
        const auto status = callbacks_.pause(prepare);
        if (!status.ok())
          return status;
        generation_ = generation;
        paused_ = prepare;
        callbacks_.emit(
            "HSTREAM_PROCESS_PAUSE status=" + std::string(prepare ? "prepared" : "resumed") +
            " generation=" + std::to_string(generation) + "\n");
        return absl::OkStatus();
      }
      callbacks_.emit(
          "runtime command ignored: stale process pause/resume generation=" + std::to_string(generation) + "\n");
      return absl::OkStatus();
    }
  }
  if (!trailing && command == "@set-render-audio-muted" && (value == "0" || value == "1")) {
    const auto branches = callbacks_.mute(value == "1");
    if (!branches.ok())
      return branches.status();
    callbacks_.emit("HSTREAM_RENDER_AUDIO muted=" + value + " branches=" + std::to_string(*branches) + "\n");
    return absl::OkStatus();
  }
  callbacks_.emit("runtime command failed: unsupported or malformed AMD Program command\n");
  return absl::OkStatus();
}
absl::Status ProgramRuntimeControl::Service() {
  size_t consumed = 0;
  for (;;) {
    if (callbacks_.cancelled())
      return absl::CancelledError("Program runtime cancelled");
    if (eof_)
      return paused_ ? absl::CancelledError("Runtime input closed while Program was paused") : absl::OkStatus();
    if (!paused_ && consumed >= maximum_)
      return absl::OkStatus();
    struct pollfd descriptor {
      fd_, POLLIN, 0
    };
    const int result = poll(&descriptor, 1, paused_ ? poll_ms_ : 0);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return absl::UnavailableError("Cannot poll runtime input: " + std::string(std::strerror(errno)));
    }
    if (result == 0) {
      if (paused_)
        continue;
      return absl::OkStatus();
    }
    if (descriptor.revents & (POLLERR | POLLNVAL))
      return absl::UnavailableError("Runtime input failed");
    char byte{};
    const auto count = read(fd_, &byte, 1);
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return absl::UnavailableError("Cannot read runtime input: " + std::string(std::strerror(errno)));
    }
    if (!count) {
      eof_ = true;
      if (!line_.empty() || dropping_)
        callbacks_.emit("runtime command failed: unterminated command at EOF\n");
      line_.clear();
      continue;
    }
    ++consumed;
    if (byte == '\n') {
      if (!dropping_) {
        auto line = std::move(line_);
        line_.clear();
        const auto status = Command(line);
        if (!status.ok())
          return status;
      }
      dropping_ = false;
      continue;
    }
    if (!dropping_) {
      if (line_.size() == maximum_) {
        line_.clear();
        dropping_ = true;
        callbacks_.emit("runtime command failed: line exceeds configured byte limit\n");
      } else {
        line_.push_back(byte);
      }
    }
  }
}
} // namespace hm::program
