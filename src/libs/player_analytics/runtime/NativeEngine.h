#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <filesystem>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/ModelContract.h"

namespace hm::player_analytics {

absl::StatusOr<RuntimeIdentity> QueryRuntimeIdentity(int gpu_id);

// Enabled-path only. Deserializes a verified prepared bundle; never builds,
// downloads or initializes an ONNX parser. One caller serializes this context.
// The caller owns its stream and MUST retire all work (including failed enqueue
// and consumers of these buffers) before destruction. No per-enqueue allocation.
class NativeEngine {
 public:
  static absl::StatusOr<std::unique_ptr<NativeEngine>> Load(
      const std::filesystem::path& bundle_directory,
      ModelFeature expected_feature,
      int gpu_id,
      size_t required_batch);
  ~NativeEngine();
  NativeEngine(const NativeEngine&) = delete;
  NativeEngine& operator=(const NativeEngine&) = delete;

  const ModelManifest& manifest() const noexcept;
  int gpu_id() const noexcept;
  size_t maximum_batch() const noexcept;
  float* input() noexcept;
  const float* output(size_t index) const noexcept;
  size_t input_elements_per_sample() const noexcept;
  size_t output_elements_per_sample(size_t index) const noexcept;
  // Requires a non-default caller stream on gpu_id(). A false enqueue may still
  // have submitted GPU work: the caller's error path must synchronize the stream.
  absl::Status Enqueue(size_t batch, cudaStream_t stream);

 private:
  struct Impl;
  explicit NativeEngine(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::player_analytics
