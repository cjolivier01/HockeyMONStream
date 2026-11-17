#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <gst/gst.h>

#include "absl/status/status.h"

namespace hm::videoprep {

// Takes ownership of both arguments, including when configuration or activation
// fails. The caller must not leak the pool when returning a startup error.
GstBufferPool* configure_output_pool(GstBufferPool* pool, GstStructure* config);

struct OutputGpuMemory {
  uint64_t free_bytes;
  uint64_t total_bytes;
};

// Reports the actual negotiated pool configuration, rather than input sizes or
// pre-calibration canvas hints. A memory snapshot supplies context, not proof
// that an arbitrary allocation/flow failure was caused by memory exhaustion.
absl::Status output_buffer_failure_status(
    GstBufferPool* pool,
    GstFlowReturn flow,
    unsigned gpu,
    const std::optional<OutputGpuMemory>& memory,
    const std::string& memory_error);

} // namespace hm::videoprep
