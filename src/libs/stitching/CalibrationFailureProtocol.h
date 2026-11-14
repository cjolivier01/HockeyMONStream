#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/CalibrationFailure.h"

namespace hm::stitching::failure {
inline constexpr char kBindingEnvironment[] = "HSTREAM_FAILURE_BINDING";
inline constexpr char kScopePrefix[] = "HSTREAM_FAILURE_SCOPE ";
inline constexpr char kContextPrefix[] = "HSTREAM_FAILURE_CONTEXT ";
inline constexpr std::size_t kMaximumRecordBytes = 4096;
inline constexpr std::size_t kMaximumLineBytes = 65536;
inline constexpr std::size_t kMaximumLogBytes = 16 * 1024 * 1024;
inline constexpr unsigned kMaximumGenerations = 32;
bool HexIdentity(const std::string& value, std::size_t size);
absl::Status ValidateContext(const Context& context);
absl::StatusOr<std::string> ScopeRecord(
    const std::string& binding,
    const std::string& invocation,
    unsigned generation,
    int stage,
    unsigned instances);
absl::StatusOr<std::string> ContextRecord(
    const std::string& binding,
    const std::string& invocation,
    unsigned generation,
    const Context& context);

// One instance per admitted process invocation. Ordinary native stages and
// configuration generations do not reset evidence or solve/restart outcomes.
class Session {
 public:
  explicit Session(std::string binding = {}) : binding_(std::move(binding)) {}
  absl::Status Record(const std::string& line);
  Diagnosis Analyze(const std::string& terminal, const Context* fallback = nullptr, const std::string& stage = {})
      const;
  const Evidence& evidence() const {
    return evidence_;
  }
  const std::string& invocation() const {
    return invocation_;
  }
  const std::string& error() const {
    return error_;
  }
  bool has_context() const {
    return !contexts_.empty();
  }
  const std::map<std::uint64_t, std::string>& records() const {
    return records_;
  }

 private:
  std::string binding_, invocation_, error_;
  std::uint64_t generation_{};
  Evidence evidence_;
  std::map<std::uint64_t, Context> contexts_;
  std::map<std::uint64_t, std::string> records_;
};
} // namespace hm::stitching::failure
