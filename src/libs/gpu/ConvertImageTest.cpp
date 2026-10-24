#include "hstream/src/libs/gpu/ConvertImage.h"

#include <opencv2/core.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace hm::gpu;
void Check(bool valid, const char* message) {
  if (!valid)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t status) {
  Check(status == cudaSuccess, cudaGetErrorString(status));
}
struct Buffer {
  void* data{};
  explicit Buffer(size_t bytes) {
    Gpu(cudaMalloc(&data, bytes));
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
void Test(PixelFormat format, int width, int height, cudaStream_t stream) {
  const size_t input_pitch = width * 8 + 24, output_pitch = width * 8 + 32;
  std::vector<uint8_t> input(input_pitch * height, 0xa5), uv(input_pitch * height, 0x5a);
  std::vector<std::array<float, 4>> expected(width * height);
  const bool ten_bit = format == PixelFormat::kP010;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto* row = input.data() + y * input_pitch;
      auto& p = expected[y * width + x];
      p = {float(8 + x * 7), float(15 + y * 23), float(70 + x + y), float(100 + x * 3)};
      if (format == PixelFormat::kRgba8) {
        for (int c = 0; c < 4; ++c)
          row[x * 4 + c] = p[c];
      } else if (format == PixelFormat::kRgb10A2) {
        uint32_t packed = 0;
        for (int c = 0; c < 3; ++c) {
          const auto code = static_cast<uint32_t>(p[c] * 4 + 1);
          packed |= code << (c * 10);
          p[c] = code * (255.f / 1023);
        }
        packed |= uint32_t(x % 4) << 30;
        p[3] = (x % 4) * 85.f;
        reinterpret_cast<uint32_t*>(row)[x] = packed;
      } else if (format == PixelFormat::kRgba16Float) {
        for (int c = 0; c < 4; ++c) {
          const auto value = cv::float16_t(p[c] + .125f);
          reinterpret_cast<cv::float16_t*>(row)[x * 4 + c] = value;
          p[c] = float(value);
        }
      } else if (format == PixelFormat::kRgb8) {
        for (int c = 0; c < 3; ++c)
          row[x * 3 + c] = p[c];
        p[3] = 255;
      } else if (format == PixelFormat::kGray8) {
        row[x] = p[0];
        p = {p[0], p[0], p[0], 255};
      } else {
        const int luma = 20 + x * 11 + y * 19, u = 90 + (x / 2) * 9, v = 110 + (y / 2) * 13;
        auto* chroma = uv.data() + (y / 2) * input_pitch;
        if (ten_bit) {
          reinterpret_cast<uint16_t*>(row)[x] = (luma * 4 + 1) << 6;
          reinterpret_cast<uint16_t*>(chroma)[(x / 2) * 2] = (u * 4 + 2) << 6;
          reinterpret_cast<uint16_t*>(chroma)[(x / 2) * 2 + 1] = (v * 4 + 3) << 6;
        } else {
          row[x] = luma;
          chroma[(x / 2) * 2] = u;
          chroma[(x / 2) * 2 + 1] = v;
        }
        const double yf = (luma + (ten_bit ? .25 : 0) - 16) * 255 / 219;
        const double uf = u + (ten_bit ? .5 : 0) - 128, vf = v + (ten_bit ? .75 : 0) - 128;
        p = {
            float(std::clamp(yf + 1.79274107 * vf, 0., 255.)),
            float(std::clamp(yf - .21324861 * uf - .53290933 * vf, 0., 255.)),
            float(std::clamp(yf + 2.11240179 * uf, 0., 255.)),
            255};
      }
    }
  }
  Buffer source(input.size()), chroma(uv.size()), destination(output_pitch * height + 32);
  Gpu(cudaMemcpyAsync(source.data, input.data(), input.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemcpyAsync(chroma.data, uv.data(), uv.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(destination.data, 0xa5, output_pitch * height + 32, stream));
  const float factor = ten_bit ? 1020.f / 1023 : 1.f;
  Image image{
      source.data,
      chroma.data,
      input_pitch,
      input_pitch,
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
  Gpu(ConvertToRgba16Float(image, destination.data, output_pitch, stream));
  std::vector<uint8_t> output(output_pitch * height + 32);
  Gpu(cudaMemcpyAsync(output.data(), destination.data, output.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  for (int y = 0; y < height; ++y) {
    const auto* row = reinterpret_cast<const cv::float16_t*>(output.data() + y * output_pitch);
    for (int x = 0; x < width; ++x)
      for (int c = 0; c < 4; ++c)
        Check(
            std::abs(float(row[x * 4 + c]) - expected[y * width + x][c]) <= .13f,
            "Pixel mismatch (includes final row/column)");
    for (size_t i = width * 8; i < output_pitch; ++i)
      Check(output[y * output_pitch + i] == 0xa5, "Output pitch guard overwritten");
  }
  for (size_t i = output_pitch * height; i < output.size(); ++i)
    Check(output[i] == 0xa5, "Output tail guard overwritten");
  Check(
      ConvertToRgba16Float(image, source.data, input_pitch, stream) == cudaErrorInvalidValue,
      "Overlapping planes accepted");
  Check(
      ConvertToRgba16Float(image, destination.data, width * 8 - 1, stream) == cudaErrorInvalidValue,
      "Short output pitch accepted");
  Check(
      ConvertToRgba16Float(image, destination.data, std::numeric_limits<size_t>::max(), stream) ==
          cudaErrorInvalidValue,
      "Overflowing pitch accepted");
  image.width = 0;
  Check(
      ConvertToRgba16Float(image, destination.data, output_pitch, stream) == cudaErrorInvalidValue,
      "Empty image accepted");
}
} // namespace
int main() {
  try {
    Stream stream;
    for (const auto format :
         {PixelFormat::kRgba8,
          PixelFormat::kRgb10A2,
          PixelFormat::kRgba16Float,
          PixelFormat::kRgb8,
          PixelFormat::kGray8,
          PixelFormat::kNv12,
          PixelFormat::kP010}) {
      Test(format, 8, 6, stream.value);
      if (format != PixelFormat::kNv12 && format != PixelFormat::kP010) {
        Test(format, 1, 1, stream.value);
        Test(format, 7, 3, stream.value);
      }
    }
    std::cout << "GPU conversion: seven formats, odd/single-pixel images, precision and guards passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
