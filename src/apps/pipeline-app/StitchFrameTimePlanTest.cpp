#include "src/apps/pipeline-app/StitchFrameTimePlan.h"

#include "hstream/src/libs/common/utils.h"

#include <iostream>
#include <limits>
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
  ok &= expect(
      hm::pipeline_internal::stitch_frame_rewind_request_is_current(
          /*request_stage=*/2,
          /*request_generation=*/4,
          /*current_stage=*/2,
          /*current_generation=*/4,
          /*main_loop_active=*/true) &&
          !hm::pipeline_internal::stitch_frame_rewind_request_is_current(2, 4, 3, 4, true) &&
          !hm::pipeline_internal::stitch_frame_rewind_request_is_current(2, 4, 2, 5, true) &&
          !hm::pipeline_internal::stitch_frame_rewind_request_is_current(2, 4, 2, 4, false),
      "Deferred rewinds must belong to the active stage and main-loop generation");
  ok &= expect(
      !hm::pipeline_internal::stitch_frame_should_account_playback(/*calibration_rewind_pending=*/true) &&
          hm::pipeline_internal::stitch_frame_should_account_playback(/*calibration_rewind_pending=*/false),
      "Calibration-frame playback must not consume time-limit or progress accounting");
  ok &= expect(
      hm::pipeline_internal::stitch_output_rotations_are_consistent({}) &&
          hm::pipeline_internal::stitch_output_rotations_are_consistent({0.0, -0.0}) &&
          hm::pipeline_internal::stitch_output_rotations_are_consistent({10.0, 10.0}) &&
          !hm::pipeline_internal::stitch_output_rotations_are_consistent({0.0, 10.0}) &&
          !hm::pipeline_internal::stitch_output_rotations_are_consistent(
              {10.0, std::numeric_limits<double>::quiet_NaN()}),
      "Every active stitcher in a stage must use the same finite output rotation generation input");
  ok &= expect(
      hm::hhmmss_to_nanoseconds("00:60:00") == 3'600'000'000'000ULL,
      "The existing start-time parser must retain rollover compatibility");
  ok &= expect(
      hm::stitch_frame_time_to_nanoseconds("00:00:07") == 7'000'000'000ULL &&
          hm::stitch_frame_time_to_nanoseconds("00:10:07.500") == 607'500'000'000ULL,
      "Stitch-frame parsing must retain valid whole-second and fractional timestamps");
  for (const char* invalid : {
           "",
           "bogus",
           "00:00:07junk",
           "-1",
           "00:60:00",
           "00:00:60",
           "24:00:00",
           "nan",
           "inf",
       }) {
    bool rejected = false;
    try {
      (void)hm::stitch_frame_time_to_nanoseconds(invalid);
    } catch (const std::exception&) {
      rejected = true;
    }
    ok &= expect(rejected, "Malformed stitch-frame timestamps must be rejected without partial parsing");
  }
  return ok ? 0 : 1;
}
