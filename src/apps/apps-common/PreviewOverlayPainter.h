#pragma once

#include "hstream/src/apps/apps-common/PreviewOverlayScene.h"
#include "hstream/src/apps/apps-common/RinkMaskImage.h"
#include "hstream/src/libs/draw_display/AlphaMaskOverlay.h"

#include <memory>

namespace hm::gpu_preview {

struct PreviewPaintOptions {
  std::string_view channel;
  // The desktop conversion dimensions, independently of the source canvas and
  // encoded viewport. They determine the shared Stitched watermark size.
  unsigned negotiated_width, negotiated_height;
  bool show_rink_mask;
};

enum class PreviewPaintStatus { kOk, kInvalidArgument, kInteropUnavailable, kOverlaySuppressed, kGpuError };
struct PreviewPaintResult {
  PreviewPaintStatus status{PreviewPaintStatus::kOk};
  draw_display::analytics::RenderStatus overlay_status{draw_display::analytics::RenderStatus::kOk};
  cudaError_t gpu_error{cudaSuccess};
  unsigned egl_error{0}, gl_error{0};
  bool mask_drawn{false};
  bool mask_identity_missing{false};
};

// Computes the same inverse mapping as Qt's rink-mask texture pass. A missing
// Program transform/invalid canvas must never show a mask from another space.
std::optional<draw_display::AlphaMaskMapping> PreviewMaskMapping(
    const PreviewOverlayScene& scene,
    unsigned output_width,
    unsigned output_height);

// Independently owned opaque RGBA preview subimage only: never pass the producer's tee
// frame, archive surface, or letterbox bars. One device and stream; serialized
// calls may migrate producer threads when the selected route changes.
// Caller completes that stream before the next Paint, mask replacement, and
// destruction; PreviewGpuSurface already establishes that boundary. Static
// masks upload once per immutable asset, without reading back any video pixels.
// Native diagnostic paths use the unchanged desktop GL pass through GPU-only
// interop. Unavailable interop fails before any preview writes. Optional path
// capacity suppression preserves the mandatory Stitched watermark.
class PreviewOverlayPainter {
 public:
  PreviewOverlayPainter();
  ~PreviewOverlayPainter();
  PreviewOverlayPainter(const PreviewOverlayPainter&) = delete;
  PreviewOverlayPainter& operator=(const PreviewOverlayPainter&) = delete;

  // Caller must acquire this immutable artifact through visit_current_field_mask
  // and load_rink_mask_png. Live-frame identity
  // and actual stitched canvas dimensions are checked again for every frame.
  void SetRinkMask(std::shared_ptr<const RinkMaskImage> image, std::string generation);
  PreviewPaintResult Paint(
      const draw_display::analytics::ImageView& owned_video,
      const PreviewOverlayScene& scene,
      const draw_display::analytics::CommandList& analytics,
      const PreviewPaintOptions& options,
      cudaStream_t stream);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace hm::gpu_preview
