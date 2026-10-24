#pragma once

/* clang-format off */
#include <cuda_egl_interop.h>
#include <GL/glew.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Status

/* clang-format on */

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "cupano/cuda/cudaStatus.h"
#include "hstream/src/libs/common/utils.h"

#include "hstream/src/libs/common/StatusMacros.h"

#define HM_CUDA_ASSIGN_OR_RETURN(lhs, rexpr) \
  HM_CUDA_ASSIGN_OR_RETURN_IMPL(HM_CONCAT(_hm_status_or_value, __COUNTER__), lhs, rexpr)

#define HM_CUDA_ASSIGN_OR_RETURN_IMPL(statusor, lhs, rexpr) \
  do {                                                      \
    auto statusor = (rexpr);                                \
    if (!statusor.ok()) {                                   \
      return to_status(statusor.status());                  \
    }                                                       \
    lhs = std::move(statusor.ConsumeValueOrDie());          \
  } while (false)

namespace hm {
inline absl::Status to_status(const CudaStatus& s, const char* prefix = nullptr) {
  if (s.ok()) {
    return absl::OkStatus();
  }
  if (prefix && *prefix) {
    return absl::Status(absl::StatusCode::kFailedPrecondition, std::string(prefix) + ": " + s.message());
  }
  return absl::Status(absl::StatusCode::kFailedPrecondition, s.message());
}

#define XCUDA_RETURN_IF_ERROR(rexpr)          \
  do {                                        \
    cudaError_t _cerr$ = (rexpr);             \
    if (_cerr$ != cudaError_t::cudaSuccess) { \
      return hm::to_status(_cerr$, #rexpr);   \
    }                                         \
  } while (false)

inline absl::Status to_status(const cudaError_t& status) {
  if (status == cudaError_t::cudaSuccess) {
    return absl::OkStatus();
  }
  return absl::InternalError(TO_STRING("CUDA Error: " << cudaGetErrorString(status)));
}

} // namespace hm
