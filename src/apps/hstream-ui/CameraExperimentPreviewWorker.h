#pragma once

#include "src/apps/hstream-ui/CameraExperimentPreview.h"

#include <functional>
#include <memory>

// All graph operations, including Poll's state changes and teardown, run on
// one worker. Qt reads a short-lived status snapshot and never takes a graph lock.
class CameraExperimentPreviewWorker {
 public:
  using Action = std::function<bool(CameraExperimentPreview&, std::string*)>;
  struct Status : CameraExperimentPreview::Status {
    bool busy{false};
    bool open{false};
  };
  CameraExperimentPreviewWorker();
  ~CameraExperimentPreviewWorker();
  void Submit(Action action);
  Status Poll() const;
  void Close();
  bool Capture(const std::string& path, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
