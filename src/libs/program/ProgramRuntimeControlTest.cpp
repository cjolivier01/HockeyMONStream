#include "hstream/src/libs/program/ProgramRuntimeControl.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  int writer{-1};
  std::atomic<bool> cancelled{false};
  bool paused{false}, muted{false}, fail_pause{false};
  size_t pause_calls{0};
  uint64_t reset_generation{0}, seek_generation{0};
  int64_t seek_position{0};
  bool seek_relative{false};
  std::string preview_channel;
  uint64_t preview_generation{0};
  hm::pipeline_internal::PreviewOverlayCommand overlays;
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::string> messages;
  std::unique_ptr<hm::program::ProgramRuntimeControl> control;
  std::future<absl::Status> pending;
  Fixture() {
    int pipefd[2];
    Check(pipe(pipefd) == 0, "pipe");
    writer = pipefd[1];
    auto created = hm::program::ProgramRuntimeControl::Create(
        pipefd[0],
        64,
        5000000,
        {[this](bool value) {
           std::lock_guard<std::mutex> lock(mutex);
           if (fail_pause)
             return absl::UnavailableError("test output refused pause");
           paused = value;
           ++pause_calls;
           return absl::OkStatus();
         },
         [this](bool value) -> absl::StatusOr<size_t> {
           muted = value;
           return 1;
         },
         [this](const std::string& value) {
           {
             std::lock_guard<std::mutex> lock(mutex);
             messages.push_back(value);
           }
           wake.notify_all();
         },
         [this] { return cancelled.load(); },
         [this](uint64_t generation) {
           reset_generation = generation;
           return absl::OkStatus();
         },
         [this](int64_t position, bool relative, uint64_t generation) {
           seek_position = position;
           seek_relative = relative;
           seek_generation = generation;
           return absl::OkStatus();
         },
         [this](const std::string& channel, uint64_t generation) {
           preview_channel = channel;
           preview_generation = generation;
           return absl::OkStatus();
         },
         [this](const hm::pipeline_internal::PreviewOverlayCommand& command) {
           overlays = command;
           return absl::OkStatus();
         }});
    close(pipefd[0]);
    Check(created.ok(), created.status().ToString());
    control = std::move(*created);
  }
  ~Fixture() {
    cancelled = true;
    if (pending.valid())
      pending.wait();
    if (writer >= 0)
      close(writer);
  }
  void Send(const std::string& bytes) {
    Check(write(writer, bytes.data(), bytes.size()) == ssize_t(bytes.size()), "write command");
  }
  void Start() {
    pending = std::async(std::launch::async, [this] { return control->Service(); });
  }
  void WaitMessage(const std::string& text) {
    std::unique_lock<std::mutex> lock(mutex);
    Check(
        wake.wait_for(
            lock,
            std::chrono::seconds(2),
            [&] {
              for (const auto& message : messages)
                if (message.find(text) != std::string::npos)
                  return true;
              return false;
            }),
        "missing acknowledgment: " + text);
  }
  absl::Status Complete() {
    Check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "control did not unblock");
    return pending.get();
  }
};
} // namespace
int main() {
  try {
    {
      Fixture fixture;
      fixture.Send("@process-pa");
      Check(fixture.control->Service().ok() && fixture.pause_calls == 0, "partial command executed");
      fixture.Send("use 7\n");
      fixture.Start();
      fixture.WaitMessage("status=prepared generation=7");
      fixture.Send("@seek 100 1\n");
      fixture.WaitMessage("reason=playback-paused");
      fixture.Send("@process-resume 6\n");
      fixture.WaitMessage("stale process pause/resume generation=6");
      {
        std::lock_guard<std::mutex> lock(fixture.mutex);
        Check(fixture.paused && fixture.pause_calls == 1, "stale resume changed state");
      }
      fixture.Send("@process-resume 7\n");
      Check(fixture.Complete().ok() && !fixture.paused && fixture.pause_calls == 2, "resume failed");
      fixture.WaitMessage("status=resumed generation=7");
      fixture.Send("@set-render-audio-muted 1\n");
      Check(fixture.control->Service().ok() && fixture.muted, "mute failed");
      fixture.WaitMessage("HSTREAM_RENDER_AUDIO muted=1 branches=1");
      fixture.Send("@set-preview-active program 9\n");
      Check(
          fixture.control->Service().ok() && fixture.preview_channel == "program" && fixture.preview_generation == 9,
          "preview activation fields lost");
      for (const auto* command :
           {"@set-preview-active none 0\n", "@set-preview-active none -1\n", "@set-preview-active none 10 extra\n"}) {
        fixture.Send(command);
        Check(
            fixture.control->Service().ok() && fixture.preview_channel == "program" && fixture.preview_generation == 9,
            "malformed preview activation executed");
      }
      fixture.Send("@reset-progress-rate 3\n@seek 123456789 2\n");
      Check(
          fixture.control->Service().ok() && fixture.reset_generation == 3 && fixture.seek_position == 123456789 &&
              fixture.seek_generation == 2,
          "seek/reset fields lost");
      fixture.Send("@set-preview-overlays 9 1 0 1\n");
      Check(
          fixture.control->Service().ok() && fixture.overlays.generation == 9 && fixture.overlays.selection.players &&
              !fixture.overlays.selection.play && fixture.overlays.selection.rink,
          "overlay command fields lost");
      for (const auto* command :
           {"@set-preview-overlays 0 0 0 0\n",
            "@set-preview-overlays 10 2 0 0\n",
            "@set-preview-overlays 10 0 0\n",
            "@set-preview-overlays 10 0 0 0 extra\n"}) {
        fixture.Send(command);
        Check(
            fixture.control->Service().ok() && fixture.overlays.generation == 9, "malformed overlay command executed");
      }
      fixture.Send("@seek 999 1\n@seek 123 2 extra\n@seek -1 3\n");
      Check(
          fixture.control->Service().ok() && fixture.seek_position == 123456789 && fixture.seek_generation == 2,
          "stale/malformed seek executed");
      fixture.Send("@seek-relative -9223372036854775808 3\n");
      Check(
          fixture.control->Service().ok() && fixture.seek_relative && fixture.seek_position == INT64_MIN &&
              fixture.seek_generation == 3,
          "signed jump lost or overflowed");
      fixture.Send("@seek-relative 0 4\n");
      Check(fixture.control->Service().ok() && fixture.seek_generation == 3, "zero jump executed");
      fixture.Send("@process-pause +8\n@process-pause 0\n@process-pause 8 extra\n");
      Check(fixture.control->Service().ok() && fixture.pause_calls == 2, "malformed generation executed");
      fixture.Send(std::string(100, 'x') + "@quit\n@set-render-audio-muted 0\n");
      for (int i = 0; i < 4; ++i)
        Check(fixture.control->Service().ok(), "overlong line escaped as a command");
      Check(!fixture.muted, "overlong line prevented following valid command");
      fixture.WaitMessage("line exceeds configured byte limit");
    }
    {
      Fixture fixture;
      fixture.Send("@process-pause 1\n");
      fixture.Start();
      fixture.WaitMessage("status=prepared");
      fixture.cancelled = true;
      Check(absl::IsCancelled(fixture.Complete()), "paused control ignored cancellation");
    }
    {
      Fixture fixture;
      fixture.Send("@process-pause 1\n");
      fixture.Start();
      fixture.WaitMessage("status=prepared");
      close(fixture.writer);
      fixture.writer = -1;
      Check(absl::IsCancelled(fixture.Complete()), "paused EOF did not stop playback");
    }
    {
      Fixture fixture;
      fixture.Send("@process-pause 1\n");
      fixture.fail_pause = true;
      Check(
          !fixture.control->Service().ok() && fixture.messages.empty(),
          "failed output received a success acknowledgment");
    }
    {
      Fixture fixture;
      fixture.Send("@quit\n");
      Check(absl::IsCancelled(fixture.control->Service()), "quit failed");
    }
    {
      Fixture fixture;
      fixture.Send("@process-pause 1");
      close(fixture.writer);
      fixture.writer = -1;
      Check(fixture.control->Service().ok() && fixture.pause_calls == 0, "partial command executed at EOF");
      Check(fixture.control->Service().ok(), "ordinary EOF stopped playback");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
