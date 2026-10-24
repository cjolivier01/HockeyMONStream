#pragma once

#include "hstream/src/libs/program/AmdProgramTypes.h"

namespace hm::program {
// A frame owns its panorama allocation and can outlive the producing element.
// The stitcher only reuses an allocation after every downstream owner releases it.
class AmdPanoramaFrame {
 public:
  ~AmdPanoramaFrame();
  gpu::Image image() const;
  const GstVideoInfo& source_info() const;
  int device() const;

 private:
  friend class AmdStitcher;
  struct Impl;
  explicit AmdPanoramaFrame(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class AmdStitcher {
 public:
  static absl::StatusOr<std::unique_ptr<AmdStitcher>> Create(
      const AmdProgramProcessorConfig& config,
      size_t maximum_frames);
  ~AmdStitcher();
  absl::StatusOr<std::shared_ptr<const AmdPanoramaFrame>> Process(GstSample* left, GstSample* right);
  int width() const;
  int height() const;
  const std::string& generation() const;
  const cv::Mat& rink_mask() const;

 private:
  struct Impl;
  explicit AmdStitcher(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class AmdDetector {
 public:
  static absl::StatusOr<std::unique_ptr<AmdDetector>> Create(const detection::DetectorConfig& config);
  ~AmdDetector();
  absl::StatusOr<std::vector<detection::Proposal>> Process(const AmdPanoramaFrame& frame);

 private:
  struct Impl;
  explicit AmdDetector(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

class AmdProgramRenderer {
 public:
  static absl::StatusOr<std::unique_ptr<AmdProgramRenderer>> Create(int device);
  ~AmdProgramRenderer();
  bool healthy() const;
  absl::Status Render(
      const AmdPanoramaFrame& frame,
      const hm::BBox& crop,
      GstBuffer* output,
      const GstVideoInfo& info,
      const ProgramRenderSettings& settings,
      ProgramPreviewBoxes* preview = nullptr);

 private:
  struct Impl;
  explicit AmdProgramRenderer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::program
