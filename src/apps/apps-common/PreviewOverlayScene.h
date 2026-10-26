#pragma once

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hm::draw_display {
class RinkMiniMap;
}

namespace hm::gpu_preview {

struct OverlayColor {
  float red{1.0F};
  float green{1.0F};
  float blue{1.0F};
  float alpha{1.0F};
};

struct OverlayPath {
  std::vector<hm::preview_overlay::Point> points;
  OverlayColor color;
  float width{1.0F};
  bool closed{false};
  bool filled{false};
};

struct PreviewOverlayScene {
  std::vector<OverlayPath> paths;
  std::optional<hm::preview_overlay::PlayCropperTransform> program_transform;
  float coordinate_width{0.0F};
  float coordinate_height{0.0F};
  // The saved field mask is an artifact of the actual stitched video
  // surface. Its dimensions can legitimately differ from DeepStream's
  // metadata coordinate space, which remains authoritative for vector
  // overlays.
  float stitched_surface_width{0.0F};
  float stitched_surface_height{0.0F};
  std::string stitched_output_generation;
  bool diagnostic_coordinates_valid{true};
  // Null means the configured map accepted this frame; other messages are stable literals.
  const char* rink_map_unavailable_reason{nullptr};
};

// Every setting is supplied by the caller's resolved preview configuration.
// Geometry describes the incoming frame before any renderer-specific letterbox.
struct PreviewOverlayOptions {
  std::string_view channel;
  unsigned negotiated_width;
  unsigned negotiated_height;
  unsigned source_width;
  unsigned source_height;
  bool show_player_tracking;
  bool show_play_tracking;
  unsigned player_analytics_layers;
  float player_joint_confidence;
};

// Reads immutable native metadata only. Both paths and commands use the returned
// coordinate space; Program geometry is applied exactly once. Clears commands
// even when metadata is missing, while preserving its reusable storage. The
// caller retains the frame and owns the optional map's per-stream history.
// No video pixel access, GPU work, file I/O, or renderer/status side effects.
PreviewOverlayScene BuildPreviewOverlayScene(
    const NvDsFrameMeta* frame,
    const PreviewOverlayOptions& options,
    draw_display::analytics::CommandList& commands,
    draw_display::RinkMiniMap* rink_mini_map);

// Artifact canvas dimensions must be exact positive uint32 values.
std::optional<std::uint32_t> ExactPreviewDimension(float dimension);

} // namespace hm::gpu_preview
