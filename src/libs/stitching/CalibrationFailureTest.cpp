#include "hstream/src/libs/stitching/CalibrationFailureProtocol.h"

#include <cstdlib>
#include <iostream>

namespace failure = hm::stitching::failure;
namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
failure::Context context(int width = 8192) {
  failure::Context value;
  value.matcher = "akaze-hamming";
  value.mapping_backend = "opencv-magsac";
  value.projection = "rectilinear";
  value.frame_count = 4;
  value.control_point_limit = value.default_control_point_limit = 1500;
  value.max_output_width = width;
  value.camera = value.default_camera = {"gopro-mission-1", 127.2, 95};
  value.calibration_required = true;
  value.calibration_start_stage = "input";
  return value;
}
const std::string binding(64, 'a'), invocation(32, 'b');
void scope(failure::Session& session, unsigned generation, const failure::Context& value) {
  const auto start = failure::ScopeRecord(binding, invocation, generation, 0, 1);
  const auto record = failure::ContextRecord(binding, invocation, generation, value);
  check(start.ok() && record.ok(), "Context must serialize");
  check(session.Record(*start).ok() && session.Record(*record).ok(), "Bound context must round-trip");
}
void line(failure::Session& session, const std::string& value) {
  check(session.Record(value).ok(), "Evidence rejected");
}
bool has(const std::string& value, const std::string& part) {
  return value.find(part) != std::string::npos;
}
} // namespace
int main() {
  failure::Session session(binding);
  scope(session, 1, context());
  line(session, "Cuda failure: status=2");
  line(session, "ERROR from hmstitcher_conv0: failed to activate bufferpool");
  line(
      session,
      "HSTREAM_CALIBRATION stage=calibration status=failed message=Pipeline failed during stitching calibration");
  for (int i = 0; i < 30; ++i)
    line(session, "INTERNAL: noisy teardown " + std::to_string(i));
  auto result = session.Analyze("INTERNAL: App run failed");
  check(
      result.category == "cuda-oom-input-pool" && result.cause_generation == 1 &&
          has(result.action, "try 4096 pixels (half)") && has(result.action, "does not reduce the pre-stitch"),
      "Input-pool OOM or width guidance lost");
  check(result.evidence.size() == 6 && result.applicability == "calibration", "Evidence bound/applicability changed");
  scope(session, 2, context(16384));
  line(session, "HSTREAM_CALIBRATION stage=calibration status=complete");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=failed message=Playback could not restart");
  line(session, "CUDA error at cudaBlend.cu:875: out of memory");
  result = session.Analyze("App run failed");
  check(
      result.category == "cuda-oom-blender" && result.cause_generation == 2 &&
          result.applicability == "playback-restart" && result.solve_outcome == "complete" &&
          result.restart_outcome == "failed" && has(result.action, "try 8192 pixels (half)"),
      "Solve completion must not suppress restart failure");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=complete");
  result = session.Analyze("PERMISSION_DENIED: archive publication failed");
  check(result.applicability == "none", "Completed solve/restart must defeat unrelated later output errors");

  session = failure::Session(binding);
  scope(session, 1, context());
  line(session, "Rejected calibrated MAGSAC hypothesis 1: FAILED_PRECONDITION: unsafe canvas extent");
  line(session, "ERROR: first no-progress watchdog expired");
  line(session, "Cuda failure: status=2");
  scope(session, 2, context(4096));
  line(session, "HSTREAM_ONNX_FALLBACK provider=cpu reason=cuda-out-of-memory model=/private/model.onnx");
  line(session, "ERROR: later no-progress watchdog expired");
  result = session.Analyze("INTERNAL: App run failed");
  check(
      result.category == "watchdog" && has(result.reported_cause, "first") && result.cause_generation == 1 &&
          result.rejected_hypotheses == 1,
      "Fallback/generation must preserve first watchdog and counters");
  line(session, "Cuda failure: status=2");
  result = session.Analyze("INTERNAL: App run failed");
  check(
      result.category == "cuda-oom" && result.cause_generation == 2 && has(result.action, "do not reduce it further"),
      "Fresh OOM must outrank watchdog with its own context");

  session = failure::Session(binding);
  scope(session, 1, context());
  line(session, "Cuda failure: status=2");
  scope(session, 2, context(2048));
  line(session, "ERROR from hmstitcher_conv0: failed to activate bufferpool");
  result = session.Analyze("App run failed");
  check(
      result.category == "cuda-oom" && result.cause_generation == 1,
      "Different generations must not combine allocation flags");
  line(session, "HSTREAM_ONNX_FALLBACK provider=cpu reason=cuda-out-of-memory model=/private/model.onnx");
  line(session, "FAILED_PRECONDITION: No stitching calibration frame pair produced usable matches");
  result = session.Analyze("App run failed");
  check(
      result.category == "overlap" && !has(result.reported_cause, "CUDA"),
      "Recovered OOM must not mask later model/geometry failure");

  for (const int width : {0, 16384, 8192, 6000, 4096, 2048, 16385}) {
    failure::Session sample(binding);
    scope(sample, 1, context(width));
    line(sample, "Cuda failure: status=2");
    const auto advice = sample.Analyze("failed").action;
    check(width != 0 || has(advice, "was Auto"), "Auto width changed");
    check(width != 6000 || has(advice, "4096 pixels (the lowest reasonable value)"), "Width floor changed");
    check(width != 16385 || has(advice, "8192 pixels (half, rounded down)"), "Odd width rounding changed");
    check(width > 4096 || !has(advice, "try 2048"), "Advice crossed existing width floor");
  }
  auto selected = context();
  selected.control_point_limit = 250;
  selected.camera_differs_from_defaults = true;
  selected.camera.configuration = "gopro-hero-11";
  session = failure::Session(binding);
  scope(session, 1, selected);
  result = session.Analyze("No stitching calibration frame pair produced a usable solution");
  check(
      result.category == "selected-geometry" && has(result.action, "CP limit is 250") &&
          has(result.action, "gopro-hero-11"),
      "Frozen selected/default guidance changed");
  for (const auto& error :
       {"NOT_FOUND: HM_NONA is not executable: /bad/nona",
        "PERMISSION_DENIED: publish stitch artifact failed",
        "INVALID_ARGUMENT: unsafe canvas extent"}) {
    result = session.Analyze(error);
    check(
        result.reported_cause == error && result.category != "selected-geometry",
        "Specific final cause must outrank generic selected-settings fallback");
  }

  session = failure::Session(binding);
  line(session, *failure::ScopeRecord(binding, invocation, 1, 0, 1));
  line(session, "Cuda failure: status=2");
  line(session, *failure::ContextRecord(binding, invocation, 1, context(16384)));
  line(session, "HSTREAM_CALIBRATION stage=calibration status=failed message=App run failed");
  result = session.Analyze("App run failed");
  check(
      result.category == "cuda-oom" && result.cause_generation == 0 &&
          result.configuration == "Native launch context unavailable" && !has(result.action, "16384"),
      "Pre-context cause inherited later configuration guidance");
  line(session, *failure::ScopeRecord(binding, invocation, 2, 0, 1));
  line(session, "ERROR from hmstitcher_conv0: failed to activate bufferpool");
  result = session.Analyze("App run failed");
  check(result.category == "cuda-oom", "Unbound evidence combined allocation flags across generations");

  session = failure::Session(binding);
  scope(session, 1, selected);
  line(session, *failure::ScopeRecord(binding, invocation, 2, 0, 1));
  result = session.Analyze("No stitching calibration frame pair produced a usable solution");
  check(
      result.cause_generation == 0 && result.category != "selected-geometry" &&
          result.configuration == "Native launch context unavailable",
      "Missing successor context inherited preceding configuration");

  session = failure::Session(binding);
  scope(session, 1, context());
  line(session, "HSTREAM_CALIBRATION stage= status=failed message=ignored");
  check(session.Analyze("failed").solve_outcome == "not-observed", "Empty stage mutated native outcome");
  line(session, "HSTREAM_CALIBRATION stage=calibration status=complete");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=started");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=skipped");
  result = session.Analyze("PERMISSION_DENIED: archive publication failed");
  check(
      result.restart_outcome == "pending" && result.applicability == "none",
      "Missing restart completion was inferred as failure");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=failed message=Could not resume playback");
  check(
      session.Analyze("App run failed").stage == "Restarting playback",
      "Restart failure retained the preceding solve stage");
  line(session, "HSTREAM_CALIBRATION stage=playback-restart status=complete");
  line(session, "HSTREAM_CALIBRATION stage=calibration status=complete");
  check(session.Analyze("failed").restart_outcome == "complete", "Late solve completion reset completed restart");

  // Native progress includes substeps that are not separate rows in the main
  // calibration dialog. They must still replace the preceding failed stage.
  for (const auto& [stage, label] : std::map<std::string, std::string>{
           {"input", "Wait for synchronized camera frames"},
           {"orientation", "Find the ice rink and orient cameras"},
           {"features", "Look for control points"},
           {"matching", "Match control points"},
           {"optimizer", "Run panorama optimizer (autooptimiser)"},
           {"projection", "Apply panorama projection"},
           {"leveling", "Optionally level the rink"},
           {"canvas", "Build stitch maps and panorama"},
           {"rink-mask", "Find the ice surface"},
           {"materialize", "Prepare selected calibration frames"}}) {
    session = failure::Session(binding);
    scope(session, 1, context());
    line(session, "HSTREAM_CALIBRATION stage=optimizer status=started");
    line(session, "HSTREAM_CALIBRATION stage=optimizer status=complete");
    line(session, "HSTREAM_CALIBRATION stage=" + stage + " status=started");
    line(session, "HSTREAM_CALIBRATION stage=calibration status=failed message=Native substep failed");
    result = session.Analyze("App run failed");
    check(result.stage == label && result.solve_outcome == "failed", "Native calibration substep was ignored");
  }

  session = failure::Session(binding);
  scope(session, 1, context());
  line(session, std::string(65000, 'x') + " INVALID_ARGUMENT: unsafe canvas extent");
  result = session.Analyze("App run failed");
  check(
      result.category == "unsafe-geometry" && result.reported_cause.size() == 4096 &&
          result.evidence.front().size() == 4096,
      "Display truncation ran before full bounded evidence classification");

  session = failure::Session(binding);
  scope(session, 1, context());
  check(!session.Record(std::string(failure::kContextPrefix) + "{}").ok(), "Malformed context accepted");
  check(
      !session.Record("HSTREAM_CALIBRATION stage=calibration status=failed message=original native failure").ok(),
      "Protocol rejection was not permanent");
  result = session.Analyze("App run failed");
  check(
      result.solve_outcome == "failed" && result.reported_cause == "original native failure" &&
          result.configuration == "Native launch context unavailable",
      "Optional malformed protocol replaced the original outcome");

  const auto good = *failure::ScopeRecord(binding, invocation, 1, 0, 1);
  session = failure::Session(binding);
  check(session.Record(good).ok() && !session.Record(good).ok(), "Repeated scope accepted");
  session = failure::Session(binding);
  auto duplicate = good;
  duplicate.insert(duplicate.find('{') + 1, "\"version\":1,");
  check(!session.Record(duplicate).ok(), "Duplicate JSON fields accepted");
  session = failure::Session(binding);
  check(
      !session.Record(*failure::ScopeRecord(std::string(64, 'c'), invocation, 1, 0, 1)).ok(),
      "Foreign binding accepted");
  session = failure::Session(binding);
  line(session, good);
  check(
      !session.Record(*failure::ScopeRecord(binding, std::string(32, 'd'), 2, 0, 1)).ok(),
      "Second invocation accepted");
  session = failure::Session(binding);
  check(!session.Record(*failure::ContextRecord(binding, invocation, 1, context())).ok(), "Unscoped context accepted");
  session = failure::Session(binding);
  check(
      !session.Record(*failure::ScopeRecord(binding, invocation, 1, 0, 2)).ok(),
      "Ambiguous multiple instances accepted");
  session = failure::Session(binding);
  check(!session.Record(std::string(failure::kMaximumLineBytes + 1, 'x')).ok(), "Oversized line accepted");
  check(
      !failure::IsCudaOutOfMemory("Cuda failure: status=20") &&
          !failure::IsCudaOutOfMemory("CUDA error at cudaBlend.cu: illegal memory access"),
      "Non-OOM CUDA error misclassified");
  std::cout << "Calibration failure analysis and scoped protocol passed\n";
}
