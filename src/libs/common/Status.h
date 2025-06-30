#pragma once

/* clang-format off */
#include <X11/Xlib.h>
#undef Status
/* clang-format on */

#include "absl/status/status.h"
#include "cupano/cuda/cudaStatus.h"
#include "hstream/src/libs/common/utils.h"

// HM_RETURN_IF_ERROR: Evaluates an expression that returns absl::Status,
// and returns from the current function if the status is not ok.
#define HM_RETURN_IF_ERROR(expr)           \
  do {                                     \
    const ::absl::Status _status = (expr); \
    if (!_status.ok())                     \
      return _status;                      \
  } while (0)

// Helpers to generate unique variable names.
#define HM_CONCAT_IMPL(x, y) x##y
#define HM_CONCAT(x, y) HM_CONCAT_IMPL(x, y)

// HM_ASSIGN_OR_RETURN: Evaluates an expression that returns an
// absl::StatusOr<T>. If the result is not ok, returns the error status from the
// current function. Otherwise, moves the contained value into the provided
// variable.
#define HM_ASSIGN_OR_RETURN(lhs, rexpr) \
  HM_ASSIGN_OR_RETURN_IMPL(HM_CONCAT(_hm_status_or_value, __COUNTER__), lhs, rexpr)

#define HM_ASSIGN_OR_RETURN_IMPL(statusor, lhs, rexpr) \
  do {                                                 \
    auto statusor = (rexpr);                           \
    if (!statusor.ok()) {                              \
      return statusor.status();                        \
    }                                                  \
    lhs = std::move(statusor.value());                 \
  } while (false)

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
