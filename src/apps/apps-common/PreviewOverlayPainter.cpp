#include "PreviewOverlayPainter.h"

#include "hstream/src/libs/draw_display/Watermark.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hm::gpu_preview {
namespace a = draw_display::analytics;
namespace {
bool Positive(float value) {
  return std::isfinite(value) && value > 0 && value <= 1000000;
}
bool ValidImage(const a::ImageView& image) {
  return image.format == a::PixelFormat::kRgba8 && image.data && image.width && image.height && image.width <= 16384 &&
      image.height <= 16384 && image.pitch >= size_t(image.width) * 4 && image.pitch % 4 == 0 &&
      reinterpret_cast<uintptr_t>(image.data) % 4 == 0 &&
      image.pitch <= std::numeric_limits<size_t>::max() / image.height;
}
bool ValidMask(const RinkMaskImage& mask, const PreviewOverlayScene& scene) {
  const auto width = ExactPreviewDimension(scene.stitched_surface_width);
  const auto height = ExactPreviewDimension(scene.stitched_surface_height);
  return width && height && rink_mask_dimensions_match(mask, *width, *height) && mask.width && mask.height &&
      mask.width <= kMaximumRinkMaskDimension && mask.height <= kMaximumRinkMaskDimension &&
      uint64_t(mask.width) * mask.height <= kMaximumRinkMaskTextureBytes &&
      mask.alpha.size() == size_t(mask.width) * mask.height;
}
} // namespace

std::optional<draw_display::AlphaMaskMapping> PreviewMaskMapping(
    const PreviewOverlayScene& scene,
    unsigned output_width,
    unsigned output_height) {
  if (!scene.diagnostic_coordinates_valid || !output_width || !output_height ||
      !Positive(scene.stitched_surface_width) || !Positive(scene.stitched_surface_height) ||
      !Positive(scene.coordinate_width) || !Positive(scene.coordinate_height))
    return std::nullopt;
  using hm::preview_overlay::Point;
  Point origin{0, 0}, right{scene.stitched_surface_width, 0}, bottom{0, scene.stitched_surface_height};
  if (scene.program_transform) {
    const auto& transform = *scene.program_transform;
    if (!Positive(transform.crop_width) || !Positive(transform.crop_height) ||
        transform.output_width != scene.coordinate_width || transform.output_height != scene.coordinate_height ||
        transform.input_width != scene.stitched_surface_width ||
        transform.input_height != scene.stitched_surface_height)
      return std::nullopt;
    origin = hm::preview_overlay::output_to_input(transform, {0, 0});
    right = hm::preview_overlay::output_to_input(transform, {scene.coordinate_width, 0});
    bottom = hm::preview_overlay::output_to_input(transform, {0, scene.coordinate_height});
  }
  const draw_display::AlphaMaskMapping mapping{
      origin.x / scene.stitched_surface_width,
      origin.y / scene.stitched_surface_height,
      (right.x - origin.x) / scene.stitched_surface_width / output_width,
      (right.y - origin.y) / scene.stitched_surface_height / output_width,
      (bottom.x - origin.x) / scene.stitched_surface_width / output_height,
      (bottom.y - origin.y) / scene.stitched_surface_height / output_height,
  };
  for (const float value :
       {mapping.origin_u, mapping.origin_v, mapping.step_x_u, mapping.step_x_v, mapping.step_y_u, mapping.step_y_v})
    if (!std::isfinite(value) || std::abs(value) > 1000000)
      return std::nullopt;
  return mapping;
}

struct PreviewOverlayPainter::State {
  a::Compositor paths, analytics, watermark;
  a::CommandList path_commands, watermark_commands;
  std::shared_ptr<const RinkMaskImage> mask, uploaded_mask;
  std::string generation;
  uint8_t* mask_device{nullptr};
  bool bound{false}, failed{false};
  int device{-1};
  cudaStream_t stream{nullptr};
  ~State() {
    if (mask_device)
      cudaFree(mask_device);
  }
};

PreviewOverlayPainter::PreviewOverlayPainter() : state_(std::make_unique<State>()) {}
PreviewOverlayPainter::~PreviewOverlayPainter() = default;

void PreviewOverlayPainter::SetRinkMask(std::shared_ptr<const RinkMaskImage> image, std::string generation) {
  state_->mask = std::move(image);
  state_->generation = std::move(generation);
}

