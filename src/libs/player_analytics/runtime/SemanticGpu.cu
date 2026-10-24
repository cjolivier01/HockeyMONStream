#include "hstream/src/libs/player_analytics/runtime/SemanticGpu.h"

#include <algorithm>
#include <limits>

namespace hm::player_analytics {
namespace {
constexpr int kCoefficientBits = 22;
constexpr int kHorizontalTaps = 4 * kJerseyMaximumCropExtent / kJerseyInputWidth + 1;
constexpr int kVerticalTaps = 4 * kJerseyMaximumCropExtent / kJerseyInputHeight + 1;

template <int Output, int Taps>
struct AxisWeights {
  int start[Output];
  int count[Output];
  int32_t coefficients[Output][Taps];
};
struct CropArguments {
  JerseyCrop values[kMaximumBatch];
};
struct JerseyWork {
  JerseyCrop crop;
  AxisWeights<kJerseyInputWidth, kHorizontalTaps> x;
  AxisWeights<kJerseyInputHeight, kVerticalTaps> y;
  uint8_t horizontal[kJerseyMaximumCropExtent * kJerseyInputWidth * 3];
};

// Match scalar Pillow's double polynomial evaluation, including its absence of
// fused multiply-add. The coefficient precision is Pillow's22-bit RGB path.
__device__ double Cubic(double x) {
  x = fabs(x);
  if (x < 1.0)
    return __dadd_rn(__dmul_rn(__dmul_rn(__dadd_rn(__dmul_rn(1.5, x), -2.5), x), x), 1.0);
  if (x < 2.0)
    return __dmul_rn(__dadd_rn(__dmul_rn(__dadd_rn(__dmul_rn(x - 5.0, x), 8.0), x), -4.0), -0.5);
  return 0;
}

template <int Output, int Taps>
__device__ void PrepareAxis(int extent, int output, AxisWeights<Output, Taps>& weights) {
  const double scale = static_cast<double>(extent) / Output;
  const double filter_scale = fmax(1.0, scale), support = 2.0 * filter_scale;
  const double center = (output + 0.5) * scale, inverse_scale = 1.0 / filter_scale;
  const int first = max(0, static_cast<int>(center - support + 0.5));
  const int end = min(extent, static_cast<int>(center + support + 0.5));
  double sum = 0;
  for (int source = first; source < end; ++source)
    sum = __dadd_rn(sum, Cubic((source - center + 0.5) * inverse_scale));
  weights.start[output] = first;
  weights.count[output] = end - first;
  for (int source = first; source < end; ++source) {
    const double weight = Cubic((source - center + 0.5) * inverse_scale) / sum;
    const double scaled = __dmul_rn(weight, static_cast<double>(1 << kCoefficientBits));
    weights.coefficients[output][source - first] = static_cast<int>(__dadd_rn(scaled, weight < 0 ? -0.5 : 0.5));
  }
}

__global__ void Coefficients(JerseyWork* work, CropArguments crops) {
  auto& sample = work[blockIdx.x];
  const int output = threadIdx.x;
  const auto crop = crops.values[blockIdx.x];
  if (blockIdx.y == 0) {
    if (output == 0)
      sample.crop = crop;
    PrepareAxis(crop.width, output, sample.x);
  } else if (output < kJerseyInputHeight) {
    PrepareAxis(crop.height, output, sample.y);
  }
}

__device__ int Read(const ImageView image, int x, int y, int channel) {
  if (x < 0 || y < 0 || x >= image.width || y >= image.height)
    return 0;
  const auto* row = static_cast<const uint8_t*>(image.data) + static_cast<size_t>(y) * image.pitch;
  if (image.format == PixelFormat::kRgba8)
    return row[x * 4 + channel];
  const uint32_t pixel = reinterpret_cast<const uint32_t*>(row)[x];
  return (((pixel >> (channel * 10)) & 1023U) * 255U + 511U) / 1023U;
}
__device__ uint8_t Quantize(int value) {
  return static_cast<uint8_t>(max(0, min(255, value >> kCoefficientBits)));
}

__global__ void Horizontal(ImageView image, JerseyWork* work) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  auto& sample = work[blockIdx.z];
  if (x >= kJerseyInputWidth || y >= sample.crop.height)
    return;
  const int first = sample.x.start[x], count = sample.x.count[x];
  for (int channel = 0; channel < 3; ++channel) {
    int accumulator = 1 << (kCoefficientBits - 1);
    for (int tap = 0; tap < count; ++tap)
      accumulator +=
          Read(image, sample.crop.left + first + tap, sample.crop.top + y, channel) * sample.x.coefficients[x][tap];
    sample.horizontal[(y * kJerseyInputWidth + x) * 3 + channel] = Quantize(accumulator);
  }
}

__global__ void Vertical(const JerseyWork* work, float* output) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= kJerseyInputWidth || y >= kJerseyInputHeight)
    return;
  const auto& sample = work[blockIdx.z];
  const int first = sample.y.start[y], count = sample.y.count[y];
  for (int channel = 0; channel < 3; ++channel) {
    int accumulator = 1 << (kCoefficientBits - 1);
    for (int tap = 0; tap < count; ++tap)
      accumulator +=
          sample.horizontal[((first + tap) * kJerseyInputWidth + x) * 3 + channel] * sample.y.coefficients[y][tap];
    const size_t index = (static_cast<size_t>(blockIdx.z) * 3 + channel) * kJerseyInputHeight * kJerseyInputWidth +
        y * kJerseyInputWidth + x;
    output[index] = Quantize(accumulator) / 127.5F - 1.0F;
  }
}

