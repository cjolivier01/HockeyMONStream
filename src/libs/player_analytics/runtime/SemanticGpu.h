#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

namespace hm::player_analytics {

inline constexpr int kJerseyInputWidth = 128;
inline constexpr int kJerseyInputHeight = 32;
inline constexpr size_t kJerseyInputElements = 3 * kJerseyInputWidth * kJerseyInputHeight;
inline constexpr int kJerseyMaximumCropExtent = 8192;
inline constexpr int kJerseyTokens = 95;
inline constexpr int kActionLabels = 60;

// Integer surface-pixel region. Borders outside the source image are black,
// matching PIL.Image.crop, then PIL RGB bicubic resize without reducing_gap.
struct JerseyCrop {
  int left{0};
  int top{0};
  int width{0};
  int height{0};
};

inline constexpr int kTeamInputWidth = 128;
inline constexpr int kTeamInputHeight = 256;
inline constexpr size_t kTeamInputElements = 3 * kTeamInputWidth * kTeamInputHeight;

// The crop rectangle carries no feature-specific state, so the team path takes
// the jersey one rather than declaring an identical twin. Only the band that
// produces it differs; see MakeTeamCrop.
using TeamCrop = JerseyCrop;

// Zero means unsupported batch. The caller allocates this once iff jersey is
// enabled. At batch8 the bound is ~26 MiB, independent of video canvas size.
size_t JerseyScratchBytes(size_t maximum_batch) noexcept;

struct TeamObservation {
  // Mean CIELAB L of the resized crop, 0..100. The embedding has no
  // photometric axis, so ranking the two clusters dark-vs-light needs this
  // measured separately from the pixels the model actually saw.
  float lightness{0};
};

// Resize each crop to 256x128 and apply the reidentificationnet-rgb-bilinear-v1
// preprocessing: half-pixel bilinear (torch align_corners=false), then
// (pixel - offset) * scale per channel. Also returns each crop's mean
// lightness, which costs one extra pass over the already-resized batch.
//
// Unlike the jersey path there is no scratch buffer and no coefficient pass:
// plain bilinear is a 2x2 gather, so each output pixel is computed directly
// from the surface. Crops are bounded CPU metadata and are copied into the
// launch arguments by value, so host_crops may be reused once this returns.
// No allocation or synchronization; the caller serializes and retires every
// submitted operation before reusing or freeing the outputs, on errors too.
cudaError_t PreprocessTeam(
    const ImageView& image,
    const TeamCrop* host_crops,
    size_t batch,
    float* device_input,
    TeamObservation* device_observations,
    cudaStream_t stream);

// Crops are bounded CPU metadata, not pixels. Copies them into CUDA launch
// arguments by value; host_crops may be released/reused after this call returns.
// All crop pixels, horizontal uint8 intermediate, AA coefficients and NCHW
// output stay on GPU. No allocation or synchronization; caller serializes and
// retires every submitted operation before reusing/freeing scratch, on errors too.
// Uses PIL's a=-.5 half-pixel AA, normalized22-bit coefficients and saturating
// uint8 rounding after EACH separable pass, then pixel/127.5-1.
cudaError_t PreprocessJersey(
    const ImageView& image,
    const JerseyCrop* host_crops,
    size_t batch,
    void* device_scratch,
    size_t scratch_bytes,
    float* device_input,
    cudaStream_t stream);

struct JerseyVocabulary {
  // 0..9 for digits, -1 for other tokens. EOS is specified independently.
  int8_t digits[kJerseyTokens]{};
  int32_t eos_index{-1};
};

struct JerseyObservation {
  char text[3]{};
  uint8_t length{0}; // 0 means unknown; preserves leading zeroes.
  float confidence{0}; // Product of full-vocabulary probabilities including EOS.
};

// Greedy first-argmax over all95 tokens for each of3 positions. Requires1–2
// digit tokens followed by EOS; rejects non-digit/no-EOS/empty/nonfinite used rows.
// Vocabulary is immutable metadata built once from the actual manifest.
cudaError_t DecodeJersey(
    const float* device_logits,
    size_t batch,
    const JerseyVocabulary& vocabulary,
    JerseyObservation* device_results,
    cudaStream_t stream);

struct ActionObservation {
  int32_t label{-1};
  float confidence{0};
};

// Stable softmax over all60 classes; first argmax wins ties. A nonfinite logit
// makes this observation unknown. This helper owns no history or label gating.
cudaError_t ReduceAction(
    const float* device_logits,
    size_t batch,
    ActionObservation* device_results,
    cudaStream_t stream);

} // namespace hm::player_analytics
