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

bool physical_wrap_boundary() {
  using namespace hm::pipeline;
  const PlayerFrameScanPairPosition anchor{{{11, 3120000000}, {22, 3100000000}}};
  bool ok = expect(
      !PlayerFrameScanReachedAnchor({{{10, 9000000000}, {21, 9000000000}}}, anchor),
      "PTS in earlier chapter files must not trigger the physical boundary");
  ok &= expect(
      PlayerFrameScanReachedAnchor({{{11, 3080000000}, {22, 3100000000}}}, anchor),
      "either camera reaching its anchor stops scanning despite independent offsets");
  ok &= expect(
      PlayerFrameScanReachedAnchor({{{11, 3200000000}, {22, 3080000000}}}, anchor),
      "variable frame gaps that jump past the anchor must also stop scanning");
  // Fractional seeks at 25 fps retain 3.120 s for requested 3.131 s and
  // 0 s for requested .001 s. The nominal 3.130 s span includes a duplicate.
  PlayerFrameScanTiming timing(3130000000, kSecond / 2);
  for (uint64_t pts = 0; pts <= 3120000000; pts += 40000000) {
    const bool boundary =
        PlayerFrameScanReachedAnchor({{{11, pts}, {22, pts > 20000000 ? pts - 20000000 : 0}}}, anchor);
    const auto selected = timing.Select(pts, boundary);
    ok &= expect(selected.ok(), "fractional wrapped timeline stays valid");
    if (selected.ok() && *selected) {
      if (boundary) {
        ok &= expect(
            timing.AtBoundary(pts) && !timing.progress().boundary_observed,
            "physical boundary enters inference without being scored or completing early");
      } else {
        ok &=
            expect(pts < anchor[0].source_pts && !timing.AtBoundary(pts), "only unique physical frames can be scored");
      }
      ok &= expect(timing.Observe(pts).ok(), "validated inferred frames advance wrap progress");
    }
  }
  ok &= expect(timing.progress().boundary_observed, "physical anchor completes the shorter wrapped span");
  const auto beyond = timing.Select(3160000000);
  ok &= expect(beyond.ok() && !*beyond, "frames after physical boundary never enter inference");
  PlayerFrameScanTiming immediate(1000000, kSecond / 2);
  const auto first = immediate.Select(3120000000, true);
  ok &= expect(
      first.ok() && *first && immediate.AtBoundary(3120000000) && !immediate.progress().boundary_observed,
      "immediate overlap still requires an inferred boundary, even with zero new scored frames");
  ok &= expect(
      immediate.Observe(3120000000).ok() && immediate.progress().boundary_observed,
      "validated immediate overlap completes without scoring its duplicated anchor");
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
  return cold_start_at_nonzero_seek() && boundary_waits_for_inference() && physical_wrap_boundary() &&
          cancellation_before_first_frame()
      ? 0
      : 1;
}
