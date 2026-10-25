#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>
#include "hstream/src/libs/draw_display/RinkMiniMap.h"
#include "yaml-cpp/yaml.h"

namespace hm::rink_map {
struct Settings {
  bool enabled;
  draw_display::RinkMiniMapOptions options;
  unsigned pool_frames;
};
absl::StatusOr<Settings> ResolveSettings(const YAML::Node& config);
struct Configuration {
  Settings settings;
  std::shared_ptr<const stitching::PlaybackRinkMap> map;
  int device;
};
// Creates hmrinkmap, a GPU-resident video filter. Configuration is immutable
// during a run. The selected build supplies the CUDA or HIP surface adapter.
absl::StatusOr<GstElement*> Create(const Configuration& config);

// Compact immutable metadata, attached before handing an owned AMD output to
// appsrc. Copies share only immutable observations, never writable video.
bool AttachFrame(GstBuffer* buffer, const draw_display::RinkMapFrame& frame, const GstVideoInfo& info);
const draw_display::RinkMapFrame* FindFrame(GstBuffer* buffer, const GstVideoInfo** info);
} // namespace hm::rink_map
