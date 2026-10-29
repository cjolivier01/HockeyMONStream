#include "hstream/src/libs/program/ProgramGpu.h"
#include "hstream/src/libs/program/ShadowToneCurve.h"

#include <opencv2/core.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using hm::gpu::Image;
using hm::gpu::PixelFormat;
using namespace hm::playcropper;
void Check(bool condition, const char* error) {
  if (!condition)
    throw std::runtime_error(error);
}
void Gpu(cudaError_t status) {
  Check(status == cudaSuccess, cudaGetErrorString(status));
}
struct Buffer {
  void* data{};
  explicit Buffer(size_t size) {
    Gpu(cudaMalloc(&data, size));
  }
  ~Buffer() {
    (void)cudaFree(data);
  }
};
struct Stream {
  cudaStream_t value{};
  Stream() {
    Gpu(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking));
  }
  ~Stream() {
    (void)cudaStreamSynchronize(value);
    (void)cudaStreamDestroy(value);
  }
};
void Test(PixelFormat format, float angle, float lift, bool black, float exposure, cudaStream_t stream) {
  constexpr int width = 8, height = 6, out_width = 13, out_height = 9;
  constexpr size_t pitch = 80, out_pitch = 64;
  const bool gray = format == PixelFormat::kGray8, rgb = format == PixelFormat::kRgb8;
  const int channels = gray ? 1 : rgb ? 3 : 4;
  std::vector<uint8_t> source(pitch * height, 0xa5), uv(pitch * height / 2, 0x5a);
  std::array<std::array<float, 4>, width * height> pixels{};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto& pixel = pixels[y * width + x];
      const int r = 4 + x * 23, g = 2 + y * 31, b = 7 + x * 5 + y * 9, a = 17 + x * 9 + y * 7;
      if (format == PixelFormat::kRgba8 || rgb || gray) {
        auto* p = source.data() + y * pitch + x * channels;
        p[0] = r;
        if (!gray) {
          p[1] = g;
          p[2] = b;
        }
        if (channels == 4)
          p[3] = a;
        pixel = {float(r), float(gray ? r : g), float(gray ? r : b), float(channels == 4 ? a : 255)};
      } else if (format == PixelFormat::kRgba16Float) {
        auto* p = reinterpret_cast<cv::float16_t*>(source.data() + y * pitch) + x * 4;
        p[0] = cv::float16_t(r + .125f);
        p[1] = cv::float16_t(g + .25f);
        p[2] = cv::float16_t(b + .0625f);
        p[3] = cv::float16_t(a + .5f);
        pixel = {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
      } else if (format == PixelFormat::kRgb10A2) {
        reinterpret_cast<uint32_t*>(source.data() + y * pitch)[x] =
            (r * 4) | ((g * 4) << 10) | ((b * 4) << 20) | (uint32_t(x % 4) << 30);
        pixel = {r * 1020.f / 1023, g * 1020.f / 1023, b * 1020.f / 1023, (x % 4) * 85.f};
      } else {
        const int luma = 16 + x * 17 + y * 9, u = 96 + (x / 2) * 10, v = 120 + (y / 2) * 14;
        float yf = luma, uf = u, vf = v;
        if (format == PixelFormat::kNv12) {
          source[y * pitch + x] = luma;
          uv[(y / 2) * pitch + (x / 2) * 2] = u;
          uv[(y / 2) * pitch + (x / 2) * 2 + 1] = v;
        } else {
          reinterpret_cast<uint16_t*>(source.data() + y * pitch)[x] = (luma * 4) << 6;
          auto* chroma = reinterpret_cast<uint16_t*>(uv.data() + (y / 2) * pitch);
          chroma[(x / 2) * 2] = (u * 4) << 6;
          chroma[(x / 2) * 2 + 1] = (v * 4) << 6;
          yf *= 1020.f / 1023;
          uf *= 1020.f / 1023;
          vf *= 1020.f / 1023;
        }
        const float factor = format == PixelFormat::kP010 ? 1020.f / 1023 : 1.f;
        yf = (yf - 16 * factor) * 255 / (219 * factor);
        uf = (uf - 128 * factor) / factor;
        vf = (vf - 128 * factor) / factor;
        pixel = {
            std::clamp(yf + 1.79274107f * vf, 0.f, 255.f),
            std::clamp(yf - .21324861f * uf - .53290933f * vf, 0.f, 255.f),
            std::clamp(yf + 2.11240179f * uf, 0.f, 255.f),
            255};
      }
    }
  }
  Buffer input(source.size()), chroma(uv.size()), output(out_pitch * out_height + 64);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemcpyAsync(chroma.data, uv.data(), uv.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, out_pitch * out_height + 64, stream));
  const float factor = format == PixelFormat::kP010 ? 1020.f / 1023 : 1.f;
  Image image{
      input.data,
      chroma.data,
      pitch,
      pitch,
      width,
      height,
      format,
      16 * factor,
      128 * factor,
      255 / (219 * factor),
      1.79274107f / factor,
      -.21324861f / factor,
      -.53290933f / factor,
      2.11240179f / factor};
  PackedOutput destination{
      output.data,
      out_pitch,
      out_width,
      out_height,
      gray      ? PixelFormat::kGray8
          : rgb ? PixelFormat::kRgb8
                : PixelFormat::kRgba8};
  const FrameTransformGeometry transform{hm::BBox(-.5, .25, 8, 6), hm::Point{3, 2}, hm::BBox(-1, -1, 9, 7), angle};
  Gpu(RenderProgram(image, destination, transform, lift, black, exposure, stream));
  std::vector<uint8_t> actual(out_pitch * out_height + 64);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, actual.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  // Independent scalar inverse transform and weighted bilinear reference.
  const double radians = -angle * std::acos(-1.0) / 180;
  for (int y = 0; y < out_height; ++y) {
    for (int x = 0; x < out_width; ++x) {
      const double bx = -1 + double(x) / out_width * 10, by = -1 + double(y) / out_height * 8;
      const double sx = -.5 + 3 + (bx - 3) * std::cos(radians) - (by - 2) * std::sin(radians);
      const double sy = .25 + 2 + (bx - 3) * std::sin(radians) + (by - 2) * std::cos(radians);
      std::array<float, 4> expected{};
      const bool inside = sx >= 0 && sy >= 0 && sx < width - 1 && sy < height - 1;
      if (inside) {
        const int ix = std::floor(sx), iy = std::floor(sy);
        const double fx = sx - ix, fy = sy - iy;
        for (int c = 0; c < channels; ++c)
          expected[c] = pixels[iy * width + ix][c] * (1 - fx) * (1 - fy) +
              pixels[iy * width + ix + 1][c] * fx * (1 - fy) + pixels[(iy + 1) * width + ix][c] * (1 - fx) * fy +
              pixels[(iy + 1) * width + ix + 1][c] * fx * fy;
        if (lift > 0 && gray)
          expected[0] = evaluate_shadow_lift_luma(expected[0] / 255, lift, black) * 255;
        else if (lift > 0) {
          for (int c = 0; c < 3; ++c)
            expected[c] /= 255;
          evaluate_shadow_lift_rgb(
              &expected[0], &expected[1], &expected[2], shadow_lift_gamma(lift), shadow_lift_amount(lift), black);
          for (int c = 0; c < 3; ++c)
            expected[c] *= 255;
        }
      }
      for (int c = 0; c < channels; ++c) {
        if (inside && c != 3) {
          expected[c] *= exposure_gain(exposure);
          if (lift > 0 || exposure > 0)
            expected[c] += .5;
        }
        const int value = std::clamp(expected[c], 0.f, 255.f);
        Check(
            std::abs(int(actual[y * out_pitch + x * channels + c]) - value) <= 1,
            "crop/rotation/color/tone/alpha differs from scalar reference");
      }
    }
    Check(
        std::all_of(
            actual.begin() + y * out_pitch + out_width * channels,
            actual.begin() + (y + 1) * out_pitch,
            [](uint8_t b) { return b == 0xa5; }),
        "output row padding changed");
  }
  Check(std::all_of(actual.end() - 64, actual.end(), [](uint8_t b) { return b == 0xa5; }), "output guard changed");
  auto bad = destination;
  bad.pitch = 1;
  Check(
      RenderProgram(image, bad, transform, lift, black, exposure, stream) == cudaErrorInvalidValue,
      "short output pitch rejected");
  bad = {input.data, pitch, width, height, PixelFormat::kRgba8};
  Check(
      RenderProgram(image, bad, transform, lift, black, exposure, stream) == cudaErrorInvalidValue,
      "overlapping input/output rejected");
  auto broken = transform;
  broken.angle = std::numeric_limits<float>::quiet_NaN();
  Check(
      RenderProgram(image, destination, broken, lift, black, exposure, stream) == cudaErrorInvalidValue,
      "nonfinite transform rejected");
}
void TestYuv(bool ten_bit, bool full_range, float kr, float kb, cudaStream_t stream) {
  constexpr size_t pitch = 24;
  std::vector<uint8_t> pixels(6 * 6 * 4);
  for (int y = 0; y < 6; ++y)
    for (int x = 0; x < 6; ++x) {
      auto* p = pixels.data() + (y * 6 + x) * 4;
      p[0] = x * 37;
      p[1] = y * 41;
      p[2] = x * 13 + y * 7;
      p[3] = 255;
    }
  Buffer input(pixels.size()), y_device(pitch * 4 + 32), uv_device(pitch * 2 + 32);
  Gpu(cudaMemcpyAsync(input.data, pixels.data(), pixels.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(y_device.data, 0xa5, pitch * 4 + 32, stream));
  Gpu(cudaMemsetAsync(uv_device.data, 0xa5, pitch * 2 + 32, stream));
  const float factor = ten_bit ? 4 : 1;
  const float maximum = ten_bit ? 1023 : 255;
  const YuvEncoding color{
      kr,
      kb,
      full_range ? 0 : 16 * factor,
      full_range ? maximum : 219 * factor,
      128 * factor,
      full_range ? maximum : 224 * factor};
  const Image image{input.data, nullptr, 24, 0, 6, 6, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const YuvOutput output{
      y_device.data, uv_device.data, pitch, pitch, 4, 4, ten_bit ? PixelFormat::kP010 : PixelFormat::kNv12, color};
  const FrameTransformGeometry t{hm::BBox(0, 0, 6, 6), hm::Point{0, 0}, hm::BBox(.25, .5, 4.25, 4.5), 0};
  Gpu(RenderProgramYuv(image, output, t, 0, false, 0, stream));
  std::vector<uint8_t> y_values(pitch * 4 + 32), uv_values(pitch * 2 + 32);
  Gpu(cudaMemcpyAsync(y_values.data(), y_device.data, y_values.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaMemcpyAsync(uv_values.data(), uv_device.data, uv_values.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  const auto encoded = [&](double x, double y) {
    const double r = (x + .25) * 37 / 255, g = (y + .5) * 41 / 255, b = ((x + .25) * 13 + (y + .5) * 7) / 255;
    const double luma = kr * r + (1 - kr - kb) * g + kb * b;
    return std::array<double, 3>{
        color.y_offset + color.y_range * luma,
        color.uv_offset + color.uv_range * (b - luma) / (2 * (1 - kb)),
        color.uv_offset + color.uv_range * (r - luma) / (2 * (1 - kr))};
  };
  const auto code = [&](const std::vector<uint8_t>& values, int x, int y) {
    if (!ten_bit)
      return int(values[y * pitch + x]);
    const auto word = reinterpret_cast<const uint16_t*>(values.data() + y * pitch)[x];
    Check((word & 63) == 0, "P010 low bits must be zero");
    return int(word >> 6);
  };
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      Check(std::abs(code(y_values, x, y) - std::lround(encoded(x, y)[0])) <= 1, "YUV luma mismatch");
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x)
      for (int c = 0; c < 2; ++c) {
        double mean = 0;
        for (int j = 0; j < 2; ++j)
          for (int i = 0; i < 2; ++i)
            mean += encoded(2 * x + i, 2 * y + j)[c + 1] * .25;
        Check(std::abs(code(uv_values, 2 * x + c, y) - std::lround(mean)) <= 1, "YUV 2x2 chroma mismatch");
      }
  const auto guards = [&](const std::vector<uint8_t>& values, int rows) {
    for (int y = 0; y < rows; ++y)
      Check(
          std::all_of(
              values.begin() + y * pitch + 4 * (ten_bit ? 2 : 1),
              values.begin() + (y + 1) * pitch,
              [](uint8_t b) { return b == 0xa5; }),
          "YUV pitch padding changed");
    Check(std::all_of(values.end() - 32, values.end(), [](uint8_t b) { return b == 0xa5; }), "YUV guard changed");
  };
  guards(y_values, 4);
  guards(uv_values, 2);
  auto bad = output;
  bad.width = 3;
  Check(RenderProgramYuv(image, bad, t, 0, false, 0, stream) == cudaErrorInvalidValue, "odd YUV geometry rejected");
  bad = output;
  bad.uv = bad.y;
  Check(
      RenderProgramYuv(image, bad, t, 0, false, 0, stream) == cudaErrorInvalidValue, "overlapping YUV planes rejected");
  bad = output;
  bad.color.uv_range = std::numeric_limits<float>::quiet_NaN();
  Check(RenderProgramYuv(image, bad, t, 0, false, 0, stream) == cudaErrorInvalidValue, "nonfinite YUV matrix rejected");
}
// A 1px pattern is what a single bilinear tap cannot see. Under an even
// reduction every tap lands on an integer source position and returns one
// whole pixel, so grid parity decides the result and a 50% grey field renders
// solid black. An area average of the footprint the output pixel actually
// covers has to return the mean instead.
//
// The orientation matters, because the tap count is per axis. A checkerboard
// reads 127.5 from any tap that is half-integer in *either* axis, so it still
// passes if one axis stops averaging. Stripes do not: horizontal ones are
// constant in x and so only the y taps can produce the mean, and the reverse
// for vertical. Run all three or a dropped axis goes unnoticed.
//
// The ratios are the ones the taps resolve: 2 is AMD's ~1.94x UHD crop
// rounded up, 4 is the cap.
enum class Pattern { kCheckerboard, kHorizontalStripes, kVerticalStripes };
uint8_t PatternValue(Pattern pattern, int x, int y) {
  const int parity = pattern == Pattern::kCheckerboard ? x + y : pattern == Pattern::kHorizontalStripes ? y : x;
  return (parity & 1) ? 255 : 0;
}
void TestMinify(
    int size,
    int ratio,
    int out_size,
    float origin,
    Pattern pattern,
    float lift,
    float exposure,
    cudaStream_t stream) {
  const size_t pitch = size * 4;
  std::vector<uint8_t> source(pitch * size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = PatternValue(pattern, x, y);
      // Alpha carries the same pattern at a different level, so a mean that
      // forgets to divide the alpha channel saturates instead of landing here.
      p[3] = static_cast<uint8_t>(PatternValue(pattern, x, y) ? 180 : 60);
    }
  const size_t out_pitch = out_size * 4 + 16;
  const size_t bytes = out_pitch * out_size + 32;
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_size, out_size, PixelFormat::kRgba8};
  const float extent = static_cast<float>(ratio * out_size);
  const FrameTransformGeometry t{
      hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(origin, origin, origin + extent, origin + extent), 0};
  Gpu(RenderProgram(image, destination, t, lift, false, exposure, stream));
  std::vector<uint8_t> actual(bytes);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  // Grading runs once on the averaged 127.5, not per tap, so the reference
  // grades the mean rather than averaging graded samples.
  float graded = 127.5f;
  if (lift > 0) {
    float r = graded / 255, g = r, b = r;
    evaluate_shadow_lift_rgb(&r, &g, &b, shadow_lift_gamma(lift), shadow_lift_amount(lift), false);
    graded = r * 255;
  }
  graded *= exposure_gain(exposure);
  if (lift > 0 || exposure > 0)
    graded += .5f;
  const int expected = std::clamp(graded, 0.f, 255.f);
  for (int y = 0; y < out_size; ++y) {
    for (int x = 0; x < out_size; ++x) {
      for (int c = 0; c < 3; ++c)
        Check(
            std::abs(int(actual[y * out_pitch + x * 4 + c]) - expected) <= 1,
            "minifying crop sampled through the gaps instead of averaging its footprint");
      Check(
          std::abs(int(actual[y * out_pitch + x * 4 + 3]) - 120) <= 1,
          "area average did not normalise the alpha channel");
    }
    Check(
        std::all_of(
            actual.begin() + y * out_pitch + out_size * 4,
            actual.begin() + (y + 1) * out_pitch,
            [](uint8_t b) { return b == 0xa5; }),
        "minifying crop overwrote pitch padding");
  }
  Check(
      std::all_of(actual.end() - 32, actual.end(), [](uint8_t b) { return b == 0xa5; }),
      "minifying crop overwrote guard");
}
// The stock crop starts at left 0, so the first column's footprint reaches
// past the source and some of its taps land outside. Averaging those in as
// transparent black halved the border; only taps that land inside count. A
// crop entirely outside still has to return the original transparent black.
void TestMinifyEdge(cudaStream_t stream) {
  constexpr int size = 32, out_size = 8;
  constexpr size_t pitch = size * 4, out_pitch = out_size * 4 + 16;
  constexpr size_t bytes = out_pitch * out_size;
  std::vector<uint8_t> source(pitch * size, 200);
  for (size_t i = 3; i < source.size(); i += 4)
    source[i] = 255;
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_size, out_size, PixelFormat::kRgba8};
  std::vector<uint8_t> actual(bytes);
  // Flush with the origin, then wholly outside the source.
  for (const bool outside : {false, true}) {
    const float origin = outside ? 40.f : 0.f;
    const FrameTransformGeometry t{
        hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(origin, origin, origin + 16, origin + 16), 0};
    Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
    Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
    Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    const int expected_rgb = outside ? 0 : 200, expected_alpha = outside ? 0 : 255;
    for (int y = 0; y < out_size; ++y)
      for (int x = 0; x < out_size; ++x) {
        for (int c = 0; c < 3; ++c)
          Check(
              std::abs(int(actual[y * out_pitch + x * 4 + c]) - expected_rgb) <= 1,
              outside ? "crop outside the source stopped returning black"
                      : "taps past the source edge darkened the border");
        Check(
            std::abs(int(actual[y * out_pitch + x * 4 + 3]) - expected_alpha) <= 1,
            outside ? "crop outside the source stopped returning transparent" : "edge taps ate the border alpha");
      }
  }
}
// A scalar model of the documented tap grid: nx*ny bilinear samples spread
// across the output pixel, averaged. The counts are written out here rather
// than taken from the kernel, so a kernel that picks a different number
// disagrees with it.
double CheckerTexel(int x, int y) {
  return ((x + y) & 1) ? 255. : 0.;
}
double BilinearChecker(double sx, double sy) {
  const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
  const double fx = sx - x0, fy = sy - y0;
  return CheckerTexel(x0, y0) * (1 - fx) * (1 - fy) + CheckerTexel(x0 + 1, y0) * fx * (1 - fy) +
      CheckerTexel(x0, y0 + 1) * (1 - fx) * fy + CheckerTexel(x0 + 1, y0 + 1) * fx * fy;
}
double AreaMean(double origin, double ex, double ey, int out_w, int out_h, int nx, int ny, int x, int y) {
  double total = 0;
  for (int j = 0; j < ny; ++j)
    for (int i = 0; i < nx; ++i)
      total += BilinearChecker(
          origin + (x + (i + .5) / nx - .5) / out_w * ex, origin + (y + (j + .5) / ny - .5) / out_h * ey);
  return total / (nx * ny);
}
std::vector<uint8_t> CheckerSource(int size, size_t pitch) {
  std::vector<uint8_t> source(pitch * size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = static_cast<uint8_t>(CheckerTexel(x, y));
      p[3] = 255;
    }
  return source;
}
// Pin the tap count itself against an explicit model. Picking the ratio
// matters: at an integer one, several counts land back on half-integers and
// read the same 127.5, so they cannot be told apart. At 7x a five-, six- or
// seven-tap grid puts samples on whole pixels and reads parity instead, and
// at 2.45x rounding the count down instead of up changes three taps to two.
void TestMinifyTapCount(int size, double ratio, int out_size, double origin, int taps, cudaStream_t stream) {
  const size_t pitch = size * 4, out_pitch = out_size * 4 + 16;
  const size_t bytes = out_pitch * out_size;
  const double extent = ratio * out_size;
  const auto source = CheckerSource(size, pitch);
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_size, out_size, PixelFormat::kRgba8};
  const FrameTransformGeometry t{
      hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(origin, origin, origin + extent, origin + extent), 0};
  Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
  std::vector<uint8_t> actual(bytes);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  for (int y = 0; y < out_size; ++y)
    for (int x = 0; x < out_size; ++x) {
      const int expected = int(AreaMean(origin, extent, extent, out_size, out_size, taps, taps, x, y));
      Check(
          std::abs(int(actual[y * out_pitch + x * 4]) - expected) <= 1,
          "a minifying crop is not using the tap count its ratio calls for");
    }
}
// RenderProgramYuv builds its own Transform, and every YUV case above uses a
// square crop into a square output, where swapping the two AxisSamples
// arguments - or dividing by the half-height chroma plane - is invisible.
// This one reduces 1.25x in x and 2.5x in y, so the counts differ. It also
// covers a crop wholly outside the source, which only the packed path had.
void TestMinifyYuvAnisotropic(cudaStream_t stream) {
  constexpr int size = 32, out_w = 16, out_h = 8;
  constexpr size_t pitch = size * 4, out_pitch = out_w * 2 + 16;
  constexpr double origin = 4, extent = 20;
  const auto source = CheckerSource(size, pitch);
  Buffer input(source.size()), luma(out_pitch * out_h), chroma(out_pitch * out_h / 2);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const YuvEncoding color{.2126f, .0722f, 0, 255, 128, 255};
  const YuvOutput output{
      luma.data, chroma.data, out_pitch, out_pitch, out_w, out_h, PixelFormat::kNv12, color};
  std::vector<uint8_t> ys(out_pitch * out_h), uvs(out_pitch * out_h / 2);
  for (const bool outside : {false, true}) {
    const double left = outside ? 40 : origin;
    const FrameTransformGeometry t{
        hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(left, left, left + extent, left + extent), 0};
    Gpu(cudaMemsetAsync(luma.data, 0xa5, ys.size(), stream));
    Gpu(cudaMemsetAsync(chroma.data, 0xa5, uvs.size(), stream));
    Gpu(RenderProgramYuv(image, output, t, 0, false, 0, stream));
    Gpu(cudaMemcpyAsync(ys.data(), luma.data, ys.size(), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaMemcpyAsync(uvs.data(), chroma.data, uvs.size(), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    for (int y = 0; y < out_h; ++y)
      for (int x = 0; x < out_w; ++x) {
        const int expected = outside ? 0 : int(std::lround(AreaMean(origin, extent, extent, out_w, out_h, 1, 3, x, y)));
        Check(
            std::abs(int(ys[y * out_pitch + x]) - expected) <= 1,
            outside ? "a YUV crop outside the source stopped returning black"
                    : "the YUV entry point crossed or mis-scaled its per-axis tap counts");
      }
    if (outside)
      for (int y = 0; y < out_h / 2; ++y)
        for (int x = 0; x < out_w; ++x)
          Check(std::abs(int(uvs[y * out_pitch + x]) - 128) <= 1, "a YUV crop outside the source shifted chroma");
  }
}
// A checkerboard is symmetric, so it cannot see a shifted tap grid. A ramp
// can: its mean lands on the footprint centre, so dropping the -.5f that
// centres the grid biases every value by half the reduction. The ramp has to
// run along each axis in turn, because one along x says nothing about the y
// offsets. Rotating it also pins the rotation, which no other minifying case
// exercises even though the shipped fixed-edge angle is nonzero. A
// rectangular output gives the two axes different tap counts, where a
// symmetric pattern cannot tell the per-axis offsets apart but a ramp's mean
// position can.
void TestMinifyRegistration(bool vertical, float angle, int out_w, int out_h, cudaStream_t stream) {
  constexpr int size = 32;
  const size_t pitch = size * 4, out_pitch = out_w * 4 + 16;
  const size_t bytes = out_pitch * out_h;
  std::vector<uint8_t> source(pitch * size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = static_cast<uint8_t>(4 + (vertical ? y : x) * 7);
      p[3] = 255;
    }
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_w, out_h, PixelFormat::kRgba8};
  const FrameTransformGeometry t{
      hm::BBox(0, 0, size, size), hm::Point{12, 12}, hm::BBox(4, 4, 20, 20), angle};
  Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
  std::vector<uint8_t> actual(bytes);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  // The ramp is linear and the tap grid is symmetric about the output pixel,
  // so the mean is the ramp's value at the pixel centre. Same scalar inverse
  // transform as the reference above, evaluated at offset zero.
  const double radians = -angle * std::acos(-1.0) / 180;
  for (int y = 0; y < out_h; ++y)
    for (int x = 0; x < out_w; ++x) {
      const double bx = 4 + double(x) / out_w * 16, by = 4 + double(y) / out_h * 16;
      const double dx = bx - 12, dy = by - 12;
      const double source = vertical ? 12 + dx * std::sin(radians) + dy * std::cos(radians)
                                     : 12 + dx * std::cos(radians) - dy * std::sin(radians);
      // The kernel truncates here, so the reference has to as well, or the
      // rotated cases spend their whole tolerance on the rounding difference.
      const int expected = int(4 + source * 7);
      for (int c = 0; c < 3; ++c)
        Check(
            std::abs(int(actual[y * out_pitch + x * 4 + c]) - expected) <= 1,
            "area average is not centred on the output pixel's footprint");
    }
}
// The tap count is chosen per axis, so every case where both axes agree
// leaves the per-axis wiring untested: an offset divided by the wrong axis's
// count, or one axis silently dropping to a single tap, both survive. Here x
// is exactly 1:1 and only the other averages; run it both ways round, or the
// untested axis is free to take its offsets from the other one's count.
// The origin-flush case only ever overruns the low edge. A crop reaching
// past the far side overruns the high one, where a bounds test that drifted
// out of step with the fetch would count a tap that contributed transparent
// black. On a solid field every pixel must come out either fully opaque at
// the source value or fully transparent - never partly darkened. The 2.5x
// ratio also puts a non-integer reduction and a 3-tap grid under test.
void TestMinifyFarEdge(cudaStream_t stream) {
  constexpr int size = 32, out_size = 8;
  constexpr size_t pitch = size * 4, out_pitch = out_size * 4 + 16;
  constexpr size_t bytes = out_pitch * out_size;
  std::vector<uint8_t> source(pitch * size, 200);
  for (size_t i = 3; i < source.size(); i += 4)
    source[i] = 255;
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_size, out_size, PixelFormat::kRgba8};
  const FrameTransformGeometry t{hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(16, 16, 36, 36), 0};
  Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
  std::vector<uint8_t> actual(bytes);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  int opaque = 0, transparent = 0;
  for (int y = 0; y < out_size; ++y)
    for (int x = 0; x < out_size; ++x) {
      const auto* p = actual.data() + y * out_pitch + x * 4;
      const bool solid = std::abs(int(p[0]) - 200) <= 1 && std::abs(int(p[1]) - 200) <= 1 &&
          std::abs(int(p[2]) - 200) <= 1 && p[3] == 255;
      const bool empty = !p[0] && !p[1] && !p[2] && !p[3];
      Check(solid || empty, "taps past the far source edge were counted and darkened the pixel");
      opaque += solid;
      transparent += empty;
    }
  Check(opaque && transparent, "far-edge case covered neither partial nor fully outside pixels");
}
// The floor itself needs ratios either side of it, close enough that moving
// it a little fails. A scalar single-tap reference settles which side the
// kernel is on: just under it the output must match that reference exactly,
// just over it must not.
void TestMinifyThreshold(cudaStream_t stream) {
  constexpr int size = 32, out_size = 10;
  constexpr size_t pitch = size * 4, out_pitch = out_size * 4 + 16;
  constexpr size_t bytes = out_pitch * out_size;
  std::vector<uint8_t> source(pitch * size);
  const auto texel = [](int x, int y) { return ((x + y) & 1) ? 255. : 0.; };
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = static_cast<uint8_t>(texel(x, y));
      p[3] = 255;
    }
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_size, out_size, PixelFormat::kRgba8};
  std::vector<uint8_t> actual(bytes);
  for (const float ratio : {1.5f, 1.74f, 1.76f, 1.8f}) {
    const float extent = ratio * out_size;
    const FrameTransformGeometry t{
        hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(4, 4, 4 + extent, 4 + extent), 0};
    Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
    Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
    Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    int matched = 0;
    for (int y = 0; y < out_size; ++y)
      for (int x = 0; x < out_size; ++x) {
        const double sx = 4 + double(x) / out_size * extent, sy = 4 + double(y) / out_size * extent;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const double fx = sx - x0, fy = sy - y0;
        const double single = texel(x0, y0) * (1 - fx) * (1 - fy) + texel(x0 + 1, y0) * fx * (1 - fy) +
            texel(x0, y0 + 1) * (1 - fx) * fy + texel(x0 + 1, y0 + 1) * fx * fy;
        matched += std::abs(int(actual[y * out_pitch + x * 4]) - int(single)) <= 1;
      }
    const int pixels = out_size * out_size;
    if (ratio < 1.75f)
      Check(matched == pixels, "a reduction below the averaging threshold stopped taking a single tap");
    else
      Check(2 * matched < pixels, "a reduction above the threshold is still taking a single tap");
  }
}
void TestMinifyAnisotropic(int out_w, int out_h, Pattern pattern, cudaStream_t stream) {
  constexpr int size = 32;
  const size_t pitch = size * 4, out_pitch = out_w * 4 + 16;
  const size_t bytes = out_pitch * out_h;
  std::vector<uint8_t> source(pitch * size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = PatternValue(pattern, x, y);
      p[3] = 255;
    }
  Buffer input(source.size()), output(bytes);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0xa5, bytes, stream));
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const PackedOutput destination{output.data, out_pitch, out_w, out_h, PixelFormat::kRgba8};
  const FrameTransformGeometry t{hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(4, 4, 20, 20), 0};
  Gpu(RenderProgram(image, destination, t, 0, false, 0, stream));
  std::vector<uint8_t> actual(bytes);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, bytes, cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  for (int y = 0; y < out_h; ++y)
    for (int x = 0; x < out_w; ++x)
      for (int c = 0; c < 3; ++c)
        Check(
            std::abs(int(actual[y * out_pitch + x * 4 + c]) - 127) <= 1,
            "per-axis tap counts are crossed or one axis stopped averaging");
}
// The AMD archive and preview render through RenderProgramYuv, so the
// minifying path needs coverage there and not only in the packed kernel.
void TestMinifyYuv(bool ten_bit, Pattern pattern, cudaStream_t stream) {
  constexpr int size = 32, out_size = 8;
  constexpr size_t pitch = size * 4, out_pitch = out_size * 2 + 16;
  std::vector<uint8_t> source(pitch * size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto* p = source.data() + y * pitch + x * 4;
      p[0] = p[1] = p[2] = PatternValue(pattern, x, y);
      p[3] = 255;
    }
  Buffer input(source.size()), luma(out_pitch * out_size), chroma(out_pitch * out_size / 2);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(luma.data, 0xa5, out_pitch * out_size, stream));
  Gpu(cudaMemsetAsync(chroma.data, 0xa5, out_pitch * out_size / 2, stream));
  const float maximum = ten_bit ? 1023 : 255;
  const YuvEncoding color{.2126f, .0722f, 0, maximum, ten_bit ? 512.f : 128.f, maximum};
  const Image image{input.data, nullptr, pitch, 0, size, size, PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const YuvOutput output{
      luma.data,
      chroma.data,
      out_pitch,
      out_pitch,
      out_size,
      out_size,
      ten_bit ? PixelFormat::kP010 : PixelFormat::kNv12,
      color};
  const FrameTransformGeometry t{hm::BBox(0, 0, size, size), hm::Point{0, 0}, hm::BBox(4, 4, 20, 20), 0};
  Gpu(RenderProgramYuv(image, output, t, 0, false, 0, stream));
  std::vector<uint8_t> ys(out_pitch * out_size), uvs(out_pitch * out_size / 2);
  Gpu(cudaMemcpyAsync(ys.data(), luma.data, ys.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaMemcpyAsync(uvs.data(), chroma.data, uvs.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  const auto code = [&](const std::vector<uint8_t>& bytes, int x, int y) {
    const auto* row = bytes.data() + y * out_pitch;
    return ten_bit ? int(reinterpret_cast<const uint16_t*>(row)[x] >> 6) : int(row[x]);
  };
  // Grey source, so the 2x2-tap mean is half scale and both chroma are neutral.
  for (int y = 0; y < out_size; ++y)
    for (int x = 0; x < out_size; ++x)
      Check(
          std::abs(code(ys, x, y) - std::lround(maximum * .5)) <= 1,
          "minifying YUV crop sampled through the gaps instead of averaging");
  for (int y = 0; y < out_size / 2; ++y)
    for (int x = 0; x < out_size; ++x)
      Check(std::abs(code(uvs, x, y) - std::lround(color.uv_offset)) <= 1, "minifying YUV crop shifted chroma");
}
void TestScoreboard(bool ten_bit, cudaStream_t stream) {
  constexpr int sw = 32, sh = 24, ow = 64, oh = 32;
  constexpr size_t pitch = 160;
  std::vector<cv::float16_t> source(sw * sh * 4);
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) {
      const size_t index = (y * sw + x) * 4;
      source[index] = cv::float16_t(10 + x * 4);
      source[index + 1] = cv::float16_t(20 + y * 7);
      source[index + 2] = cv::float16_t(3 + x * 2 + y);
      source[index + 3] = cv::float16_t(0); // Video alpha must not erase the scoreboard.
    }
  Buffer input(source.size() * sizeof(cv::float16_t)), luma(pitch * oh + 32), chroma(pitch * oh / 2 + 32);
  Gpu(cudaMemcpyAsync(
      input.data, source.data(), source.size() * sizeof(cv::float16_t), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(luma.data, 0xa5, pitch * oh + 32, stream));
  Gpu(cudaMemsetAsync(chroma.data, 0xa5, pitch * oh / 2 + 32, stream));
  const Image image{input.data, nullptr, sw * 8, 0, sw, sh, PixelFormat::kRgba16Float};
  const float maximum = ten_bit ? 1023 : 255;
  const YuvEncoding color{.2126f, .0722f, 0, maximum, ten_bit ? 512.f : 128.f, maximum};
  const YuvOutput output{
      luma.data, chroma.data, pitch, pitch, ow, oh, ten_bit ? PixelFormat::kP010 : PixelFormat::kNv12, color};
  const FrameTransformGeometry transform{hm::BBox(0, 0, sw, sh), hm::Point{0, 0}, hm::BBox(4, 4, 28, 20), 0};
  const auto made = hm::scoreboard::MakePlan(
      {{{2, 2}, {26, 3}, {25, 19}, {3, 18}}}, sw, sh, ow, oh, 25, 17, hm::scoreboard::Position::kUpperRight);
  Check(made.ok(), made.status().ToString().c_str());
  const auto plan = *made;
  Gpu(RenderProgramYuv(image, output, transform, 0, false, 1.3f, stream, &plan));
  std::vector<uint8_t> ys(pitch * oh + 32), uvs(pitch * oh / 2 + 32);
  Gpu(cudaMemcpyAsync(ys.data(), luma.data, ys.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaMemcpyAsync(uvs.data(), chroma.data, uvs.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  const auto code = [&](const std::vector<uint8_t>& bytes, int x, int y) {
    const auto* row = bytes.data() + y * pitch;
    return ten_bit ? int(reinterpret_cast<const uint16_t*>(row)[x] >> 6) : int(row[x]);
  };
  const auto expected = [&](int x, int y) {
    double sx = 4. + double(x) / ow * 24, sy = 4. + double(y) / oh * 16;
    double gain = exposure_gain(1.3f);
    if (x >= plan.left && x < plan.left + plan.width && y < plan.height) {
      const double px = x - plan.left, py = y;
      const auto* h = plan.inverse;
      const double d = h[6] * px + h[7] * py + h[8];
      sx = (h[0] * px + h[1] * py + h[2]) / d;
      sy = (h[3] * px + h[4] * py + h[5]) / d;
      gain = 1; // Scoreboard samples the original panorama after crop tone.
    }
    const double r = std::clamp((10 + sx * 4) * gain, 0., 255.) / 255;
    const double g = std::clamp((20 + sy * 7) * gain, 0., 255.) / 255;
    const double b = std::clamp((3 + sx * 2 + sy) * gain, 0., 255.) / 255;
    const double ycode = color.kr * r + (1 - color.kr - color.kb) * g + color.kb * b;
    return std::array<double, 3>{
        maximum * ycode,
        color.uv_offset + maximum * (b - ycode) / (2 * (1 - color.kb)),
        color.uv_offset + maximum * (r - ycode) / (2 * (1 - color.kr))};
  };
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x)
      Check(std::abs(code(ys, x, y) - std::lround(expected(x, y)[0])) <= 1, "Scoreboard luma/untouched crop mismatch");
  for (int y = 0; y < oh; y += 2)
    for (int x = 0; x < ow; x += 2)
      for (int c = 1; c <= 2; ++c) {
        double mean = 0;
        for (int j = 0; j < 2; ++j)
          for (int i = 0; i < 2; ++i)
            mean += expected(x + i, y + j)[c] / 4;
        Check(std::abs(code(uvs, x + c - 1, y / 2) - std::lround(mean)) <= 1, "Scoreboard mixed-block chroma mismatch");
      }
  for (const auto* bytes : {&ys, &uvs}) {
    const size_t rows = (bytes->size() - 32) / pitch;
    for (size_t y = 0; y < rows; ++y)
      Check(
          std::all_of(
              bytes->begin() + y * pitch + ow * (ten_bit ? 2 : 1),
              bytes->begin() + (y + 1) * pitch,
              [](uint8_t value) { return value == 0xa5; }),
          "Scoreboard overwrote pitch padding");
    Check(
        std::all_of(bytes->end() - 32, bytes->end(), [](uint8_t value) { return value == 0xa5; }),
        "Scoreboard overwrote guard");
  }
  auto invalid = plan;
  invalid.source_width += 1;
  Check(
      RenderProgramYuv(image, output, transform, 0, false, 0, stream, &invalid) == cudaErrorInvalidValue,
      "Scoreboard accepted stale source geometry");
  invalid = plan;
  invalid.inverse[8] = std::numeric_limits<float>::quiet_NaN();
  Check(
      RenderProgramYuv(image, output, transform, 0, false, 0, stream, &invalid) == cudaErrorInvalidValue,
      "Scoreboard accepted malformed transform");
}

} // namespace
int main() {
  try {
    Stream stream;
    for (auto format :
         {PixelFormat::kRgba8,
          PixelFormat::kRgb8,
          PixelFormat::kGray8,
          PixelFormat::kRgb10A2,
          PixelFormat::kNv12,
          PixelFormat::kP010,
          PixelFormat::kRgba16Float}) {
      Test(format, 0, 0, false, 0, stream.value);
      Test(format, 12, 65, false, .6, stream.value);
      Test(format, -18, 100, true, 1.3, stream.value);
    }
    for (bool ten_bit : {false, true})
      for (bool full : {false, true}) {
        TestYuv(ten_bit, full, .2126, .0722, stream.value);
        TestYuv(ten_bit, full, .299, .114, stream.value);
      }
    // Every case above magnifies or is exactly 1:1, so all of them stay on
    // the single-tap path and pin it.
    for (auto pattern : {Pattern::kCheckerboard, Pattern::kHorizontalStripes, Pattern::kVerticalStripes}) {
      TestMinify(32, 2, 8, 4, pattern, 0, 0, stream.value);
      TestMinify(32, 4, 7, 2, pattern, 0, 0, stream.value);
      // Ratio 12 is past the cap; its four offsets scale to +/-4.5 and
      // +/-1.5 source pixels, all half-integers, so every tap reads 127.5.
      // Five or six taps would not, so this pins the cap from below.
      TestMinify(64, 12, 4, 8, pattern, 0, 0, stream.value);
      TestMinifyYuv(false, pattern, stream.value);
    }
    TestMinify(32, 2, 8, 4, Pattern::kCheckerboard, 65, .6, stream.value);
    TestMinify(32, 2, 8, 4, Pattern::kCheckerboard, 100, 1.3, stream.value);
    TestMinifyYuv(true, Pattern::kHorizontalStripes, stream.value);
    // Integer taps read 0 and 255 rather than a flat 127.5, which is where
    // grading each tap and grading the average separate - by 17 codes here.
    TestMinify(32, 2, 8, 4.5, Pattern::kCheckerboard, 100, 0, stream.value);
    TestMinifyEdge(stream.value);
    TestMinifyFarEdge(stream.value);
    TestMinifyThreshold(stream.value);
    TestMinifyTapCount(64, 7, 4, 8, 4, stream.value);
    TestMinifyTapCount(32, 2.45, 10, 4, 3, stream.value);
    TestMinifyYuvAnisotropic(stream.value);
    TestMinifyAnisotropic(16, 8, Pattern::kHorizontalStripes, stream.value);
    TestMinifyAnisotropic(8, 16, Pattern::kVerticalStripes, stream.value);
    for (bool vertical : {false, true}) {
      for (float angle : {0.f, 12.f})
        TestMinifyRegistration(vertical, angle, 8, 8, stream.value);
      TestMinifyRegistration(vertical, 0, 8, 4, stream.value);
    }
    TestScoreboard(false, stream.value);
    TestScoreboard(true, stream.value);
    std::cout << "2 scoreboard, 30 minifying crop, 8 YUV output and 21 Program crop/rotation/tone format cases passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
