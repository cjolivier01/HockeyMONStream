#include "PreviewOverlayPainter.h"

#include "PreviewOverlayGlPaths.h"
#include "hstream/src/libs/draw_display/GpuGlSurface.h"
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
  a::Compositor analytics, watermark;
  a::CommandList watermark_commands;
  std::unique_ptr<draw_display::GpuGlSurface> gl_paths;
  unsigned gl_width{0}, gl_height{0};
  std::shared_ptr<const RinkMaskImage> mask, uploaded_mask;
  std::string generation;
  uint8_t* mask_device{nullptr};
  bool bound{false}, failed{false}, interop_unavailable{false};
  draw_display::GpuGlResult interop_failure;
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
  bool path_capacity_exceeded = false;
  size_t path_commands = 0;
  // Validate the complete scene before any GPU writes, even after capacity
  // exhaustion. Count the same triangles/segments without allocating commands;
  // all paths share one GL pass, preserving mixed fill/stroke draw order.
  if (diagnostics) {
    for (const auto& path : scene.paths) {
      if (path.points.size() < (path.filled ? 3U : 2U))
        continue;
      for (float color : {path.color.red, path.color.green, path.color.blue, path.color.alpha})
        if (!std::isfinite(color) || color < 0 || color > 1)
          return invalid();
      if (!std::isfinite(path.width) || path.width < 0 || path.width > 16384)
        return invalid();
      for (const auto& point : path.points)
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > 1000000 ||
            std::abs(point.y) > 1000000)
          return invalid();
      const auto count = path.points.size() - (path.filled ? 2U : (path.closed ? 0U : 1U));
      if (count > a::kMaximumCommands - path_commands)
        path_capacity_exceeded = true;
      else if (!path_capacity_exceeded)
        path_commands += count;
    }
  }
  if (path_capacity_exceeded) {
    result.status = PreviewPaintStatus::kOverlaySuppressed;
    result.overlay_status = a::RenderStatus::kCapacity;
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

  const bool draw_paths = diagnostics && path_commands && !path_capacity_exceeded;
  auto gl_failure = [&](const draw_display::GpuGlResult& failure) {
    result.status = failure.status == draw_display::GpuGlStatus::kUnavailable ? PreviewPaintStatus::kInteropUnavailable
                                                                              : PreviewPaintStatus::kGpuError;
    result.gpu_error = failure.gpu_error;
    result.egl_error = failure.egl_error;
    result.gl_error = failure.gl_error;
    state.failed = result.status == PreviewPaintStatus::kGpuError;
    return result;
  };
  if (draw_paths) {
    if (state.interop_unavailable)
      return gl_failure(state.interop_failure);
    if (!state.gl_paths || state.gl_width != image.width || state.gl_height != image.height) {
      // The caller completed the previous frame's stream before this Paint.
      state.gl_paths = std::make_unique<draw_display::GpuGlSurface>();
      state.gl_width = image.width;
      state.gl_height = image.height;
    }
    const auto prepared = state.gl_paths->Prepare(image, stream);
    if (prepared.status != draw_display::GpuGlStatus::kOk) {
      state.interop_unavailable = prepared.status == draw_display::GpuGlStatus::kUnavailable;
      state.interop_failure = prepared;
      return gl_failure(prepared);
    }
  }

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
  if (draw_paths) {
    const auto painted = state.gl_paths->Paint(image, stream, [&] {
      DrawPreviewOverlayGlPaths(scene);
      return true;
    });
    if (painted.status != draw_display::GpuGlStatus::kOk)
      return gl_failure(painted);
  }
  if (diagnostics && !render(state.analytics, analytics)) {
    // A required watermark has independent commands/resources and still draws
    // after optional capacity/font/busy suppression, unless the GPU failed.
    if (result.status == PreviewPaintStatus::kGpuError)
      return result;
  }
  render(state.watermark, state.watermark_commands);
  return result;
}

} // namespace hm::gpu_preview
