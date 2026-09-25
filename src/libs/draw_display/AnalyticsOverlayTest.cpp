#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

std::atomic<uint64_t> allocations{0};
void* operator new(size_t size) {
  ++allocations;
  if (void* p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, size_t) noexcept {
  std::free(p);
}

namespace a = hm::draw_display::analytics;
namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t error) {
  if (error != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(error));
}
struct Stream {
  cudaStream_t value{};
  Stream() {
    Cuda(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking));
  }
  ~Stream() {
    cudaStreamSynchronize(value);
    cudaStreamDestroy(value);
  }
};
struct Image {
  uint8_t* allocation{};
  a::ImageView view;
  std::vector<uint8_t> original;
  Image(unsigned width, unsigned height, a::PixelFormat format) {
    view = {nullptr, size_t(width + 19) * 4, width, height, format};
    original.resize(view.pitch * height + 512, 0xa5);
    Cuda(cudaMalloc(reinterpret_cast<void**>(&allocation), original.size()));
    view.data = allocation + 256;
    for (unsigned y = 0; y < height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        const uint32_t pixel = format == a::PixelFormat::kRgba8
            ? ((x * 13U + 17) & 255) | (((y * 7U + 11) & 255) << 8) | (83U << 16) | (((x + y) % 4 * 85U) << 24)
            : ((x * 13U + 17) & 1023) | (((y * 7U + 11) & 1023) << 10) | (317U << 20) | (((x + y) % 4) << 30);
        std::memcpy(original.data() + 256 + y * view.pitch + x * 4, &pixel, 4);
      }
    }
  }
  ~Image() {
    cudaFree(allocation);
  }
  void Reset(cudaStream_t stream) {
    Cuda(cudaMemcpyAsync(allocation, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
  }
  std::vector<uint8_t> Read(cudaStream_t stream) {
    std::vector<uint8_t> result(original.size());
    Cuda(cudaMemcpyAsync(result.data(), allocation, result.size(), cudaMemcpyDeviceToHost, stream));
    Cuda(cudaStreamSynchronize(stream));
    return result;
  }
};
float Coverage(const a::detail::Command& c, float x, float y) {
  using a::detail::Kind;
  if (c.kind == Kind::kLine || c.kind == Kind::kDisc) {
    float t = 0;
    const float dx = c.x1 - c.x0, dy = c.y1 - c.y0;
    if (c.kind == Kind::kLine && dx * dx + dy * dy > 0)
      t = std::clamp(((x - c.x0) * dx + (y - c.y0) * dy) / (dx * dx + dy * dy), 0.0F, 1.0F);
    return std::clamp(c.radius + 0.5F - std::hypot(x - c.x0 - t * dx, y - c.y0 - t * dy), 0.0F, 1.0F);
  }
  if (x < c.x0 || x >= c.x1 || y < c.y0 || y >= c.y1)
    return 0;
  if (c.kind == Kind::kRectangle)
    return x < c.x0 + c.radius || x >= c.x1 - c.radius || y < c.y0 + c.radius || y >= c.y1 - c.radius;
  return c.kind == Kind::kFill ? 1 : 0;
}
std::array<unsigned, 4> Channels(uint32_t pixel, bool packed) {
  if (packed)
    return {pixel & 1023U, (pixel >> 10) & 1023U, (pixel >> 20) & 1023U, pixel >> 30};
  return {pixel & 255U, (pixel >> 8) & 255U, (pixel >> 16) & 255U, pixel >> 24};
}
void CheckPixels(const Image& image, const a::CommandList& commands, const std::vector<uint8_t>& actual) {
  const bool packed = image.view.format == a::PixelFormat::kRgb10A2;
  const float color_max = packed ? 1023 : 255, alpha_max = packed ? 3 : 255;
  for (unsigned y = 0; y < image.view.height; ++y) {
    for (unsigned x = 0; x < image.view.width; ++x) {
      const size_t at = 256 + y * image.view.pitch + x * 4;
      uint32_t old, pixel;
      std::memcpy(&old, image.original.data() + at, 4);
      std::memcpy(&pixel, actual.data() + at, 4);
      const auto in = Channels(old, packed), out = Channels(pixel, packed);
      float value[4] = {in[0] / color_max, in[1] / color_max, in[2] / color_max, in[3] / alpha_max};
      for (size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands.data()[i];
        const float alpha = c.color.alpha * Coverage(c, x + 0.5F, y + 0.5F);
        if (alpha <= 0)
          continue;
        const float retained = value[3] * (1 - alpha), next_alpha = alpha + retained;
        const float foreground[3] = {c.color.red, c.color.green, c.color.blue};
        for (int channel = 0; channel < 3; ++channel)
          value[channel] = (foreground[channel] * alpha + value[channel] * retained) / next_alpha;
        value[3] = next_alpha;
      }
      for (int c = 0; c < 4; ++c) {
        const int wanted = std::lround(std::clamp(value[c], 0.0F, 1.0F) * (c == 3 ? alpha_max : color_max));
        if (std::abs(static_cast<int>(out[c]) - wanted) > 1)
          throw std::runtime_error("CPU/GPU pixel mismatch at " + std::to_string(x) + "," + std::to_string(y));
      }
    }
    const size_t padding = 256 + y * image.view.pitch + image.view.width * 4;
    Check(
        std::equal(
            actual.begin() + padding,
            actual.begin() + 256 + (y + 1) * image.view.pitch,
            image.original.begin() + padding),
        "row pitch padding was overwritten");
  }
  Check(std::equal(actual.begin(), actual.begin() + 256, image.original.begin()), "leading guard overwritten");
  Check(std::equal(actual.end() - 256, actual.end(), image.original.end() - 256), "trailing guard overwritten");
}
void EmptyAndCommandBounds() {
  const uint64_t before = allocations.load();
  {
    a::Compositor compositor;
    a::CommandList commands;
    for (int i = 0; i < 100; ++i) {
      Check(commands.AddLine(0, 0, 30, 30, 2, {1, 0, 0, 0}), "alpha-zero command rejected");
      Check(commands.AddText(0, 0, 20, "87 skating", {1, 1, 1, 0}), "alpha-zero text rejected");
      Check(compositor.Render({}, commands, nullptr).status == a::RenderStatus::kOk, "empty invalid surface rejected");
      commands.Clear();
    }
    const auto& c = compositor.counters();
    Check(
        c.raster_launches == 0 && c.device_allocations == 0 && c.host_allocations == 0 && c.event_creations == 0 &&
            c.h2d_calls == 0 && c.h2d_bytes == 0 && c.empty_renders == 100,
        "empty GPU work");
  }
  Check(allocations.load() == before, "empty constructor/render allocated CPU memory");
  a::CommandList limited(2, 1);
  Check(!limited.AddText(0, 0, 24, "87", {1, 1, 1, 1}) && limited.empty(), "glyph cap was not atomic");
  Check(limited.AddLine(0, 0, 10, 10, 1, {1, 0, 0, 1}), "first priority command missing");
  Check(!limited.AddText(0, 0, 24, "87", {1, 1, 1, 1}) && limited.size() == 1, "text command cap changed prefix");
  Check(limited.AddDisc(10, 10, 2, {0, 1, 0, 1}) && !limited.AddDisc(20, 20, 2, {0, 0, 1, 1}), "command cap ignored");
  Check(!limited.AddText(0, 0, 24, "\n", {1, 1, 1, 1}), "control glyph accepted");
  Check(!limited.AddDisc(NAN, 10, 1, {1, 1, 1, 1}), "nonfinite command accepted");
  std::cout << "empty_path cpu_allocations=0 device_allocations=0 uploads=0 launches=0\n";
}
void PixelTests() {
  Stream stream;
  for (const auto format : {a::PixelFormat::kRgba8, a::PixelFormat::kRgb10A2}) {
    Image image(67, 53, format);
    a::Compositor compositor;
    a::CommandList commands;
    commands.AddFill(-20, -20, 20.2F, 15.3F, {0.4F, 0.7F, 0.3F, 0.35F});
    commands.AddLine(-100, -100, 100, 100, 3.5F, {1, 0, 0, 0.65F});
    commands.AddLine(10, 40, 66, 0, 1.2F, {0, 1, 0, 0.45F});
    commands.AddDisc(31.3F, 28.7F, 14.5F, {0, 0, 1, 0.6F});
    commands.AddRectangle(20, 15, 80, 45, 3, {0.8F, 0.2F, 0.7F, 0.5F});
    commands.AddLine(10.5F, 8.5F, 10.5F, 8.5F, 5, {1, 1, 0, 1});
    commands.AddRectangle(-40, -40, 100, 100, 1, {1, 0, 1, 1}); // entirely off-screen border
    image.Reset(stream.value);
    auto first = compositor.Render(image.view, commands, stream.value);
    Check(first.status == a::RenderStatus::kOk && first.launched, "pixel render failed");
    auto result = image.Read(stream.value);
    CheckPixels(image, commands, result);
    image.Reset(stream.value);
    auto second = compositor.Render(image.view, commands, stream.value);
    Check(second.launched && image.Read(stream.value) == result, "overlap order is nondeterministic");
    const auto before = compositor.counters();
    a::CommandList offscreen;
    offscreen.AddDisc(-100, -100, 1, {1, 1, 1, 1});
    Check(!compositor.Render(image.view, offscreen, stream.value).launched, "offscreen raster launched");
    Check(compositor.counters().h2d_calls == before.h2d_calls, "offscreen command uploaded");
    Stream other;
    Check(
        compositor.Render(image.view, commands, other.value).status == a::RenderStatus::kInvalidArgument,
        "cross-stream reuse accepted");
    Cuda(cudaStreamSynchronize(stream.value));
  }
  std::cout << "pixel_tests rgba8/rgb10a2 pitch clipping guard overlap source_over deterministic passed\n";
}
void CapacityAndText() {
  Stream stream;
  Image image(128, 96, a::PixelFormat::kRgba8);
  a::CommandList full;
  full.AddFill(0, 0, 128, 96, {1, 0, 0, 1});
  for (const a::Limits limits :
       {a::Limits{1, a::kMaximumTileReferences, a::kMaximumUploadBytes},
        a::Limits{a::kMaximumActiveTiles, 1, a::kMaximumUploadBytes},
        a::Limits{a::kMaximumActiveTiles, a::kMaximumTileReferences, 1}}) {
    a::Compositor compositor(limits);
    image.Reset(stream.value);
    Check(compositor.Render(image.view, full, stream.value).status == a::RenderStatus::kCapacity, "capacity accepted");
    Check(image.Read(stream.value) == image.original, "suppressed overlay mutated video");
    Check(compositor.counters().device_allocations == 0 && compositor.counters().h2d_calls == 0, "overflow GPU work");
  }
  a::CommandList text;
  text.AddText(3, -3, 48, "87", {1, 0, 0, 1});
  text.AddText(0, 50, 24, "skating", {0, 1, 0, 1});
  std::vector<float> glyph_reference;
  size_t atlas_bytes = 0;
  for (const auto format : {a::PixelFormat::kRgba8, a::PixelFormat::kRgb10A2}) {
    const bool packed = format == a::PixelFormat::kRgb10A2;
    Image text_image(128, 96, format);
    const uint32_t black = packed ? 0xc0000000U : 0xff000000U;
    for (unsigned y = 0; y < text_image.view.height; ++y)
      for (unsigned x = 0; x < text_image.view.width; ++x)
        std::memcpy(text_image.original.data() + 256 + y * text_image.view.pitch + x * 4, &black, 4);
    a::Compositor compositor;
    text_image.Reset(stream.value);
    auto first = compositor.Render(text_image.view, text, stream.value);
    Check(
        first.launched && first.glyph_upload_bytes > 0 && first.glyph_upload_bytes <= a::kMaximumAtlasBytes,
        "bounded glyph atlas did not initialize");
    atlas_bytes = first.glyph_upload_bytes;
    const auto pixels = text_image.Read(stream.value);
    size_t changed = 0, reference_index = 0;
    for (unsigned y = 0; y < text_image.view.height; ++y) {
      for (unsigned x = 0; x < text_image.view.width; ++x) {
        uint32_t pixel;
        std::memcpy(&pixel, pixels.data() + 256 + y * text_image.view.pitch + x * 4, 4);
        changed += pixel != black;
        const auto channels = Channels(pixel, packed);
        Check(channels[3] == (packed ? 3U : 255U) && channels[2] == 0, "glyph alpha or channel packing changed");
        for (int channel = 0; channel < 3; ++channel) {
          const float normalized = channels[channel] / (packed ? 1023.0F : 255.0F);
          if (!packed)
            glyph_reference.push_back(normalized);
          else
            Check(
                std::abs(normalized - glyph_reference[reference_index++]) < 0.003F,
                "RGBA8/RGB10A2 glyph coverage or colors differ");
        }
        if (x >= 112 || y >= 74)
          Check(pixel == black, "glyph escaped its destination rectangle");
      }
      const size_t padding = 256 + y * text_image.view.pitch + text_image.view.width * 4;
      Check(
          std::equal(
              pixels.begin() + padding,
              pixels.begin() + 256 + (y + 1) * text_image.view.pitch,
              text_image.original.begin() + padding),
          "glyph overwrote row padding");
    }
    Check(changed > 300, "glyph atlas did not draw visible text");
    Check(
        std::equal(pixels.begin(), pixels.begin() + 256, text_image.original.begin()) &&
            std::equal(pixels.end() - 256, pixels.end(), text_image.original.end() - 256),
        "glyph overwrote allocation guards");
    text_image.Reset(stream.value);
    auto second = compositor.Render(text_image.view, text, stream.value);
    Check(
        second.launched && second.glyph_upload_bytes == 0 && text_image.Read(stream.value) == pixels,
        "glyph cache changed");
    Cuda(cudaStreamSynchronize(stream.value));
  }
  std::cout << "capacity_tests commands glyphs tiles refs bytes passed; paired_format_glyphs=1 glyph_atlas_bytes="
            << atlas_bytes << '\n';
}
void BusyOwnership() {
  Stream stream;
  Image image(64, 64, a::PixelFormat::kRgba8);
  a::Compositor compositor;
  a::CommandList commands;
  commands.AddFill(0, 0, 64, 64, {1, 0, 0, 1});
  image.Reset(stream.value);
  for (int i = 0; i < 3; ++i) {
    Check(compositor.Render(image.view, commands, stream.value).launched, "slot warmup failed");
    Cuda(cudaStreamSynchronize(stream.value));
  }
  const auto before = compositor.counters();
  std::atomic_bool release{false};
  Cuda(cudaLaunchHostFunc(
      stream.value,
      +[](void* p) {
        while (!static_cast<std::atomic_bool*>(p)->load(std::memory_order_acquire))
          std::this_thread::yield();
      },
      &release));
  std::array<a::RenderResult, 4> results;
  for (auto& result : results)
    result = compositor.Render(image.view, commands, stream.value);
  commands.Clear(); // in-flight H2D must retain the old command bytes
  commands.AddFill(0, 0, 64, 64, {0, 1, 0, 1});
  release.store(true, std::memory_order_release);
  auto pixels = image.Read(stream.value);
  for (int i = 0; i < 3; ++i)
    Check(results[i].launched, "available pinned slot not used");
  Check(results[3].status == a::RenderStatus::kBusy && !results[3].launched, "busy slot overwritten");
  Check(pixels[256] == 255 && pixels[257] == 0, "caller command mutation reached in-flight GPU upload");
  Check(
      compositor.counters().device_allocations == before.device_allocations &&
          compositor.counters().host_allocations == before.host_allocations,
      "warm path allocated");
  std::cout << "async_ownership busy_suppression=1 command_reuse_safe=1 warm_allocations=0\n";
}
void People(a::CommandList* commands, unsigned width, unsigned height) {
  constexpr float joints[17][2] = {
      {.5F, .07F},
      {.45F, .06F},
      {.55F, .06F},
      {.4F, .08F},
      {.6F, .08F},
      {.3F, .25F},
      {.7F, .25F},
      {.15F, .4F},
      {.85F, .4F},
      {.05F, .55F},
      {.95F, .55F},
      {.35F, .55F},
      {.65F, .55F},
      {.3F, .75F},
      {.7F, .75F},
      {.25F, .97F},
      {.75F, .97F}};
  constexpr int edges[][2] = {
      {0, 1},
      {0, 2},
      {1, 3},
      {2, 4},
      {5, 6},
      {5, 7},
      {7, 9},
      {6, 8},
      {8, 10},
      {5, 11},
      {6, 12},
      {11, 12},
      {11, 13},
      {13, 15},
      {12, 14},
      {14, 16},
      {0, 5},
      {0, 6},
      {3, 5}};
  const float scale = width / 1920.0F;
  for (int person = 0; person < 32; ++person) {
    const float left = (person % 8) * width / 8.0F + 30 * scale;
    const float top = (person / 8) * height / 4.0F + 10 * scale;
    const float w = 110 * scale, h = 190 * scale;
    const a::Color color{
        (person % 3) == 0 ? .9F : .2F, (person % 3) == 1 ? .8F : .2F, (person % 3) == 2 ? .9F : .2F, 1};
    commands->AddRectangle(left, top, left + w, top + h, 2 * scale, color);
    for (const auto& edge : edges)
      commands->AddLine(
          left + joints[edge[0]][0] * w,
          top + joints[edge[0]][1] * h,
          left + joints[edge[1]][0] * w,
          top + joints[edge[1]][1] * h,
          2.5F * scale,
          color);
    for (const auto& joint : joints)
      commands->AddDisc(left + joint[0] * w, top + joint[1] * h, 3 * scale, color);
    commands->AddText(left, top + h, 20 * scale, "87 skating", color);
  }
  Check(commands->rejected() == 0, "32-player fixture overflowed commands");
}
float Percentile(std::vector<float> values, float q) {
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(q * (values.size() - 1))];
}
void Benchmark(unsigned width, unsigned height, a::PixelFormat format) {
  Stream stream;
  Image image(width, height, format);
  a::Compositor compositor;
  a::CommandList commands;
  People(&commands, width, height);
  a::TimingEvents timing;
  Cuda(cudaEventCreate(&timing.before_upload));
  Cuda(cudaEventCreate(&timing.before_raster));
  Cuda(cudaEventCreate(&timing.after_raster));
  image.Reset(stream.value);
  const auto warm_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  for (int i = 0; i < 8 || std::chrono::steady_clock::now() < warm_until; ++i) {
    Check(compositor.Render(image.view, commands, stream.value).launched, "fixture warmup failed");
    Cuda(cudaStreamSynchronize(stream.value));
  }
  const auto before = compositor.counters();
  std::vector<float> raster, upload_and_raster, cpu, build, wall;
  a::RenderResult result;
  for (int i = 0; i < 200; ++i) {
    const uint64_t allocations_before = allocations.load();
    const auto frame_start = std::chrono::steady_clock::now();
    commands.Clear();
    People(&commands, width, height);
    const auto start = std::chrono::steady_clock::now();
    result = compositor.Render(image.view, commands, stream.value, &timing);
    const auto stop = std::chrono::steady_clock::now();
    Check(allocations.load() == allocations_before, "warmed command building/binning allocated CPU memory");
    Check(result.launched && result.glyph_upload_bytes == 0, "warmed fixture suppressed or reuploaded glyphs");
    Cuda(cudaEventSynchronize(timing.after_raster));
    const auto complete = std::chrono::steady_clock::now();
    float kernel_ms, total_ms;
    Cuda(cudaEventElapsedTime(&kernel_ms, timing.before_raster, timing.after_raster));
    Cuda(cudaEventElapsedTime(&total_ms, timing.before_upload, timing.after_raster));
    raster.push_back(kernel_ms * 1000);
    upload_and_raster.push_back(total_ms * 1000);
    cpu.push_back(std::chrono::duration<float, std::micro>(stop - start).count());
    build.push_back(std::chrono::duration<float, std::micro>(start - frame_start).count());
    wall.push_back(std::chrono::duration<float, std::micro>(complete - frame_start).count());
  }
  const auto& after = compositor.counters();
  Check(
      after.device_allocations == before.device_allocations && after.host_allocations == before.host_allocations,
      "benchmark allocated on warm path");
  Check(
      after.h2d_calls - before.h2d_calls == 200 && after.raster_launches - before.raster_launches == 200,
      "expected one upload and kernel per frame");
  std::cout << "benchmark width=" << width << " height=" << height
            << " format=" << (format == a::PixelFormat::kRgba8 ? "rgba8" : "rgb10a2")
            << " players=32 commands=" << result.commands << " active_tiles=" << result.active_tiles
            << " refs=" << result.tile_references << " upload_bytes=" << result.upload_bytes
            << " atlas_bytes=" << after.glyph_upload_bytes
            << " warmed_allocations=0 warmed_cpu_allocations=0 kernel_us_p50=" << Percentile(raster, .5F)
            << " kernel_us_p95=" << Percentile(raster, .95F)
            << " upload_kernel_us_p50=" << Percentile(upload_and_raster, .5F)
            << " cpu_build_us_p50=" << Percentile(build, .5F) << " cpu_bin_submit_us_p50=" << Percentile(cpu, .5F)
            << " completion_wall_us_p50=" << Percentile(wall, .5F)
            << " completion_wall_us_p95=" << Percentile(wall, .95F) << " device_bytes=" << after.device_bytes
            << " pinned_bytes=" << after.pinned_host_bytes << '\n';
  Cuda(cudaEventDestroy(timing.before_upload));
  Cuda(cudaEventDestroy(timing.before_raster));
  Cuda(cudaEventDestroy(timing.after_raster));
  Cuda(cudaStreamSynchronize(stream.value));
}
} // namespace
int main(int argc, char** argv) {
  try {
    EmptyAndCommandBounds();
    if (argc == 2 && std::string(argv[1]) == "--cpu-only")
      return 0;
    PixelTests();
    CapacityAndText();
    BusyOwnership();
    if (argc == 2 && std::string(argv[1]) == "--benchmark")
      for (auto format : {a::PixelFormat::kRgba8, a::PixelFormat::kRgb10A2}) {
        Benchmark(1920, 1080, format);
        Benchmark(3840, 2160, format);
      }
    std::cout << "Analytics overlay checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
