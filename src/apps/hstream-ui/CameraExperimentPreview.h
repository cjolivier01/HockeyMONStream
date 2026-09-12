#pragma once

#include "hstream/src/libs/playtracker_replay/ReplaySession.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Owns a small decode/crop/render graph. It never runs inference or camera
// policy, and keeps only a few GPU video buffers regardless of clip length.
class CameraExperimentPreview {
 public:
  using Frame = hm::playtracker_replay::Frame;
  struct Status {
    bool playing{false};
    bool seeking{false};
    std::optional<std::size_t> frame;
    std::string error;
  };

  CameraExperimentPreview();
  ~CameraExperimentPreview();
  CameraExperimentPreview(const CameraExperimentPreview&) = delete;
  CameraExperimentPreview& operator=(const CameraExperimentPreview&) = delete;

  bool Open(
      const hm::playtracker_replay::MediaBinding& media,
      unsigned canvas_width,
      unsigned canvas_height,
      std::uint64_t window_id,
      double left_rotation,
      double right_rotation,
      std::string* error);
  bool SetTrajectory(
      std::shared_ptr<const std::vector<Frame>> frames,
      std::uint64_t end_pts_ns,
      std::string* error,
      bool play = false,
      std::size_t index = 0);
  bool Seek(std::size_t index, bool play, std::string* error);
  void Pause();
  void SetLoop(bool loop);
  Status Poll();
  bool Capture(const std::string& path, std::string* error);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
