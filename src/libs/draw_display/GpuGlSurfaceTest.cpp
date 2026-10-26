#include "hstream/src/libs/draw_display/GpuGlSurface.h"

#include <EGL/egl.h>
#include <GL/gl.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace d = hm::draw_display;
namespace a = d::analytics;
namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t error) {
  if (error != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(error));
}
void PaintOkay(d::GpuGlResult result) {
  if (result.status != d::GpuGlStatus::kOk)
    throw std::runtime_error(
        "interop status=" + std::to_string(int(result.status)) + " gpu=" + cudaGetErrorString(result.gpu_error) +
        " egl=" + std::to_string(result.egl_error) + " gl=" + std::to_string(result.gl_error));
}
struct Surface {
  unsigned width, height;
  size_t pitch;
  void* allocation{};
  cudaStream_t stream{};
  a::ImageView image;
  std::vector<uint8_t> original;
  Surface(unsigned width, unsigned height)
      : width(width), height(height), pitch((width + 16) * 4), original(pitch * (height + 4), 0xa5) {
    Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    Gpu(cudaMalloc(&allocation, original.size()));
    image = {static_cast<uint8_t*>(allocation) + pitch * 2 + 16, pitch, width, height};
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        const uint32_t value = ((x * 7) & 255U) | (((y * 11) & 255U) << 8) | (64U << 16) | (255U << 24);
        std::memcpy(original.data() + (y + 2) * pitch + 16 + x * 4, &value, 4);
      }
    Reset();
  }
  ~Surface() {
    cudaStreamSynchronize(stream);
    cudaFree(allocation);
    cudaStreamDestroy(stream);
  }
  void Reset() {
    Gpu(cudaMemcpyAsync(allocation, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
  }
  std::vector<uint8_t> Read() {
    std::vector<uint8_t> result(original.size());
    Gpu(cudaMemcpyAsync(result.data(), allocation, result.size(), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    for (unsigned y = 0; y < height + 4; ++y)
      for (size_t x = 0; x < pitch; ++x)
        if (y < 2 || y >= height + 2 || x < 16 || x >= 16 + width * 4)
          Check(result[y * pitch + x] == original[y * pitch + x], "interop changed letterbox/pitch guard");
    return result;
  }
};
bool Paths() {
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(.8F, .1F, .3F, .5F);
  glLineWidth(3);
  glBegin(GL_LINE_LOOP);
  glVertex2f(-.75F, .75F);
  glVertex2f(.5F, .75F);
  glVertex2f(.5F, -.25F);
  glVertex2f(-.75F, -.25F);
  glEnd();
  glColor4f(.1F, .8F, .4F, .7F);
  glLineWidth(2);
  glBegin(GL_LINE_STRIP);
  glVertex2f(-1.4F, .4F);
  glVertex2f(.25F, -.75F);
  glVertex2f(.75F, .9F);
  glEnd();
  return true;
}
void PixelsAndOwnership() {
  Surface surface(96, 72);
  d::GpuGlSurface first, second;
  PaintOkay(first.Prepare(surface.image, surface.stream));
  Check(surface.Read() == surface.original && first.counters().frames == 0, "preflight touched video pixels");
  PaintOkay(first.Paint(surface.image, surface.stream, [] { return true; }));
  Check(surface.Read() == surface.original, "interop flips/resamples unchanged video");
  PaintOkay(first.Paint(surface.image, surface.stream, Paths));
  const auto painted = surface.Read();
  Check(painted != surface.original, "shared GL pass changed no pixels");
  surface.Reset();
  PaintOkay(second.Paint(surface.image, surface.stream, Paths));
  Check(surface.Read() == painted, "independent contexts draw different pixels");
  surface.Reset();
  PaintOkay(first.Paint(surface.image, surface.stream, Paths));
  Check(surface.Read() == painted, "reused context changed pixels");
  Check(
      first.counters().texture_allocations == 2 && first.counters().texture_bytes == 96 * 72 * 8 &&
          first.counters().copied_bytes == 3 * 96 * 72 * 8,
      "unbounded texture allocation or transfer accounting");
  auto invalid = surface.image;
  invalid.width += 1;
  Check(
      first.Paint(invalid, surface.stream, Paths).status == d::GpuGlStatus::kInvalidArgument,
      "geometry mutation accepted");
  Check(
      first.Paint(surface.image, nullptr, Paths).status == d::GpuGlStatus::kInvalidArgument,
      "stream mutation accepted");
  Check(surface.Read() == painted, "invalid request changed output");
  surface.Reset();
  Check(
      second.Paint(
                surface.image,
                surface.stream,
                [] {
                  Paths();
                  return false;
                })
              .status == d::GpuGlStatus::kCallbackFailed,
      "callback failure ignored");
  Check(surface.Read() == surface.original, "failed callback published partial pixels");
  Check(
      second.Paint(surface.image, surface.stream, Paths).status == d::GpuGlStatus::kInvalidArgument,
      "failed context reused");
  {
    d::GpuGlSurface transient;
    PaintOkay(transient.Paint(surface.image, surface.stream, Paths));
    surface.Read();
  }
  surface.Reset();
  PaintOkay(first.Paint(surface.image, surface.stream, Paths));
  Check(surface.Read() == painted, "destroying one owner terminated another EGL context");
  surface.Reset();
  d::GpuGlResult migrated{d::GpuGlStatus::kGpuError};
  int device = -1;
  Gpu(cudaGetDevice(&device));
  std::thread producer([&] {
    if (cudaSetDevice(device) != cudaSuccess)
      return;
    migrated = first.Paint(surface.image, surface.stream, Paths);
  });
  producer.join();
  PaintOkay(migrated);
  Check(surface.Read() == painted, "serialized producer thread migration changed pixels");
  PaintOkay(first.Paint(surface.image, surface.stream, [&] {
    Check(
        first.Prepare(surface.image, surface.stream).status == d::GpuGlStatus::kInvalidArgument,
        "reentrant resource access accepted");
    const auto caller_context = eglGetCurrentContext();
    const auto caller_display = eglGetCurrentDisplay();
    {
      d::GpuGlSurface nested;
      PaintOkay(nested.Prepare(surface.image, surface.stream));
      Check(
          eglGetCurrentContext() == caller_context && eglGetCurrentDisplay() == caller_display,
          "nested owner did not restore EGL context");
    }
    Check(
        eglGetCurrentContext() == caller_context && eglGetCurrentDisplay() == caller_display,
        "nested owner destruction did not restore EGL context");
    return true;
  }));
  surface.Read();
  std::cout << "gpu_gl pixels_stay_on_device=1 row_orientation=1 rgba_opaque=1 pitch_guards=1 contexts_independent=1 "
               "geometry_stream_bound=1 failed_callback_unpublished=1 thread_migration=1 concurrent_rejected=1 "
               "egl_context_restored=1 textures=2\n";
}
void Unavailable() {
  Surface surface(96, 72);
  d::GpuGlSurface renderer;
  const auto result = renderer.Prepare(surface.image, surface.stream);
  Check(result.status == d::GpuGlStatus::kUnavailable, "expected unavailable device interop");
  Check(surface.Read() == surface.original, "unavailable interop changed preview pixels");
  bool called = false;
  Check(
      renderer.Paint(
                  surface.image,
                  surface.stream,
                  [&] {
                    called = true;
                    return true;
                  })
                  .status == d::GpuGlStatus::kInvalidArgument &&
          !called,
      "unavailable interop invoked drawing callback");
  Check(surface.Read() == surface.original, "retired interop changed preview pixels");
  std::cout << "gpu_gl unavailable=1 unchanged_output=1 callback_not_invoked=1 gpu="
            << cudaGetErrorString(result.gpu_error) << '\n';
}
void Benchmark() {
  Surface surface(1920, 1080);
  d::GpuGlSurface renderer;
  for (int i = 0; i < 8; ++i) {
    PaintOkay(renderer.Paint(surface.image, surface.stream, Paths));
    Gpu(cudaStreamSynchronize(surface.stream));
  }
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) {
    PaintOkay(renderer.Paint(surface.image, surface.stream, Paths));
    Gpu(cudaStreamSynchronize(surface.stream));
  }
  const auto elapsed =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100;
  a::CommandList commands;
  commands.AddRectangle(240, 135, 1440, 675, 3, {.8F, .1F, .3F, .5F});
  commands.AddLine(-384, 324, 1200, 945, 2, {.1F, .8F, .4F, .7F});
  commands.AddLine(1200, 945, 1680, 54, 2, {.1F, .8F, .4F, .7F});
  a::Compositor portable;
  for (int i = 0; i < 8; ++i) {
    Check(portable.Render(surface.image, commands, surface.stream).launched, "portable warmup");
    Gpu(cudaStreamSynchronize(surface.stream));
  }
  const auto portable_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; ++i) {
    Check(portable.Render(surface.image, commands, surface.stream).launched, "portable benchmark");
    Gpu(cudaStreamSynchronize(surface.stream));
  }
  const auto portable_elapsed =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - portable_start).count() / 100;
  std::cout << "gpu_gl_1080p wall_us_mean=" << elapsed << " portable_us_mean=" << portable_elapsed
            << " texture_bytes=" << renderer.counters().texture_bytes << " d2d_bytes_per_frame=" << 1920 * 1080 * 8
            << " loops=100 pixel_readbacks=0\n";
}
} // namespace
int main(int argc, char** argv) {
  try {
    Gpu(cudaSetDevice(0));
    if (argc == 2 && std::string(argv[1]) == "--expect-interop-unavailable") {
      Unavailable();
      return 0;
    }
    PixelsAndOwnership();
    if (argc == 2 && std::string(argv[1]) == "--benchmark")
      Benchmark();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
