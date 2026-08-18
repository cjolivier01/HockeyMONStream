#include "src/apps/pipeline-app/StitchFrameTimePlan.h"

#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  bool ok = true;
  constexpr uint64_t kStartTimeNs = 2;
  constexpr uint64_t kStitchFrameTimeNs = 7;
  ok &= expect(
      hm::pipeline_internal::stitch_frame_initial_position(
          kStartTimeNs, kStitchFrameTimeNs, /*calibration_required=*/true, /*rewind_complete=*/false) ==
              kStitchFrameTimeNs &&
          hm::pipeline_internal::stitch_frame_initial_position(
              kStartTimeNs, kStitchFrameTimeNs, /*calibration_required=*/false, /*rewind_complete=*/false) ==
              kStartTimeNs &&
          hm::pipeline_internal::stitch_frame_initial_position(
              kStartTimeNs, kStitchFrameTimeNs, /*calibration_required=*/true, /*rewind_complete=*/true) ==
              kStartTimeNs,
      "Only an unfinished one-pass calibration should start at stitch-frame time");

  const std::vector<hm::pipeline_internal::StitchFrameRewindState> states = {
      {true, false, false},
      {true, false, false},
      {false, false, false},
      {true, true, false},
      {true, false, true},
  };
  const std::vector<size_t> candidates =
      hm::pipeline_internal::stitch_frame_rewind_candidates(kStitchFrameTimeNs, states);
  ok &= expect(
      candidates == std::vector<size_t>({0, 1}),
      "One completion message must rewind every active context that started at stitch-frame time");
  ok &= expect(
      hm::pipeline_internal::stitch_frame_rewind_candidates(/*stitch_frame_time_ns=*/0, states).empty(),
      "A default stitch-frame time must never schedule a rewind");
  return ok ? 0 : 1;
}
