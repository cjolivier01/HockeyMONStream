#include "hstream/src/apps/apps-common/PreviewOverlayPainter.h"

#include "hstream/src/libs/draw_display/Watermark.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace p = hm::gpu_preview;
namespace a = hm::draw_display::analytics;
namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t result) {
  if (result != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(result));
}
struct Surface {
  static constexpr unsigned width = 320, height = 180, left = 8, top = 5;
  static constexpr size_t pitch = 352 * 4;
  cudaStream_t stream{};
  void* data{};
  std::vector<uint8_t> original = std::vector<uint8_t>(pitch * (height + 10), 0xa5);
  a::ImageView image;
  Surface() {
    Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    Gpu(cudaMalloc(&data, original.size()));
    image = {static_cast<uint8_t*>(data) + top * pitch + left * 4, pitch, width, height};
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        const uint32_t value = 32U | (64U << 8) | (96U << 16) | (255U << 24);
        std::memcpy(original.data() + (y + top) * pitch + (x + left) * 4, &value, 4);
      }
    Reset();
  }
  ~Surface() {
    cudaStreamSynchronize(stream);
    cudaFree(data);
    cudaStreamDestroy(stream);
  }
  void Reset() {
    Gpu(cudaMemcpyAsync(data, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
  }
  std::vector<uint8_t> Read() {
    std::vector<uint8_t> result(original.size());
    Gpu(cudaMemcpyAsync(result.data(), data, result.size(), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    for (unsigned y = 0; y < height + 10; ++y)
      for (unsigned x = 0; x < pitch; ++x)
        if (y < top || y >= top + height || x < left * 4 || x >= (left + width) * 4)
          Check(result[y * pitch + x] == original[y * pitch + x], "painter changed letterbox/pitch guard");
    return result;
  }
};
p::PreviewOverlayScene Scene() {
  p::PreviewOverlayScene scene;
  scene.coordinate_width = 640;
  scene.coordinate_height = 360;
  scene.stitched_surface_width = 1280;
  scene.stitched_surface_height = 720;
  scene.stitched_output_generation = "calibration-17";
  return scene;
}

void Mapping() {
  auto scene = Scene();
  scene.program_transform = hm::preview_overlay::PlayCropperTransform{
      1280, 720, 1920, 1080, 31, 17, 100, 90, 20, 30, 320, 160, 640, 360, 27, true, 0};
  const auto mapping = p::PreviewMaskMapping(scene, 320, 180);
  Check(mapping.has_value(), "valid Program mask mapping rejected");
  for (float x : {0.0F, 19.5F, 319.5F})
    for (float y : {0.0F, 71.5F, 179.5F}) {
      const auto expected = hm::preview_overlay::output_to_input(*scene.program_transform, {x * 2, y * 2});
      const float u = mapping->origin_u + x * mapping->step_x_u + y * mapping->step_y_u;
      const float v = mapping->origin_v + x * mapping->step_x_v + y * mapping->step_y_v;
      Check(
          std::abs(u * 1280 - expected.x) < .001F && std::abs(v * 720 - expected.y) < .001F,
          "mask inverse map does not match native transform");
    }
  scene.program_transform->crop_width = 0;
  Check(!p::PreviewMaskMapping(scene, 320, 180), "degenerate Program crop accepted");
  scene.program_transform.reset();
  scene.diagnostic_coordinates_valid = false;
  Check(!p::PreviewMaskMapping(scene, 320, 180), "missing diagnostic geometry accepted");
}

void MaskIdentity() {
  Surface surface;
  p::PreviewOverlayPainter painter;
  auto scene = Scene();
  p::PreviewPaintOptions options{"source", 640, 360, true};
  a::CommandList commands;
  auto mask = std::make_shared<p::RinkMaskImage>();
  mask->canvas_width = 1280;
  mask->canvas_height = 720;
  mask->width = 4;
  mask->height = 3;
  mask->alpha.assign(12, 255);
  painter.SetRinkMask(mask, "calibration-16");
  auto result = painter.Paint(surface.image, scene, commands, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kOk && result.mask_identity_missing && !result.mask_drawn,
      "stale calibration mask rendered");
  Check(surface.Read() == surface.original, "stale mask modified video");
  painter.SetRinkMask(mask, "calibration-17");
  scene.stitched_surface_width = 1279;
  result = painter.Paint(surface.image, scene, commands, options, surface.stream);
  Check(result.mask_identity_missing && surface.Read() == surface.original, "wrong canvas mask rendered");
  scene.stitched_surface_width = 1280;
  result = painter.Paint(surface.image, scene, commands, options, surface.stream);
  Check(result.status == p::PreviewPaintStatus::kOk && result.mask_drawn, "valid immutable mask did not render");
  const auto first = surface.Read();
  Check(first != surface.original, "mask pixels unchanged");
  surface.Reset();
  result = painter.Paint(surface.image, scene, commands, options, surface.stream);
  Check(result.mask_drawn && surface.Read() == first, "cached mask is nondeterministic");
  surface.Reset();
  options.channel = "program";
  commands.AddDisc(200, 200, 50, {1, 0, 0, 1});
  result = painter.Paint(surface.image, scene, commands, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kOk && !result.mask_drawn && surface.Read() == surface.original,
      "missing Program transform did not suppress diagnostics");
}

void FilledPathsAndUnqualifiedStrokes() {
  Surface surface;
  p::PreviewOverlayPainter painter;
  auto scene = Scene();
  p::PreviewPaintOptions options{"source", 640, 360, false};
  a::CommandList empty;
  scene.paths.push_back({{{20, 20}, {100, 20}, {100, 100}, {20, 100}}, {1, 0, 0, .5F}, 1, true, true});
  auto result = painter.Paint(surface.image, scene, empty, options, surface.stream);
  Check(result.status == p::PreviewPaintStatus::kOk, "convex filled path rejected");
  const auto actual = surface.Read();
  surface.Reset();
  a::CommandList rectangle;
  rectangle.AddFill(20, 20, 100, 100, {1, 0, 0, .5F});
  a::Compositor reference;
  Check(
      reference.RenderMapped(surface.image, 640, 360, rectangle, surface.stream).launched,
      "reference rectangle failed");
  Check(surface.Read() == actual, "polygon fan seam changed alpha coverage");
  surface.Reset();
  scene.paths.push_back({{{0, 0}, {640, 360}}, {1, 1, 1, 1}, 3, false, false});
  result = painter.Paint(surface.image, scene, empty, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kUnqualifiedStroke && surface.Read() == surface.original,
      "unqualified native GL stroke did not reject before GPU writes");
}

void RequiredWatermark() {
  Surface surface;
  p::PreviewOverlayPainter painter;
  auto scene = Scene();
  // The preview converter and output viewport intentionally differ. A painter
  // using source canvas/output dimensions for the mark would fail this oracle.
  p::PreviewPaintOptions options{"stitched", 800, 450, false};
  a::CommandList empty;
  auto result = painter.Paint(surface.image, scene, empty, options, surface.stream);
  Check(result.status == p::PreviewPaintStatus::kOk, "mandatory watermark failed");
  const auto actual = surface.Read();
  Check(actual != surface.original, "mandatory watermark invisible");
  surface.Reset();
  a::CommandList mark;
  Check(hm::draw_display::AppendWatermark(&mark, 640, 360, .8F), "mark reference failed");
  a::Compositor reference;
  Check(reference.RenderMapped(surface.image, 640, 360, mark, surface.stream).launched, "reference mark failed");
  Check(surface.Read() == actual, "watermark negotiated geometry differs from desktop");
  a::CommandList overflow;
  for (unsigned i = 0; i < a::kMaximumCommands; ++i)
    Check(overflow.AddFill(0, 0, 640, 360, {0, 1, 0, 1}), "capacity fixture rejected");
  surface.Reset();
  result = painter.Paint(surface.image, scene, overflow, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kOverlaySuppressed && result.overlay_status == a::RenderStatus::kCapacity,
      "optional command overflow not reported");
  Check(surface.Read() == actual, "optional overlay capacity suppressed required watermark");

  // 4097 rectangles exceed the 8192-triangle command budget before rendering.
  // This CPU preparation limit must have the same optional-layer semantics as
  // the GPU tile/reference limit checked above.
  scene.paths.assign(
      a::kMaximumCommands / 2 + 1,
      p::OverlayPath{{{20, 20}, {100, 20}, {100, 100}, {20, 100}}, {0, 1, 0, 1}, 1, true, true});
  surface.Reset();
  result = painter.Paint(surface.image, scene, empty, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kOverlaySuppressed && result.overlay_status == a::RenderStatus::kCapacity,
      "optional path preparation overflow not reported");
  Check(surface.Read() == actual, "optional path preparation capacity suppressed required watermark");

  scene.paths.push_back({{{0, 0}, {640, 360}}, {1, 1, 1, 1}, 3, false, false});
  surface.Reset();
  result = painter.Paint(surface.image, scene, empty, options, surface.stream);
  Check(
      result.status == p::PreviewPaintStatus::kUnqualifiedStroke, "path overflow hid a subsequent unqualified stroke");
  Check(surface.Read() == surface.original, "stroke after path overflow did not reject before GPU writes");
}
} // namespace
int main() {
  try {
    Gpu(cudaSetDevice(0));
    Mapping();
    MaskIdentity();
    FilledPathsAndUnqualifiedStrokes();
    RequiredWatermark();
    std::cout
        << "preview_painter immutable_mask=1 generation_canvas_fenced=1 inverse_program=1 missing_transform_closed=1 "
           "filled_polygon_seams=1 unqualified_strokes_rejected=1 watermark_required=1 letterbox_guards=1\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
