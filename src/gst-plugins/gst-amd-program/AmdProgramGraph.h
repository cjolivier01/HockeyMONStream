#pragma once

#include <functional>
#include "hstream/src/libs/program/AmdProgramTypes.h"

namespace hm::program {
// Componentized processing and rendering graphs. The application owns source
// pairing, audio and publication. Each graph element owns its stage resources.
// This first integration admits one complete pair at a time; GstMeta owns the
// GPU frame across every element and each independent output render.
class AmdProgramGraph {
 public:
  static absl::StatusOr<std::unique_ptr<AmdProgramGraph>> Create(
      const AmdProgramProcessorConfig& config,
      size_t maximum_frames,
      uint64_t timeout_ns,
      uint64_t poll_ns,
      std::function<bool()> cancelled);
  ~AmdProgramGraph();
  absl::StatusOr<AmdProgramFrame> Process(
      uint64_t sequence,
      GstSample* left,
      GstSample* right,
      GstClockTime time,
      GstClockTime duration);
  // Takes ownership of output on every path; returns the rendered owned buffer.
  absl::StatusOr<GstBuffer*> Render(
      GstBuffer* output,
      const GstVideoInfo& info,
      const ProgramRenderSettings& settings,
      const ProgramPreviewBoxes* preview = nullptr);
  absl::Status Reset();
  int width() const;
  int height() const;
  const std::string& generation() const;

 private:
  struct Impl;
  explicit AmdProgramGraph(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::program