PreviewPaintResult PreviewOverlayPainter::Paint(
    const a::ImageView& image,
    const PreviewOverlayScene& scene,
    const a::CommandList& analytics,
    const PreviewPaintOptions& options,
    cudaStream_t stream) {
  PreviewPaintResult result;
  auto& state = *state_;
  auto invalid = [&] {
    result.status = PreviewPaintStatus::kInvalidArgument;
    return result;
  };
  auto gpu_error = [&](cudaError_t error) {
    state.failed = true;
    result.status = PreviewPaintStatus::kGpuError;
    result.gpu_error = error;
    return result;
  };
  if (!ValidImage(image) || !Positive(scene.coordinate_width) || !Positive(scene.coordinate_height) ||
      !options.negotiated_width || !options.negotiated_height || options.channel.empty())
    return invalid();
  if (state.failed)
    return gpu_error(cudaErrorUnknown);
  int device = -1;
  auto error = cudaGetDevice(&device);
  if (error != cudaSuccess)
    return gpu_error(error);
  if (state.bound && (device != state.device || stream != state.stream))
    return invalid();
  state.bound = true;
  state.device = device;
  state.stream = stream;

  const bool diagnostics =
      scene.diagnostic_coordinates_valid && (options.channel != "program" || scene.program_transform.has_value());
  state.path_commands.Clear();
  // Metadata collection emits convex rectangles, circle fans and arrowheads.
  // Reject all unqualified strokes before any GPU pass, rather than silently
  // replacing Qt's display-pixel aliased lines with source-space rounded lines.
  if (diagnostics) {
    for (const auto& path : scene.paths) {
      if (path.points.size() < (path.filled ? 3U : 2U) || path.color.alpha <= 0)
        continue;
      if (!path.filled) {
        result.status = PreviewPaintStatus::kUnqualifiedStroke;
        return result;
      }
      const auto& first = path.points.front();
      for (size_t i = 2; i < path.points.size(); ++i) {
        const auto& previous = path.points[i - 1];
        const auto& next = path.points[i];
        if (!state.path_commands.AddTriangle(
                first.x,
                first.y,
                previous.x,
                previous.y,
                next.x,
                next.y,
                {path.color.red, path.color.green, path.color.blue, path.color.alpha}))
          return invalid();
      }
    }
  }
  state.watermark_commands.Clear();
  if (options.channel == "stitched" &&
      !draw_display::AppendWatermark(
          &state.watermark_commands,
          scene.coordinate_width,
          scene.coordinate_height,
          std::min(
              scene.coordinate_width / options.negotiated_width, scene.coordinate_height / options.negotiated_height)))
    return invalid();

  if (options.show_rink_mask && diagnostics) {
    const auto mapping = PreviewMaskMapping(scene, image.width, image.height);
    if (!mapping || !state.mask || state.generation.empty() || state.generation != scene.stitched_output_generation ||
        !ValidMask(*state.mask, scene)) {
      result.mask_identity_missing = true;
    } else {
      if (state.uploaded_mask != state.mask) {
        if (state.mask_device) {
          error = cudaFree(state.mask_device);
          state.mask_device = nullptr;
          if (error != cudaSuccess)
            return gpu_error(error);
        }
        error = cudaMalloc(reinterpret_cast<void**>(&state.mask_device), state.mask->alpha.size());
        if (error != cudaSuccess)
          return gpu_error(error);
        // Retained immutable host image owns this infrequent H2D upload until
        // the caller completes its stream. Steady-state frames upload nothing.
        state.uploaded_mask = state.mask;
        error = cudaMemcpyAsync(
            state.mask_device, state.mask->alpha.data(), state.mask->alpha.size(), cudaMemcpyHostToDevice, stream);
        if (error != cudaSuccess)
          return gpu_error(error);
      }
      error = draw_display::DrawAlphaMask(
          image,
          {state.mask_device, state.mask->width, state.mask->width, state.mask->height},
          *mapping,
          {0, 1, 0, .28F},
          stream);
      if (error != cudaSuccess)
        return gpu_error(error);
      result.mask_drawn = true;
    }
  }
  auto render = [&](a::Compositor& compositor, const a::CommandList& commands) {
    const auto rendered =
        compositor.RenderMapped(image, scene.coordinate_width, scene.coordinate_height, commands, stream);
    if (rendered.status != a::RenderStatus::kOk) {
      result.status = rendered.status == a::RenderStatus::kCudaError ? PreviewPaintStatus::kGpuError
                                                                     : PreviewPaintStatus::kOverlaySuppressed;
      result.overlay_status = rendered.status;
      result.gpu_error = rendered.cuda_error;
      state.failed |= result.status == PreviewPaintStatus::kGpuError;
    }
    return rendered.status == a::RenderStatus::kOk;
  };
  if (diagnostics && (!render(state.paths, state.path_commands) || !render(state.analytics, analytics))) {
    // A required watermark has independent commands/resources and still draws
    // after optional capacity/font/busy suppression, unless the GPU failed.
    if (result.status == PreviewPaintStatus::kGpuError)
      return result;
  }
  render(state.watermark, state.watermark_commands);
  return result;
}

} // namespace hm::gpu_preview
