#pragma once

#include <hip/hip_runtime_api.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::gpu {

struct ModelInputShape {
  std::string name;
  std::vector<size_t> dimensions;
};

// Static-shape FP32 input/output bindings for an AMD ONNX program. Internal
// weights/operations can use FP16. No tensor upload or readback is implicit.
class MigraphxSession {
 public:
  enum class Precision { kFloat32, kFloat16 };
  struct Tensor {
    float* data;
    std::vector<size_t> dimensions;
    size_t elements;
  };

  // Compilation belongs to preparation/startup, never a video callback. Every
  // ONNX input must have an explicit positive shape. I/O allocations share the
  // supplied byte bound; MIGraphX owns its compiled constants and workspace.
  static absl::StatusOr<std::unique_ptr<MigraphxSession>> Compile(
      const std::filesystem::path& onnx,
      const std::vector<ModelInputShape>& inputs,
      Precision precision,
      int device,
      size_t maximum_io_bytes);
  ~MigraphxSession();
  MigraphxSession(const MigraphxSession&) = delete;
  MigraphxSession& operator=(const MigraphxSession&) = delete;

  size_t input_count() const;
  const Tensor& input(size_t index) const;
  size_t output_count() const;
  // Valid after a successful Enqueue; output storage may alias an input for
  // identity graphs. A following enqueue overwrites the same owned bindings.
  const Tensor& output(size_t index) const;

  // One serialized caller and one non-default stream, bound on first enqueue.
  // Input producers and output consumers must run on that stream. It must
  // outlive the session. Errors retire the session; destruction drains work
  // before releasing bindings, including after a partially submitted enqueue.
  absl::Status Enqueue(hipStream_t stream);

 private:
  struct Impl;
  explicit MigraphxSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::gpu
