#include "hstream/src/libs/detection/DetectorGpu.h"
#include "hstream/src/libs/detection/DetectionPostprocess.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {
using namespace hm::detection;
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
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
void TestImage(PixelFormat format, cudaStream_t stream) {
  constexpr int width = 6, height = 4;
  constexpr size_t pitch = 32;
  std::vector<uint8_t> source(pitch * height, 0xa5), chroma(pitch * height / 2, 0x5a);
  cv::Mat reference(height, width, CV_32FC3);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      auto& color = reference.at<cv::Vec3f>(y, x);
      if (format == PixelFormat::kRgba8) {
        const uint8_t r = 15 + x * 30, g = 20 + y * 50, b = 8 + x * 8 + y * 9;
        auto* p = source.data() + y * pitch + x * 4;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
        color = cv::Vec3f(r, g, b);
      } else if (format == PixelFormat::kRgb10A2) {
        const uint32_t r = 60 + x * 120, g = 80 + y * 200, b = 32 + x * 32 + y * 36;
        reinterpret_cast<uint32_t*>(source.data() + y * pitch)[x] = r | (g << 10) | (b << 20) | (3u << 30);
        color = cv::Vec3f(r, g, b) * (255.f / 1023);
      } else {
        // Vary chroma across both axes to cover subsampling and pitched UV addressing.
        const int luma = 32 + x * 18 + y * 12;
        const int u8 = 96 + (x / 2) * 10, v8 = 136 + (y / 2) * 16;
        float yf, uf, vf;
        if (format == PixelFormat::kNv12) {
          source[y * pitch + x] = luma;
          chroma[(y / 2) * pitch + (x / 2) * 2] = u8;
          chroma[(y / 2) * pitch + (x / 2) * 2 + 1] = v8;
          yf = luma;
          uf = u8;
          vf = v8;
        } else {
          reinterpret_cast<uint16_t*>(source.data() + y * pitch)[x] = (luma * 4) << 6;
          auto* p = reinterpret_cast<uint16_t*>(chroma.data() + (y / 2) * pitch);
          p[(x / 2) * 2] = (u8 * 4) << 6;
          p[(x / 2) * 2 + 1] = (v8 * 4) << 6;
          yf = luma * 4 * (255.f / 1023);
          uf = u8 * 4 * (255.f / 1023);
          vf = v8 * 4 * (255.f / 1023);
        }
        const float factor = format == PixelFormat::kNv12 ? 1.f : 1020.f / 1023;
        yf = (yf - 16 * factor) * 255 / (219 * factor);
        uf = (uf - 128 * factor) / factor;
        vf = (vf - 128 * factor) / factor;
        color = cv::Vec3f(
            std::clamp(yf + 1.79274107f * vf, 0.f, 255.f),
            std::clamp(yf - .21324861f * uf - .53290933f * vf, 0.f, 255.f),
            std::clamp(yf + 2.11240179f * uf, 0.f, 255.f));
      }
    }
  const auto box = MakeLetterbox(width, height, 13, 11, true, true, 1.f / 255);
  Check(box.ok(), "letterbox");
  cv::Mat resized;
  cv::resize(reference, resized, cv::Size(box->resized_width, box->resized_height), 0, 0, cv::INTER_LINEAR);
  Buffer input(source.size()), uv(chroma.size()), output(3 * 13 * 11 * sizeof(float) + 64);
  Gpu(cudaMemcpyAsync(input.data, source.data(), source.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemcpyAsync(uv.data, chroma.data(), chroma.size(), cudaMemcpyHostToDevice, stream));
  Gpu(cudaMemsetAsync(output.data, 0x5a, 3 * 13 * 11 * sizeof(float) + 64, stream));
  const float factor = format == PixelFormat::kP010 ? 1020.f / 1023 : 1.f;
  Image image{
      input.data,
      uv.data,
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
  Gpu(PreprocessDetector(image, *box, static_cast<float*>(output.data), stream));
  std::vector<float> actual(3 * 13 * 11);
  Gpu(cudaMemcpyAsync(actual.data(), output.data, actual.size() * sizeof(float), cudaMemcpyDeviceToHost, stream));
  std::vector<uint8_t> guard(64);
  Gpu(cudaMemcpyAsync(
      guard.data(),
      static_cast<char*>(output.data) + actual.size() * sizeof(float),
      64,
      cudaMemcpyDeviceToHost,
      stream));
  Gpu(cudaStreamSynchronize(stream));
  float maximum_error = 0;
  for (int y = 0; y < box->height; ++y)
    for (int x = 0; x < box->width; ++x)
      for (int c = 0; c < 3; ++c) {
        float expected = 0;
        if (x >= box->left && x < box->left + box->resized_width && y >= box->top && y < box->top + box->resized_height)
          expected = resized.at<cv::Vec3f>(y - box->top, x - box->left)[c] / 255;
        maximum_error = std::max(maximum_error, std::abs(actual[c * 13 * 11 + y * 13 + x] - expected));
      }
  Check(maximum_error < 1e-5, "RGB/YUV preprocessing must match OpenCV float bilinear reference");
  Check(std::all_of(guard.begin(), guard.end(), [](uint8_t b) { return b == 0x5a; }), "output guard");
  image.pitch0 = 1;
  Check(
      PreprocessDetector(image, *box, static_cast<float*>(output.data), stream) == cudaErrorInvalidValue,
      "short pitch rejected");
}
void TestFiltering(cudaStream_t stream) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<float> raw{1,  2,  11, 12, .8, 0,   2,   3,   12, 13, .8, 0,  1,   2,  11, 12, .8,
                               1,  -3, -4, 5,  6,  .25, 0,   1,   2,  11, 12, .2, 0,   1,  2,  11, 12,
                               .9, 2,  1,  2,  11, 12,  nan, 0,   1,  2,  11, 12, .8,  .5, 1,  2,  1,
                               12, .8, 0,  0,  0,  1,   1,   1.1, 0,  0,  0,  1,  nan, .9, 0};
  const size_t rows = raw.size() / 6;
  Buffer input(raw.size() * sizeof(float)), output(rows * sizeof(Proposal)), count(sizeof(uint32_t));
  Gpu(cudaMemcpyAsync(input.data, raw.data(), raw.size() * sizeof(float), cudaMemcpyHostToDevice, stream));
  for (int iteration = 0; iteration < 2; ++iteration) {
    Gpu(FilterDetectorProposals(
        static_cast<float*>(input.data),
        rows,
        2,
        .25,
        20,
        20,
        static_cast<Proposal*>(output.data),
        static_cast<uint32_t*>(count.data),
        stream));
    uint32_t size = 0;
    Gpu(cudaMemcpyAsync(&size, count.data, sizeof(size), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    Check(size == 4, "filter finite scores, classes, coordinates, and reset count");
    std::vector<Proposal> proposals(size);
    Gpu(cudaMemcpyAsync(
        proposals.data(), output.data, proposals.size() * sizeof(Proposal), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    const auto box = MakeLetterbox(40, 20, 20, 20, true, true, 1.f / 255);
    Check(box.ok(), "postprocess letterbox");
    const auto result = SuppressProposals(proposals, .45, 300, *box, 40, 20);
    Check(result.ok() && result->size() == 3, "class-aware suppression");
    Check(
        (*result)[0].source_index == 0 && (*result)[1].source_index == 2 && (*result)[2].source_index == 3,
        "stable tensor-order tie break across parallel compaction");
    Check(
        (*result)[0].left == 2 && (*result)[0].top == 0 && (*result)[0].right == 22 && (*result)[0].bottom == 14,
        "remove padding and rescale detection coordinates");
  }
}
} // namespace
int main() {
  cudaStream_t stream{};
  try {
    Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    for (auto format : {PixelFormat::kRgba8, PixelFormat::kRgb10A2, PixelFormat::kNv12, PixelFormat::kP010})
      TestImage(format, stream);
    TestFiltering(stream);
    Gpu(cudaStreamDestroy(stream));
    std::cout << "Detector RGB8/RGB10/NV12/P010 preprocessing and proposal tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (stream)
      (void)cudaStreamDestroy(stream);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
