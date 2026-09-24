#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

namespace fs = std::filesystem;
using namespace hm::stitching;
namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

PlayerFramePairIdentity pair(uint64_t second, const fs::path& left, const fs::path& right) {
  PlayerFramePairIdentity result;
  result.cameras[0] = {left.string(), second * kPlayerFrameSecond, 0, second * 30};
  result.cameras[1] = {right.string(), second * kPlayerFrameSecond + 100, 1, second * 30};
  result.timeline_pts_ns = second * kPlayerFrameSecond;
  return result;
}

PlayerFrameSelectionContext context() {
  return {
      {"source_context", "cameras-offsets-anchor"},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask-sha"},
      {"rink_mask_revision", "output:authority"},
      {"fieldmask_settings", "center=-0.1;bottom=0.1"},
      {"output_rotation_degrees", "0"}};
}
} // namespace

int main() {
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("player-selection-test-" + std::to_string(getpid()));
  fs::create_directories(root);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(path, e);
    }
  } cleanup{root};
  const fs::path left = root / "left.mp4", right = root / "right.mp4", chapter = root / "left-chapter2.mp4";
  for (const auto& path : {left, right, chapter})
    std::ofstream(path) << "recorded-source-fixture";
  std::vector<PlayerFrameSourceBinding> bindings;
  for (const auto& path : {left, right, chapter}) {
    auto binding = BindPlayerFrameSource(path);
    if (!binding.ok()) {
      std::cerr << binding.status() << '\n';
      return 1;
    }
    bindings.push_back(*binding);
  }
  fs::create_symlink(left, root / "candidate-left.mp4");
  auto aliased = pair(0, root / "candidate-left.mp4", right);
  ok &= expect(
      CanonicalizePlayerFramePair(&aliased).ok() && aliased.cameras[0].path == left.string(),
      "private symlinks resolve to stable physical sources");

  const cv::Size canvas(1600, 900);
  cv::Mat overlap(90, 160, CV_8UC1, cv::Scalar(255));
  overlap.colRange(0, 16).setTo(0);
  std::vector<PlayerFrameObservation> observations;
  const auto add = [&](uint64_t seconds, const std::vector<PlayerFrameBox>& boxes) {
    auto scored = ScorePlayerFrame(pair(seconds, left, right), boxes, overlap, canvas);
    if (!scored.ok()) {
      std::cerr << scored.status() << '\n';
      return false;
    }
    observations.push_back(*scored);
    return true;
  };
  ok &= add(0, {});
  ok &= add(1, {{350, 300, 20, 25, .9, 0}}); // Far.
  ok &= add(2, {{350, 300, 20, 25, .8, 0}}); // Same coverage, lower confidence.
  ok &= add(3, {{700, 500, 50, 65, .8, 0}}); // Middle.
  ok &= add(4, {{1000, 500, 80, 150, .9, 0}}); // Near.
  if (observations.size() != 5)
    return 1;
  auto ignored =
      ScorePlayerFrame(pair(5, left, right), {{0, 0, 20, 30, .9, 0}, {350, 300, 20, 25, .9, 1}}, overlap, canvas);
  ok &= expect(ignored.ok() && ignored->eligible_people == 0, "off-overlap and non-person boxes do not count");
  auto bad = ScorePlayerFrame(
      pair(5, left, right), {{400, 300, 20, 30, std::numeric_limits<double>::quiet_NaN(), 0}}, overlap, canvas);
  ok &= expect(!bad.ok(), "nonfinite person evidence is an error");
  std::vector<PlayerFrameBox> crowd(256, {350, 300, 20, 25, .9, 0});
  auto crowded = ScorePlayerFrame(pair(5, left, right), crowd, overlap, canvas);
  ok &= expect(
      crowded.ok() && crowded->coverage == observations[1].coverage && crowded->quality <= 4,
      "repeated crowd boxes cannot multiply coverage or unbounded quality");
  crowd.push_back(crowd.front());
  ok &= expect(!ScorePlayerFrame(pair(5, left, right), crowd, overlap, canvas).ok(), "box count is bounded");

  PlayerFrameSelectionSettings settings;
  auto selected = SelectPlayerFrames(settings, observations, bindings, context());
  ok &= expect(selected.ok() && selected->plan.has_value(), "automatic selection produces a plan");
  if (!selected.ok() || !selected->plan)
    return 1;
  const auto& plan = *selected->plan;
  ok &= expect(
      plan.selected.size() == 4 && plan.selected[0].pair.timeline_pts_ns == 0 &&
          plan.selected[1].pair.timeline_pts_ns == kPlayerFrameSecond &&
          plan.selected[2].pair.timeline_pts_ns == 3 * kPlayerFrameSecond &&
          plan.selected[3].pair.timeline_pts_ns == 4 * kPlayerFrameSecond,
      "anchor retained and redundant far frame replaced by middle/near coverage");
  ok &= expect(
      ValidatePlayerFrameSources(plan).ok() && ValidatePlayerFrameSourceContext(plan, "cameras-offsets-anchor").ok() &&
          !ValidatePlayerFrameSourceContext(plan, "changed-offset").ok(),
      "sources and synchronization context are verified");
  auto serialized = YAML::Dump(PlayerFrameSelectionReportYaml(*selected));
  auto parsed = ParsePlayerFrameSelectionReport(YAML::Load(serialized));
  ok &= expect(
      parsed.ok() && parsed->plan && parsed->plan->fingerprint == plan.fingerprint,
      "report and plan round-trip with stable fingerprint");
  PlayerFrameSelectionPlan string_context;
  string_context.settings.frame_count = 1;
  string_context.sources = {{"/recording/left.mp4", 100, 123}, {"/recording/right.mp4", 200, 456}};
  string_context.context = context();
  string_context.context["output_rotation_degrees"] = "0.000000";
  string_context.context["decode_anchor_ns"] = "582000000000";
  string_context.context["additional_identity"] = "yes";
  PlayerFrameObservation anchor_observation;
  anchor_observation.pair = pair(0, "/recording/left.mp4", "/recording/right.mp4");
  string_context.selected.push_back(anchor_observation);
  auto string_fingerprint = PlayerFrameSelectionFingerprint(string_context);
  if (!string_fingerprint.ok())
    return 1;
  string_context.fingerprint = *string_fingerprint;
  ok &= expect(
      *string_fingerprint == "f26a1599bade86d7b13efd45b9015ad6533a4066954d7f1fe736a711648be23d",
      "context string serialization retains the original schema-1 fingerprint encoding");
  auto string_document = YAML::Load(YAML::Dump(PlayerFrameSelectionPlanYaml(string_context)));
  for (const auto& [key, value] : string_context.context) {
    ok &= expect(
        string_document["context"][key].Tag() == "tag:yaml.org,2002:str" &&
            string_document["context"][key].as<std::string>() == value,
        "persisted context preserves string types for YAML tools with scalar resolution");
  }
  auto string_parsed = ParsePlayerFrameSelectionPlan(string_document);
  ok &= expect(
      string_parsed.ok() && string_parsed->fingerprint == string_context.fingerprint,
      "explicit string tags preserve the plan fingerprint");
  // External tools such as PyYAML may replace explicit tags with quoting. A
  // later ordinary config save must retain that string type without rebuilding
  // the plan through PlayerFrameSelectionPlanYaml.
  auto quoted_document = YAML::Clone(string_document);
  for (const auto& [key, value] : string_context.context) {
    YAML::Emitter quoted;
    quoted << YAML::DoubleQuoted << value;
    quoted_document["context"][key] = YAML::Load(quoted.c_str());
  }
  for (int rewrite = 0; rewrite < 2; ++rewrite) {
    quoted_document = YAML::Load(YAML::Dump(quoted_document));
    for (const auto& [key, value] : string_context.context) {
      ok &= expect(
          quoted_document["context"][key].Tag() == "!" &&
              quoted_document["context"][key].as<std::string>() == value,
          "ordinary native re-saves retain quoted context strings from external YAML tools");
    }
    auto rewritten_plan = ParsePlayerFrameSelectionPlan(quoted_document);
    ok &= expect(
        rewritten_plan.ok() && rewritten_plan->fingerprint == string_context.fingerprint,
        "repeated native re-saves preserve the externally quoted plan fingerprint");
  }
  auto mixed_scalars =
      YAML::Load(YAML::Dump(YAML::Load("text: '0.000000'\ncount: 3\nratio: 0.5\nenabled: true\n")));
  ok &= expect(
      mixed_scalars["text"].Tag() == "!" && mixed_scalars["count"].Tag() == "?" &&
          mixed_scalars["count"].as<int>() == 3 && mixed_scalars["ratio"].Tag() == "?" &&
          mixed_scalars["ratio"].as<double>() == 0.5 && mixed_scalars["enabled"].Tag() == "?" &&
          mixed_scalars["enabled"].as<bool>(),
      "retaining string quoting does not change the types of following numeric scalars");
  for (const auto& [key, value] : string_context.context)
    string_document["context"][key].SetTag("");
  ok &= expect(
      ParsePlayerFrameSelectionPlan(YAML::Load(YAML::Dump(string_document))).ok(),
      "legacy plans without explicit string tags remain valid");
  string_document["context"]["output_rotation_degrees"] = "0.0";
  ok &= expect(!ParsePlayerFrameSelectionPlan(string_document).ok(), "changed context text still fails validation");
  auto reordered = plan;
  std::reverse(reordered.sources.begin(), reordered.sources.end());
  auto reordered_hash = PlayerFrameSelectionFingerprint(reordered);
  ok &= expect(
      reordered_hash.ok() && *reordered_hash == plan.fingerprint, "source binding order does not change fingerprint");
  auto changed_mask = plan;
  changed_mask.context["rink_mask_sha256"] = "new-mask";
  ok &= expect(!ValidatePlayerFrameSelectionPlan(changed_mask).ok(), "mask content participates in plan fingerprint");
  auto node = PlayerFrameSelectionPlanYaml(plan);
  node["schema"] = 2;
  ok &= expect(!ParsePlayerFrameSelectionPlan(node).ok(), "unknown schema rejected");
  node = PlayerFrameSelectionPlanYaml(plan);
  node["settings"]["duration_ns"] = "-1";
  ok &= expect(!ParsePlayerFrameSelectionPlan(node).ok(), "negative unsigned values rejected");
  node = PlayerFrameSelectionPlanYaml(plan);
  node["unknown"] = 1;
  ok &= expect(!ParsePlayerFrameSelectionPlan(node).ok(), "unknown fields rejected");
  settings.frame_count = 6;
  auto unavailable = SelectPlayerFrames(settings, observations, bindings, context());
  ok &= expect(
      unavailable.ok() && !unavailable->plan && !unavailable->unavailable_reason.empty(),
      "insufficient evidence remains an explicit quality outcome");
  settings.frame_count = 1;
  auto anchor = SelectPlayerFrames(settings, {observations.front()}, bindings, context());
  ok &= expect(anchor.ok() && anchor->plan && anchor->plan->selected.size() == 1, "count one needs only the anchor");
  settings.interval_ns = 1;
  ok &= expect(!ValidatePlayerFrameSelectionSettings(settings).ok(), "unbounded observation cadence rejected");

  PlayerFrameSelectionSettings spaced_settings;
  spaced_settings.frame_count = 3;
  spaced_settings.minimum_separation_ns = 2 * kPlayerFrameSecond;
  std::vector<PlayerFrameObservation> spaced{observations[0], observations[2], observations[3], observations[4]};
  spaced[2].quality = 4;
  auto feasible = SelectPlayerFrames(spaced_settings, spaced, bindings, context());
  ok &= expect(
      feasible.ok() && feasible->plan && feasible->plan->selected[1].pair.timeline_pts_ns == 2 * kPlayerFrameSecond &&
          feasible->plan->selected[2].pair.timeline_pts_ns == 4 * kPlayerFrameSecond,
      "greedy coverage preserves a feasible count instead of choosing a blocking middle timestamp");

  auto replay = PlayerFrameReplaySelector::Create(plan);
  if (!replay.ok())
    return 1;
  auto actual = pair(0, left, right);
  actual.cameras[0].sequence = 999; // Different graph origin must not alter physical identity.
  auto captured = replay->Observe(actual);
  ok &= expect(captured.ok() && *captured, "diagnostic decoder sequence does not prevent exact anchor replay");
  for (uint64_t second = 1; second <= 4; ++second) {
    auto result = replay->Observe(pair(second, left, right));
    ok &= expect(
        result.ok() && *result == (second != 2), "replay captures exact selected pairs and skips intermediate pairs");
  }
  ok &= expect(replay->Finish().ok(), "complete exact replay succeeds");
  auto wrong_anchor = PlayerFrameReplaySelector::Create(plan);
  ok &= expect(!wrong_anchor->Observe(pair(1, left, right)).ok(), "missed anchor fails immediately");
  auto skipped = PlayerFrameReplaySelector::Create(plan);
  (void)skipped->Observe(pair(0, left, right));
  ok &= expect(
      !skipped->Observe(pair(2, left, right)).ok() && !skipped->Finish().ok(),
      "missed selected frame and early EOS fail");
  auto chapters = plan;
  chapters.selected[2].pair.cameras[0].path = chapter.string();
  chapters.selected[2].pair.cameras[0].source_pts_ns = 100;
  chapters.fingerprint = *PlayerFrameSelectionFingerprint(chapters);
  auto chapter_replay = PlayerFrameReplaySelector::Create(chapters);
  for (const auto& observation : chapters.selected) {
    auto result = chapter_replay->Observe(observation.pair);
    ok &= expect(result.ok() && *result, "physical chapter identity permits source PTS restart");
  }
  ok &= expect(chapter_replay->Finish().ok(), "chapter replay completes");

  cv::Mat parent(5, 7, CV_8UC1, cv::Scalar(255));
  cv::Mat roi = parent(cv::Rect(1, 1, 3, 3));
  auto stride_hash = PlayerFrameMaskFingerprint(roi);
  auto compact_hash = PlayerFrameMaskFingerprint(roi.clone());
  ok &= expect(
      stride_hash.ok() && compact_hash.ok() && *stride_hash == *compact_hash, "mask hash ignores allocator stride");
  roi.at<uint8_t>(1, 1) = 0;
  ok &= expect(*PlayerFrameMaskFingerprint(roi) != *stride_hash, "mask hash binds actual pixel content");
  std::ofstream(left, std::ios::app) << "changed";
  ok &= expect(!ValidatePlayerFrameSources(plan).ok(), "changed source signatures fail replay validation");
  return ok ? 0 : 1;
}
