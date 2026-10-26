#pragma once

#include "hstream/src/apps/apps-common/RinkMaskImage.h"

#include <gst/gst.h>

#include <cstdint>
#include <memory>
#include <string>

// Presentation boundary for the shared decode/crop/timestamp replay graph.
// Source frame timestamps stay unchanged. A target may have an independent
// transport clock, but must acknowledge the original frame and seek generation
// only after that frame reaches its display.
class CameraExperimentPreviewOutput {
 public:
  struct Settings {
    std::uint64_t window_id;
    bool realtime, asynchronous;
    bool players, play, rink;
    std::shared_ptr<const hm::gpu_preview::RinkMaskImage> rink_mask;
  };
  virtual ~CameraExperimentPreviewOutput() = default;
  // Returns an unparented sink. The source graph owns the element reference.
  virtual GstElement* CreateSink(const Settings& settings, std::string* error) = 0;
  virtual void SetGeneration(std::uint64_t generation) = 0;
  virtual bool Presented(std::uint64_t* generation, GstClockTime* source_pts) = 0;
  virtual std::uint64_t PresentationOperation() const = 0;
  virtual void SetOverlays(bool players, bool play, bool rink) = 0;
  virtual void Redraw() = 0;
  virtual bool Capture(const std::string& path, std::string* error) = 0;
  // Close admission and release source-pool references before setting NULL.
  virtual void RetireSource() {}
  // Retire retained source buffers before the graph releases its source pools.
  // Called after streaming has stopped and before sink destruction.
  virtual void ReleaseSink() = 0;
};

std::shared_ptr<CameraExperimentPreviewOutput> MakeNativeCameraExperimentPreviewOutput();