struct Distribution {
  float confidence;
  int index;
  bool valid;
};
template <int Threads>
struct ReductionStorage {
  float value[Threads];
  int index[Threads];
  int invalid[Threads];
};

template <int Classes, int Threads>
__device__ Distribution TopProbability(const float* logits, ReductionStorage<Threads>& storage) {
  const int tid = threadIdx.x;
  const float value = tid < Classes ? logits[tid] : -std::numeric_limits<float>::infinity();
  storage.invalid[tid] = tid < Classes && !isfinite(value);
  storage.value[tid] = isfinite(value) ? value : -std::numeric_limits<float>::infinity();
  storage.index[tid] = tid;
  __syncthreads();
  for (int stride = Threads / 2; stride > 0; stride /= 2) {
    if (tid < stride) {
      const int other = tid + stride;
      if (storage.value[other] > storage.value[tid] ||
          (storage.value[other] == storage.value[tid] && storage.index[other] < storage.index[tid])) {
        storage.value[tid] = storage.value[other];
        storage.index[tid] = storage.index[other];
      }
      storage.invalid[tid] |= storage.invalid[other];
    }
    __syncthreads();
  }
  const float maximum = storage.value[0];
  const int winner = storage.index[0];
  const bool valid = !storage.invalid[0] && isfinite(maximum);
  __syncthreads();
  storage.value[tid] = tid < Classes && valid ? expf(value - maximum) : 0;
  __syncthreads();
  for (int stride = Threads / 2; stride > 0; stride /= 2) {
    if (tid < stride)
      storage.value[tid] += storage.value[tid + stride];
    __syncthreads();
  }
  const float confidence = valid ? 1.0F / storage.value[0] : 0;
  __syncthreads();
  return {confidence, winner, valid};
}

__global__ void JerseyDecode(const float* logits, JerseyVocabulary vocabulary, JerseyObservation* observations) {
  __shared__ ReductionStorage<128> scratch;
  __shared__ Distribution positions[3];
  for (int position = 0; position < 3; ++position) {
    const auto distribution =
        TopProbability<kJerseyTokens, 128>(logits + (blockIdx.x * 3 + position) * kJerseyTokens, scratch);
    if (threadIdx.x == 0)
      positions[position] = distribution;
    __syncthreads();
  }
  if (threadIdx.x != 0)
    return;
  JerseyObservation result{};
  float probability = 1;
  for (int position = 0; position < 3; ++position) {
    const auto top = positions[position];
    if (!top.valid) {
      result = {};
      break;
    }
    probability *= top.confidence;
    if (top.index == vocabulary.eos_index) {
      if (position > 0)
        result.confidence = probability;
      break;
    }
    const int digit = vocabulary.digits[top.index];
    if (digit < 0 || position == 2) {
      result = {};
      break;
    }
    result.text[position] = '0' + digit;
    result.length = position + 1;
  }
  if (!(result.confidence > 0))
    result = {};
  observations[blockIdx.x] = result;
}

template <int Labels>
__global__ void ActionReduce(const float* logits, ActionObservation* observations) {
  __shared__ ReductionStorage<64> scratch;
  const auto top = TopProbability<Labels, 64>(logits + blockIdx.x * Labels, scratch);
  if (threadIdx.x == 0)
    observations[blockIdx.x] = top.valid ? ActionObservation{top.index, top.confidence} : ActionObservation{};
}

// reidentificationnet-rgb-bilinear-v1, as declared by the manifest and
// enforced by TeamProfile(). Kept here rather than read from the manifest for
// the same reason the jersey kernel hardcodes its own: the contract pins the
// preprocessing id, so a model wanting different numbers is a different id.
constexpr float kTeamScale = 0.01735207357279195F;
// Namespace-scope constexpr arrays are not addressable from device code, so
// the per-channel offsets live inside a device function instead.
__device__ __forceinline__ float TeamOffset(int channel) {
  constexpr float offsets[3] = {123.675F, 116.28F, 103.53F};
  return offsets[channel];
}

