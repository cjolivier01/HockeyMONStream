#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cupano/pano/blendMode.h"
#include "hstream/src/libs/detection/HipDetector.h"
#include "hstream/src/libs/program/ProgramCalibration.h"
#include "hstream/src/libs/program/ProgramPreviewBoxes.h"
#include "hstream/src/libs/program/ProgramRenderSettings.h"
#include "hstream/src/libs/tracking/ProgramTracking.h"

namespace hm::program {
// Fully resolved settings. This owner supplies no application defaults. The
// caller selects the same original camera sources used by the saved calibration.
struct AmdProgramProcessorConfig {
  std::filesystem::path game_directory;
  detection::DetectorConfig detector;
  YAML::Node camera;
  tracking::TrackerConfig tracker;
  pano::BlendSettings blend;
  bool minimize_blend, compact_workspace;
  float raise_center_ratio, lower_bottom_ratio;
  ProgramCalibrationRequest calibration;
};

struct AmdProgramFrame {
  size_t detection_count;
  tracking::ProgramObservation observation;
  hm::BBox crop;
};

// Owns the GPU panorama, inference session and native camera/tracker state for
// one serialized playback owner. Input pairing/timestamps and decoder/sink
// lifecycles belong to that owner. No per-frame file work or video readback.
// Only saved, unrotated calibration is currently supported; other generations
// fail preparation instead of silently changing the saved view.
class AmdProgramProcessor {
 public:
  static absl::StatusOr<std::unique_ptr<AmdProgramProcessor>> Create(const AmdProgramProcessorConfig& config);
  ~AmdProgramProcessor();
  AmdProgramProcessor(const AmdProgramProcessor&) = delete;
  AmdProgramProcessor& operator=(const AmdProgramProcessor&) = delete;

  // Borrows paired linear VA DMA-BUF samples. All reads finish before return,
  // including failures. Caps are fixed by the first pair and cannot change.
  absl::StatusOr<AmdProgramFrame> Process(uint64_t sequence, GstSample* left, GstSample* right);
  // Render the last successful frame to an exclusively owned VA output. May be
  // called repeatedly for different output sizes. All writes finish on return.
  // Timestamps/metadata are assigned by the caller. Output must preserve the
  // input primaries/transfer; the renderer converts matrix/range only.
  // Optional diagnostic boxes apply only to this owned output, never the source
  // panorama or another render (e.g. an archive). Metadata remains on the CPU.
  absl::Status Render(
      GstBuffer* output,
      const GstVideoInfo& info,
      const ProgramRenderSettings& settings,
      ProgramPreviewBoxes* preview = nullptr);
  // Discard the last frame and camera/object history after a seek. Compiled
  // inference and stitch allocations survive; object IDs are never reused.
  // A processing/GPU failure retires the owner and requires reconstruction.
  absl::Status Reset();

  int width() const;
  int height() const;
  const std::string& generation() const;

 private:
  struct Impl;
  explicit AmdProgramProcessor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::program
