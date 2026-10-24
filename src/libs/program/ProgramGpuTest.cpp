#include "hstream/src/libs/program/ProgramGpu.h"
#include "hstream/src/libs/program/ShadowToneCurve.h"

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
  constexpr size_t pitch = 40, out_pitch = 64;
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
          PixelFormat::kP010}) {
      Test(format, 0, 0, false, 0, stream.value);
      Test(format, 12, 65, false, .6, stream.value);
      Test(format, -18, 100, true, 1.3, stream.value);
    }
    for (bool ten_bit : {false, true})
      for (bool full : {false, true}) {
        TestYuv(ten_bit, full, .2126, .0722, stream.value);
        TestYuv(ten_bit, full, .299, .114, stream.value);
      }
    std::cout << "8 YUV output and 18 Program crop/rotation/tone format cases passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
