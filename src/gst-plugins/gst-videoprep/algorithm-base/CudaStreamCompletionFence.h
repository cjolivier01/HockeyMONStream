#pragma once

#include <cuda_runtime.h>
#include <cstdlib>

namespace hm {
namespace videoprep {

using CudaStreamSynchronizer = cudaError_t (*)(cudaStream_t);

// Once output work has been queued, every return path must synchronize before
// the output buffer can be recycled by the surrounding GStreamer pool.
class CudaStreamCompletionFence {
 public:
  explicit CudaStreamCompletionFence(
      cudaStream_t stream,
      CudaStreamSynchronizer synchronizer = cudaStreamSynchronize) noexcept
      : stream_(stream), synchronizer_(synchronizer) {}
  ~CudaStreamCompletionFence() noexcept {
    // Returning a possibly in-flight surface to the pool is memory-unsafe. A
    // persistent synchronization failure is therefore process-fatal.
    if (Synchronize() != cudaSuccess)
      std::_Exit(88);
  }
  CudaStreamCompletionFence(const CudaStreamCompletionFence&) = delete;
  CudaStreamCompletionFence& operator=(const CudaStreamCompletionFence&) = delete;

  void MarkSubmitted() noexcept {
    submitted_ = true;
  }
  cudaError_t Synchronize() noexcept {
    if (!submitted_)
      return cudaSuccess;
    const cudaError_t result = synchronizer_ ? synchronizer_(stream_) : cudaErrorInvalidValue;
    if (result == cudaSuccess)
      submitted_ = false;
    return result;
  }

 private:
  cudaStream_t stream_{nullptr};
  CudaStreamSynchronizer synchronizer_{nullptr};
  bool submitted_{false};
};

} // namespace videoprep
} // namespace hm
