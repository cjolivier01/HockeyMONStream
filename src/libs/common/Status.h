#pragma once

#include "absl/status/status_or.h"

#include <utility>

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

// HM_ASSIGN_OR_RETURN: Evaluates an expression that returns an absl::StatusOr<T>.
// If the result is not ok, returns the error status from the current function.
// Otherwise, moves the contained value into the provided variable.
#define HM_ASSIGN_OR_RETURN(lhs, rexpr) \
  HM_ASSIGN_OR_RETURN_IMPL(HM_CONCAT(_hm_status_or_value, __COUNTER__), lhs, rexpr)

#define HM_ASSIGN_OR_RETURN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                             \
  if (!statusor.ok()) {                                \
    return statusor.status();                          \
  }                                                    \
  lhs = std::move(statusor).value();
