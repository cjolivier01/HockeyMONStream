#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hm::stitching::failure {
// A projection of values already resolved by the owning native invocation.
// Zero-initialization is not a configuration default: the protocol requires
// every field and validates it before accepting a context.
struct Camera {
  std::string configuration;
  double horizontal_fov{};
  double vertical_fov{};
};
struct Context {
  std::string matcher, mapping_backend, projection;
  int frame_count{}, control_point_limit{}, max_output_width{};
  Camera camera, default_camera;
  int default_control_point_limit{};
  bool camera_differs_from_defaults{}, calibration_required{}, calibration_only{};
  std::string calibration_start_stage;
};
struct Line {
  std::string text;
  std::uint64_t generation{};
  bool context_bound{};
};
struct Allocation {
  bool oom{}, input_pool{}, blender{};
};
struct Diagnosis {
  std::string category, stage, configuration, reported_cause, explanation, action, rendered_text;
  std::string solve_outcome, restart_outcome, applicability;
  std::uint64_t cause_generation{};
  unsigned rejected_hypotheses{}, rejected_candidates{};
  std::vector<std::string> evidence;
};
bool IsCudaOutOfMemory(const std::string& line);
bool IsStallWatchdog(const std::string& line);
std::string StageLabel(const std::string& stage);

class Evidence {
 public:
  void Record(const std::string& line, std::uint64_t generation = 0, bool context_bound = false);
  void AdvanceGeneration(std::uint64_t generation, bool context_bound = false) {
    latest_generation_ = generation;
    latest_context_bound_ = context_bound;
  }
  Diagnosis Analyze(
      const std::string& terminal,
      const std::map<std::uint64_t, Context>& contexts = {},
      const Context* fallback = nullptr,
      const std::string& stage_override = {}) const;
  bool CudaOutOfMemory() const;
  bool BlenderOutOfMemory() const;
  bool InputPoolFailure() const;
  const std::string& Watchdog() const {
    return watchdog_.text;
  }
  unsigned rejected_hypotheses() const {
    return rejected_hypotheses_;
  }
  unsigned rejected_candidates() const {
    return rejected_candidates_;
  }

 private:
  std::map<std::pair<std::uint64_t, bool>, Allocation> allocations_;
  std::vector<Line> recent_;
  Line watchdog_, terminal_;
  std::string stage_, solve_{"not-observed"}, restart_{"not-observed"};
  std::uint64_t latest_generation_{};
  bool latest_context_bound_{};
  unsigned rejected_hypotheses_{}, rejected_candidates_{};
};
} // namespace hm::stitching::failure
