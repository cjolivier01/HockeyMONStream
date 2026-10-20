// Self-contained check of the team preprocessing kernels. Unlike the jersey
// fixtures there is nothing to compare against upstream here -- plain bilinear
// has no Pillow-specific behavior to reproduce -- so the reference is written
// out longhand below and the test needs no fixture files or model bundle.
#include "hstream/src/libs/player_analytics/runtime/SemanticGpu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pa = hm::player_analytics;

namespace {

void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t status) {
  Check(status == cudaSuccess, cudaGetErrorString(status));
}
void Cleanup(cudaError_t status) {
  if (status != cudaSuccess)
    std::cerr << "CUDA cleanup: " << cudaGetErrorString(status) << '\n';
}

struct Buffer {
  void* data{};
  explicit Buffer(size_t size) {
    Cuda(cudaMalloc(&data, size));
  }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() {
    if (data)
      Cleanup(cudaFree(data));
  }
  template <class T>
  T* as() {
    return static_cast<T*>(data);
  }
};

struct Stream {
  cudaStream_t stream{};
  Stream() {
    Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  ~Stream() {
    Cleanup(cudaStreamSynchronize(stream));
    Cleanup(cudaStreamDestroy(stream));
  }
};

constexpr int kSurfaceWidth = 320;
constexpr int kSurfaceHeight = 240;
constexpr float kOffsets[3] = {123.675F, 116.28F, 103.53F};
constexpr float kScale = 0.01735207357279195F;

// A deterministic pattern with structure on both axes, so a transposed or
// off-by-one gather cannot accidentally agree with the reference.
uint8_t SurfaceValue(int x, int y, int channel) {
  const int base = (x * 7 + y * 13 + channel * 29) % 256;
  return static_cast<uint8_t>(base);
}

std::vector<uint8_t> MakeSurface() {
  std::vector<uint8_t> pixels(static_cast<size_t>(kSurfaceWidth) * kSurfaceHeight * 4, 0);
  for (int y = 0; y < kSurfaceHeight; ++y)
    for (int x = 0; x < kSurfaceWidth; ++x)
      for (int channel = 0; channel < 3; ++channel)
        pixels[(static_cast<size_t>(y) * kSurfaceWidth + x) * 4 + channel] = SurfaceValue(x, y, channel);
  return pixels;
}

// Outside the surface is black, exactly as the jersey path pads.
float ReadSurface(int x, int y, int channel) {
  if (x < 0 || y < 0 || x >= kSurfaceWidth || y >= kSurfaceHeight)
    return 0;
  return SurfaceValue(x, y, channel);
}

// Half-pixel bilinear clamped to the crop's own edge: torch align_corners=false.
float Resample(const pa::TeamCrop& crop, int out_x, int out_y, int channel) {
  const float sx = std::min(
      std::max((out_x + 0.5F) * crop.width / pa::kTeamInputWidth - 0.5F, 0.0F), static_cast<float>(crop.width - 1));
  const float sy = std::min(
      std::max((out_y + 0.5F) * crop.height / pa::kTeamInputHeight - 0.5F, 0.0F), static_cast<float>(crop.height - 1));
  const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
  const int x1 = std::min(x0 + 1, crop.width - 1), y1 = std::min(y0 + 1, crop.height - 1);
  const float fx = sx - x0, fy = sy - y0;
  const float lower_left = ReadSurface(crop.left + x0, crop.top + y0, channel);
  const float lower_right = ReadSurface(crop.left + x1, crop.top + y0, channel);
  const float upper_left = ReadSurface(crop.left + x0, crop.top + y1, channel);
  const float upper_right = ReadSurface(crop.left + x1, crop.top + y1, channel);
  const float top = lower_left + fx * (lower_right - lower_left);
  const float bottom = upper_left + fx * (upper_right - upper_left);
  return top + fy * (bottom - top);
}

float SrgbToLinear(float value) {
  return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float Lightness(float red, float green, float blue) {
  constexpr float kEpsilon = 216.0F / 24389.0F, kKappa = 24389.0F / 27.0F;
  const float y = 0.2126F * SrgbToLinear(red) + 0.7152F * SrgbToLinear(green) + 0.0722F * SrgbToLinear(blue);
  const float f = y > kEpsilon ? std::cbrt(std::max(y, 1e-12F)) : (kKappa * y + 16.0F) / 116.0F;
  return 116.0F * f - 16.0F;
}

void Run() {
  const auto pixels = MakeSurface();
  Buffer surface(pixels.size());
  Cuda(cudaMemcpy(surface.data, pixels.data(), pixels.size(), cudaMemcpyHostToDevice));
  pa::ImageView image{surface.data, static_cast<size_t>(kSurfaceWidth) * 4, kSurfaceWidth, kSurfaceHeight,
      pa::PixelFormat::kRgba8};

  // Upscaled, downscaled, hanging off the left/top, and hanging off the
  // right/bottom, so both the clamp and the black padding are exercised.
  const std::vector<pa::TeamCrop> crops{
      {40, 30, 60, 110},
      {10, 20, 300, 220},
      {-25, -18, 90, 140},
      {kSurfaceWidth - 40, kSurfaceHeight - 35, 96, 150},
  };
  const size_t batch = crops.size();

  Stream stream;
  Buffer input(batch * pa::kTeamInputElements * sizeof(float));
  Buffer observations(batch * sizeof(pa::TeamObservation));
  Cuda(pa::PreprocessTeam(
      image, crops.data(), batch, input.as<float>(), observations.as<pa::TeamObservation>(), stream.stream));
  std::vector<float> actual(batch * pa::kTeamInputElements);
  std::vector<pa::TeamObservation> actual_lightness(batch);
  Cuda(cudaMemcpyAsync(
      actual.data(), input.data, actual.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
  Cuda(cudaMemcpyAsync(
      actual_lightness.data(),
      observations.data,
      batch * sizeof(pa::TeamObservation),
      cudaMemcpyDeviceToHost,
      stream.stream));
  Cuda(cudaStreamSynchronize(stream.stream));

  constexpr size_t kPixels = pa::kTeamInputWidth * pa::kTeamInputHeight;
  float worst = 0;
  for (size_t sample = 0; sample < batch; ++sample) {
    double lightness_sum = 0;
    for (int y = 0; y < pa::kTeamInputHeight; ++y) {
      for (int x = 0; x < pa::kTeamInputWidth; ++x) {
        float channels[3];
        for (int channel = 0; channel < 3; ++channel) {
          const float raw = Resample(crops[sample], x, y, channel);
          const float expected = (raw - kOffsets[channel]) * kScale;
          const size_t index = (sample * 3 + channel) * kPixels + static_cast<size_t>(y) * pa::kTeamInputWidth + x;
          worst = std::max(worst, std::abs(actual[index] - expected));
          channels[channel] = std::min(std::max(raw / 255.0F, 0.0F), 1.0F);
        }
        lightness_sum += Lightness(channels[0], channels[1], channels[2]);
      }
    }
    const float expected_lightness = static_cast<float>(lightness_sum / kPixels);
    const float difference = std::abs(actual_lightness[sample].lightness - expected_lightness);
    Check(
        difference < 2e-3F,
        "crop " + std::to_string(sample) + " lightness " + std::to_string(actual_lightness[sample].lightness) +
            " differs from " + std::to_string(expected_lightness));
    // A crop's lightness must stay in the CIELAB range whatever the padding did.
    Check(
        actual_lightness[sample].lightness >= 0 && actual_lightness[sample].lightness <= 100,
        "crop " + std::to_string(sample) + " lightness is outside CIELAB L");
  }
  Check(worst < 1e-4F, "resampled pixels differ by " + std::to_string(worst));

  // The same crop twice in one batch must land on identical output, and a
  // repeat of the whole call must reproduce it: the clusterer that consumes
  // these embeddings is deterministic and must not be fed drifting input.
  const std::vector<pa::TeamCrop> twice{crops[0], crops[0]};
  Buffer repeat_input(2 * pa::kTeamInputElements * sizeof(float));
  Buffer repeat_observations(2 * sizeof(pa::TeamObservation));
  Cuda(pa::PreprocessTeam(
      image,
      twice.data(),
      twice.size(),
      repeat_input.as<float>(),
      repeat_observations.as<pa::TeamObservation>(),
      stream.stream));
  std::vector<float> repeated(2 * pa::kTeamInputElements);
  std::vector<pa::TeamObservation> repeated_lightness(2);
  Cuda(cudaMemcpyAsync(
      repeated.data(), repeat_input.data, repeated.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
  Cuda(cudaMemcpyAsync(
      repeated_lightness.data(),
      repeat_observations.data,
      repeated_lightness.size() * sizeof(pa::TeamObservation),
      cudaMemcpyDeviceToHost,
      stream.stream));
  Cuda(cudaStreamSynchronize(stream.stream));
  for (size_t i = 0; i < pa::kTeamInputElements; ++i) {
    Check(repeated[i] == repeated[pa::kTeamInputElements + i], "batch slots disagree on the same crop");
    Check(repeated[i] == actual[i], "a repeated call produced different pixels");
  }
  Check(
      repeated_lightness[0].lightness == repeated_lightness[1].lightness &&
          repeated_lightness[0].lightness == actual_lightness[0].lightness,
      "lightness is not reproducible");

  // Rejections, so a bad crop cannot reach the model as silent garbage.
  Check(
      pa::PreprocessTeam(image, crops.data(), 0, input.as<float>(), observations.as<pa::TeamObservation>(),
          stream.stream) == cudaErrorInvalidValue,
      "an empty batch must be rejected");
  const pa::TeamCrop degenerate{10, 10, 0, 40};
  Check(
      pa::PreprocessTeam(image, &degenerate, 1, input.as<float>(), observations.as<pa::TeamObservation>(),
          stream.stream) == cudaErrorInvalidValue,
      "a zero-width crop must be rejected");
  Check(
      pa::PreprocessTeam(image, crops.data(), batch, nullptr, observations.as<pa::TeamObservation>(), stream.stream) ==
          cudaErrorInvalidValue,
      "a null output must be rejected");
  Check(
      pa::PreprocessTeam(image, crops.data(), batch, input.as<float>(), nullptr, stream.stream) ==
          cudaErrorInvalidValue,
      "a null observation buffer must be rejected");
  pa::ImageView broken = image;
  broken.data = nullptr;
  Check(
      pa::PreprocessTeam(broken, crops.data(), batch, input.as<float>(), observations.as<pa::TeamObservation>(),
          stream.stream) == cudaErrorInvalidValue,
      "an unmapped surface must be rejected");
}

} // namespace

int main() {
  try {
    Run();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "team preprocessing matches the reference resampler and is reproducible\n";
  return 0;
}