__global__ void TeamResample(const ImageView image, CropArguments crops, float* output) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= kTeamInputWidth || y >= kTeamInputHeight)
    return;
  const auto crop = crops.values[blockIdx.z];
  // Half-pixel source coordinates, clamped to the crop's own edge. That is
  // torch's align_corners=false, which is what cut these crops on the Python
  // side. Clamping is against the crop, not the image: a crop may hang off the
  // surface, and Read() pads those pixels black exactly as the jersey path does.
  const float sx =
      fminf(fmaxf((x + 0.5F) * crop.width / kTeamInputWidth - 0.5F, 0.0F), static_cast<float>(crop.width - 1));
  const float sy =
      fminf(fmaxf((y + 0.5F) * crop.height / kTeamInputHeight - 0.5F, 0.0F), static_cast<float>(crop.height - 1));
  const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
  const int x1 = min(x0 + 1, crop.width - 1), y1 = min(y0 + 1, crop.height - 1);
  const float fx = sx - x0, fy = sy - y0;
  for (int channel = 0; channel < 3; ++channel) {
    const float lower_left = Read(image, crop.left + x0, crop.top + y0, channel);
    const float lower_right = Read(image, crop.left + x1, crop.top + y0, channel);
    const float upper_left = Read(image, crop.left + x0, crop.top + y1, channel);
    const float upper_right = Read(image, crop.left + x1, crop.top + y1, channel);
    const float top = __fmaf_rn(fx, lower_right - lower_left, lower_left);
    const float bottom = __fmaf_rn(fx, upper_right - upper_left, upper_left);
    const size_t index =
        (static_cast<size_t>(blockIdx.z) * 3 + channel) * kTeamInputHeight * kTeamInputWidth + y * kTeamInputWidth + x;
    output[index] = (__fmaf_rn(fy, bottom - top, top) - TeamOffset(channel)) * kTeamScale;
  }
}

__device__ float SrgbToLinear(float value) {
  return value <= 0.04045F ? value / 12.92F : powf((value + 0.055F) / 1.055F, 2.4F);
}

// CIELAB L from sRGB in0..1, D65, matching hmlib/team/features.py::rgb_to_lab.
// Only L is wanted, so the chromatic axes and their white-point divisions drop out.
__device__ float Lightness(float red, float green, float blue) {
  constexpr float kEpsilon = 216.0F / 24389.0F, kKappa = 24389.0F / 27.0F;
  const float y = 0.2126F * SrgbToLinear(red) + 0.7152F * SrgbToLinear(green) + 0.0722F * SrgbToLinear(blue);
  const float f = y > kEpsilon ? cbrtf(fmaxf(y, 1e-12F)) : (kKappa * y + 16.0F) / 116.0F;
  return __fmaf_rn(116.0F, f, -16.0F);
}

// Reads back the normalized batch and undoes the scaling rather than resampling
// the surface a second time. One pass, and the lightness then describes exactly
// the pixels the model was shown. The Python side pools to32x32 before
// converting; this averages per-pixel L, which is the quantity that pooling
// approximates. Each thread owns a fixed stride of the crop, so the sum is
// reproducible run to run.
__global__ void TeamLightnessReduce(const float* input, TeamObservation* observations) {
  constexpr int kThreads = 256, kPixels = kTeamInputWidth * kTeamInputHeight;
  __shared__ float storage[kThreads];
  const int tid = threadIdx.x;
  const float* crop = input + static_cast<size_t>(blockIdx.x) * kTeamInputElements;
  float sum = 0;
  for (int pixel = tid; pixel < kPixels; pixel += kThreads) {
    float channels[3];
    for (int channel = 0; channel < 3; ++channel)
      channels[channel] =
          fminf(fmaxf((crop[channel * kPixels + pixel] / kTeamScale + TeamOffset(channel)) / 255.0F, 0.0F), 1.0F);
    sum += Lightness(channels[0], channels[1], channels[2]);
  }
  storage[tid] = sum;
  __syncthreads();
  for (int stride = kThreads / 2; stride > 0; stride /= 2) {
    if (tid < stride)
      storage[tid] += storage[tid + stride];
    __syncthreads();
  }
  if (tid == 0)
    observations[blockIdx.x].lightness = storage[0] / kPixels;
}

bool ValidImage(const ImageView& image) {
  return image.data && image.width > 0 && image.height > 0 && image.width <= 65536 && image.height <= 65536 &&
      image.pitch >= static_cast<size_t>(image.width) * 4 && image.pitch % 4 == 0 &&
      image.pitch <= SIZE_MAX / static_cast<size_t>(image.height) && reinterpret_cast<uintptr_t>(image.data) % 4 == 0 &&
      (image.format == PixelFormat::kRgba8 || image.format == PixelFormat::kRgba10A2);
}
} // namespace

