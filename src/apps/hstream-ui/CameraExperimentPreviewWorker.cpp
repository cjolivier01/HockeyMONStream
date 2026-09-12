#include "src/apps/hstream-ui/CameraExperimentPreviewWorker.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

struct CameraExperimentPreviewWorker::Impl {
  struct Command {
    Action action;
    bool close{false};
  };
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<Command> commands;
  Status status;
  std::uint64_t generation{0};
  bool stopping{false};
  std::thread thread;

  Impl() : thread([this] { run(); }) {}
  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      commands.clear();
    }
    wake.notify_one();
    thread.join();
  }

  void run() {
    CameraExperimentPreview preview;
    bool open = false;
    for (;;) {
      Command command;
      bool has_command = false;
      std::uint64_t epoch;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(30), [&] { return stopping || !commands.empty(); });
        if (stopping)
          break;
        epoch = generation;
        if (!commands.empty()) {
          command = std::move(commands.front());
          commands.pop_front();
          has_command = true;
        }
      }
      CameraExperimentPreview::Status result;
      try {
        if (has_command && command.close) {
          preview.Close();
          open = false;
        } else if (has_command) {
          if (!command.action(preview, &result.error)) {
            if (result.error.empty())
              result.error = "Could not update the experiment preview.";
          } else {
            open = true;
          }
        }
        if (result.error.empty() && open)
          result = preview.Poll();
      } catch (const std::exception& error) {
        result.error = error.what();
      } catch (...) {
        result.error = "Experiment preview failed.";
      }
      if (!result.error.empty()) {
        preview.Close();
        open = false;
        result.playing = result.seeking = false;
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (epoch != generation)
          continue; // A close invalidated this operation while it was running.
        if (!result.error.empty())
          commands.clear();
        // Keep failures visible until a new command acknowledges them.
        if (has_command || open || !result.error.empty())
          static_cast<CameraExperimentPreview::Status&>(status) = std::move(result);
        status.open = open;
        status.busy = !commands.empty();
      }
    }
    // Fence the renderer before the worker is joined and its native window dies.
    preview.Close();
  }
};

CameraExperimentPreviewWorker::CameraExperimentPreviewWorker() : impl_(std::make_unique<Impl>()) {}
CameraExperimentPreviewWorker::~CameraExperimentPreviewWorker() = default;

void CameraExperimentPreviewWorker::Submit(Action action) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commands.push_back({std::move(action), false});
    impl_->status.busy = true;
    impl_->status.frame.reset();
    impl_->status.error.clear();
  }
  impl_->wake.notify_one();
}

CameraExperimentPreviewWorker::Status CameraExperimentPreviewWorker::Poll() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->status;
}

void CameraExperimentPreviewWorker::Close() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->generation;
    impl_->commands.clear();
    impl_->commands.push_back({{}, true});
    impl_->status = {};
    impl_->status.busy = true;
  }
  impl_->wake.notify_one();
}

bool CameraExperimentPreviewWorker::Capture(const std::string& path, std::string* error) {
  const auto state = Poll();
  if (!state.open || state.busy || state.seeking) {
    if (error)
      *error = "Wait for the selected frame before capturing it.";
    return false;
  }
  auto result = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto finished = result->get_future();
  Submit([path, result](CameraExperimentPreview& preview, std::string*) {
    try {
      std::string failure;
      const bool ok = preview.Capture(path, &failure);
      result->set_value({ok, std::move(failure)});
    } catch (...) {
      result->set_exception(std::current_exception());
    }
    return true;
  });
  result.reset();
  try {
    const auto captured = finished.get();
    if (error)
      *error = captured.second;
    return captured.first;
  } catch (const std::exception& failure) {
    if (error)
      *error = failure.what();
    return false;
  }
}
