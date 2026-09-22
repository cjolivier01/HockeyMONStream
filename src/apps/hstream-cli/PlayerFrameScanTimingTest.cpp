#include "PlayerFrameScanTiming.h"

#include <iostream>

namespace {
constexpr uint64_t kSecond = 1000000000;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool cold_start_at_nonzero_seek() {
  hm::pipeline::PlayerFrameScanTiming timing(60 * kSecond, kSecond / 2);
  const uint64_t anchor = 582 * kSecond;
  bool ok = expect(
      !timing.progress().elapsed_ns && !timing.progress().boundary_observed,
      "cold startup has no scan progress to finish a timed run or arm a no-progress watchdog");
  const auto first = timing.Select(anchor);
  const auto queued = timing.Select(anchor + kSecond / 2);
  ok &= expect(first.ok() && *first && queued.ok() && *queued, "frames at a nonzero seek must enter inference");
  ok &= expect(
      !timing.progress().elapsed_ns && !timing.progress().boundary_observed,
      "source positions beyond the scan duration cannot advance progress while inference is still starting");
  ok &= expect(timing.Observe(anchor).ok(), "the first inferred frame must be accepted");
  ok &= expect(
      timing.progress().elapsed_ns == 0 && !timing.progress().boundary_observed,
      "the first observation starts scan time at zero, independent of the physical seek position");
  ok &= expect(timing.Observe(anchor + kSecond / 2).ok(), "the next inferred sample must be accepted");
  ok &= expect(timing.progress().elapsed_ns == kSecond / 2, "observed relative video time advances scan progress");
  return ok;
}

bool boundary_waits_for_inference() {
  hm::pipeline::PlayerFrameScanTiming timing(2 * kSecond, kSecond / 2);
  const uint64_t anchor = 582 * kSecond;
  bool ok = true;
  // The decoder can queue the entire window while inference/scoring is busy.
  for (uint64_t offset = 0; offset <= 2 * kSecond; offset += kSecond / 2) {
    const auto selected = timing.Select(anchor + offset);
    ok &= expect(selected.ok() && *selected, "each cadence sample and one boundary frame must enter inference");
  }
  const auto beyond = timing.Select(anchor + 3 * kSecond);
  ok &= expect(beyond.ok() && !*beyond, "later buffers must not add an inference burst after the boundary");
  ok &= expect(
      !timing.progress().elapsed_ns && !timing.progress().boundary_observed,
      "a queued boundary must not stop the pipeline while preceding samples are still in flight");
  for (uint64_t offset = 0; offset < 2 * kSecond; offset += kSecond / 2) {
    ok &= expect(timing.Observe(anchor + offset).ok(), "every in-flight sample must finish before completion");
    ok &= expect(
        timing.progress().elapsed_ns == offset && !timing.progress().boundary_observed,
        "only completed inference/scoring advances progress");
  }
  const auto boundary_elapsed = timing.Elapsed(anchor + 2 * kSecond);
  ok &= expect(boundary_elapsed.ok() && *boundary_elapsed == 2 * kSecond, "the boundary has the expected elapsed time");
  ok &= expect(
      !timing.progress().boundary_observed,
      "reading a boundary timestamp before validating inference and mask metadata cannot complete the scan");
  ok &= expect(timing.Observe(anchor + 2 * kSecond).ok(), "the validated boundary must be accepted");
  const auto complete = timing.progress();
  ok &= expect(
      complete.boundary_observed && complete.elapsed_ns == 2 * kSecond,
      "observing the boundary completes the window after all prior samples");
  ok &= expect(
      !timing.Observe(anchor + kSecond).ok() && timing.progress().elapsed_ns == complete.elapsed_ns,
      "out-of-order inference cannot roll progress backwards");
  ok &= expect(
      hm::pipeline::PlayerFrameScanCompletedCleanly(complete, false, false),
      "a validated boundary completes a timed scan without requiring decoder EOS");
  ok &= expect(
      !hm::pipeline::PlayerFrameScanCompletedCleanly(complete, true, false) &&
          !hm::pipeline::PlayerFrameScanCompletedCleanly(complete, true, true),
      "interruption wins over a boundary or shutdown-generated EOS and must never authorize a report");
  return ok;
}

bool cancellation_before_first_frame() {
  const hm::pipeline::PlayerFrameScanProgress pending;
  return expect(
      !hm::pipeline::PlayerFrameScanCompletedCleanly(pending, false, false) &&
          !hm::pipeline::PlayerFrameScanCompletedCleanly(pending, true, false) &&
          !hm::pipeline::PlayerFrameScanCompletedCleanly(pending, true, true) &&
          hm::pipeline::PlayerFrameScanCompletedCleanly(pending, false, true),
      "only uninterrupted natural EOS can finish a scan before its inferred boundary");
}
} // namespace

int main() {
  return cold_start_at_nonzero_seek() && boundary_waits_for_inference() && cancellation_before_first_frame() ? 0 : 1;
}