size_t JerseyScratchBytes(size_t maximum_batch) noexcept {
  return maximum_batch > 0 && maximum_batch <= kMaximumBatch ? maximum_batch * sizeof(JerseyWork) : 0;
}

cudaError_t PreprocessJersey(
    const ImageView& image,
    const JerseyCrop* crops,
    size_t batch,
    void* device_scratch,
    size_t scratch_bytes,
    float* input,
    cudaStream_t stream) {
  if (!ValidImage(image) || !crops || !device_scratch ||
      reinterpret_cast<uintptr_t>(device_scratch) % alignof(JerseyWork) != 0 || !input || !stream || batch == 0 ||
      batch > kMaximumBatch || scratch_bytes < JerseyScratchBytes(batch))
    return cudaErrorInvalidValue;
  CropArguments arguments{};
  int maximum_height = 0;
  for (size_t index = 0; index < batch; ++index) {
    const auto& crop = crops[index];
    if (crop.left < -1000000 || crop.top < -1000000 || crop.left > 1000000 || crop.top > 1000000 || crop.width < 1 ||
        crop.height < 1 || crop.width > kJerseyMaximumCropExtent || crop.height > kJerseyMaximumCropExtent)
      return cudaErrorInvalidValue;
    maximum_height = std::max(maximum_height, crop.height);
    arguments.values[index] = crop;
  }
  auto* work = static_cast<JerseyWork*>(device_scratch);
  Coefficients<<<dim3(batch, 2), kJerseyInputWidth, 0, stream>>>(work, arguments);
  auto result = cudaGetLastError();
  if (result != cudaSuccess)
    return result;
  Horizontal<<<dim3(kJerseyInputWidth / 16, (maximum_height + 15) / 16, batch), dim3(16, 16), 0, stream>>>(image, work);
  result = cudaGetLastError();
  if (result != cudaSuccess)
    return result;
  Vertical<<<dim3(kJerseyInputWidth / 16, kJerseyInputHeight / 16, batch), dim3(16, 16), 0, stream>>>(work, input);
  return cudaGetLastError();
}

cudaError_t PreprocessTeam(
    const ImageView& image,
    const TeamCrop* crops,
    size_t batch,
    float* input,
    TeamObservation* observations,
    cudaStream_t stream) {
  if (!ValidImage(image) || !crops || !input || !observations || !stream || batch == 0 || batch > kMaximumBatch)
    return cudaErrorInvalidValue;
  CropArguments arguments{};
  for (size_t index = 0; index < batch; ++index) {
    const auto& crop = crops[index];
    if (crop.left < -1000000 || crop.top < -1000000 || crop.left > 1000000 || crop.top > 1000000 || crop.width < 1 ||
        crop.height < 1 || crop.width > kJerseyMaximumCropExtent || crop.height > kJerseyMaximumCropExtent)
      return cudaErrorInvalidValue;
    arguments.values[index] = crop;
  }
  TeamResample<<<dim3((kTeamInputWidth + 15) / 16, (kTeamInputHeight + 15) / 16, batch), dim3(16, 16), 0, stream>>>(
      image, arguments, input);
  const auto result = cudaGetLastError();
  if (result != cudaSuccess)
    return result;
  TeamLightnessReduce<<<batch, 256, 0, stream>>>(input, observations);
  return cudaGetLastError();
}

cudaError_t DecodeJersey(
    const float* logits,
    size_t batch,
    const JerseyVocabulary& vocabulary,
    JerseyObservation* observations,
    cudaStream_t stream) {
  if (!logits || !observations || !stream || batch == 0 || batch > kMaximumBatch || vocabulary.eos_index < 0 ||
      vocabulary.eos_index >= kJerseyTokens || vocabulary.digits[vocabulary.eos_index] != -1)
    return cudaErrorInvalidValue;
  for (int digit : vocabulary.digits)
    if (digit < -1 || digit > 9)
      return cudaErrorInvalidValue;
  JerseyDecode<<<batch, 128, 0, stream>>>(logits, vocabulary, observations);
  return cudaGetLastError();
}

cudaError_t ReduceAction(
    const float* logits,
    size_t batch,
    ActionObservation* observations,
    cudaStream_t stream,
    size_t labels) {
  if (!logits || !observations || !stream || batch == 0 || batch > kMaximumBatch)
    return cudaErrorInvalidValue;
  if (labels == 4)
    ActionReduce<4><<<batch, 64, 0, stream>>>(logits, observations);
  else if (labels == kActionLabels)
    ActionReduce<kActionLabels><<<batch, 64, 0, stream>>>(logits, observations);
  else
    return cudaErrorInvalidValue;
  return cudaGetLastError();
}
} // namespace hm::player_analytics
