#include "hstream/src/libs/playtracker_replay/ReplaySession.h"

#include <unistd.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryCsv.h"
#include "yaml-cpp/yaml.h"

namespace {
namespace fs = std::filesystem;
using namespace hm::playtracker_replay;
void check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
const char* kConfig = R"(play-tracker:
  camera-name: GoPro
  no-wide-start: 0
  ignore-largest-bbox: false
  max-speed-ratio-x: 1.0
  max-speed-ratio-y: 1.0
  max-accel-ratio-x: 1.0
  max-accel-ratio-y: 1.0
  follower-box-min-height-ratio: 0.3
  zoom-in-aggressiveness: 25
  live-boxes:
    - name: current_roi
    - name: current_roi_aspect
      dynamic-acceleration-scaling: 0
      sticky-translation: false
      sticky-sizing: false
)";
std::string fixture(const fs::path& directory, bool legacy = false, bool skipped_tick = false) {
  fs::create_directories(directory);
  const hm::BBox arena(40, 0, 3800, 2160);
  auto created = DsPlayTrackerCreateCpuTracker(arena, YAML::Load(kConfig)["play-tracker"]);
  check(created.ok(), created.status().ToString());
  auto tracker = std::move(*created);
  hm::playtracker::PlayTrackerTelemetryCsv exporter;
  const auto status = exporter.Start(
      directory.string(),
      hm::playtracker::TelemetryConfigArtifact{"original.yaml", kConfig},
      hm::playtracker::TelemetryConfigArtifact{"effective.yaml", kConfig});
  check(status.ok(), status.ToString());
  hm::play_tracker::PlayTrackerResults previous_results;
  for (size_t tick = 0; tick <= 300; ++tick) {
    bool checkpoint = tick % 30 == 0;
    if (tick == 100) {
      const std::string tuning_yaml = R"(play-tracker:
  live-boxes: [{name: current_roi}, {name: current_roi_aspect}]
  hstream-runtime-tuning:
    max-speed-x: 3.0
)";
      const auto tuning = DsPlayTrackerLoadRuntimeTuningContents(tuning_yaml);
      check(tuning.ok(), tuning.status().ToString());
      check(DsPlayTrackerApplyCpuRuntimeTuning(&tracker, *tuning).ok(), "apply recorded tuning");
      check(
          exporter.TryRecordConfigEvent(
              {"runtime-tuning",
               "runtime-tuning-config-file",
               "tuning.yaml",
               "play_tracker_runtime_tuning",
               tuning_yaml}),
          "write event");
      checkpoint = true;
    }
    if (tick == 250) {
      auto next = DsPlayTrackerCreateCpuTracker(arena, YAML::Load(kConfig)["play-tracker"]);
      tracker = std::move(*next);
      check(exporter.TryRecordDiscontinuity({"seek", "seek", "25", {}, {}}), "write seek");
      checkpoint = true;
    }
    hm::playtracker::TelemetrySample sample;
    sample.source_frame = tick;
    sample.source_id = 0;
    sample.width = 3840;
    sample.height = 2160;
    sample.pts_ns = 2000000000ULL + tick * 100000000ULL;
    sample.seek_epoch = tick >= 250 ? 1 : 0;
    std::vector<size_t> ids;
    std::vector<hm::BBox> boxes;
    if (tick >= 3 && tick != 81 && tick != 82) {
      for (size_t player = 0; player < 12; ++player) {
        const float x = 1750 + 900 * std::sin(tick * 0.02) + (int(player % 4) - 2) * 130;
        const float y = 750 + (player / 4) * 140;
        ids.push_back(player + 1);
        boxes.emplace_back(x, y, x + 44, y + 95);
        sample.tracks.push_back({player + 1, x, y, 44, 95, 1, 0});
      }
    }
    const auto captured = DsPlayTrackerCaptureReplayInput(tracker, arena, ids, boxes, checkpoint);
    if (!(skipped_tick && tick == 85))
      previous_results = DsPlayTrackerStepCpu(&tracker, ids, boxes);
    for (const auto& box : previous_results.tracking_boxes)
      sample.policy_boxes.push_back({box.left, box.top, box.width(), box.height()});
    if (!legacy) {
      hm::playtracker::TelemetryReplaySample replay;
      replay.arena = {arena.left, arena.top, arena.right, arena.bottom};
      replay.stepped = captured.stepped && !(skipped_tick && tick == 85);
      replay.has_received_tracks = captured.has_received_tracks;
      replay.reset_epoch = tick >= 250 ? 1 : 0;
      replay.checkpoint = captured.checkpoint;
      replay.base_checkpoint = captured.base_checkpoint;
      for (size_t i = 0; i < ids.size(); ++i)
        replay.tracks.push_back({ids[i], {boxes[i].left, boxes[i].top, boxes[i].right, boxes[i].bottom}});
      sample.replay = std::move(replay);
    }
    check(exporter.TryEnqueue(std::move(sample)), "enqueue fixture sample");
  }
  const std::string path = exporter.output_manifest();
  exporter.MarkRunOutcome(hm::playtracker::TelemetryRunOutcome::kEndOfStream);
  exporter.Stop();
  if (legacy) {
    auto manifest = YAML::LoadFile(path);
    manifest["sidecars"].remove("replay");
    std::ofstream(path) << manifest;
  }
  return path;
}
bool same_frames(const std::vector<Frame>& a, const std::vector<Frame>& b, float tolerance = 0) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].sample_id != b[i].sample_id || a[i].pts_ns != b[i].pts_ns)
      return false;
    for (const auto& pair : {std::make_pair(a[i].fast, b[i].fast), std::make_pair(a[i].follower, b[i].follower)}) {
      if (pair.first.has_value() != pair.second.has_value())
        return false;
      if (pair.first &&
          (std::abs(pair.first->left - pair.second->left) > tolerance ||
           std::abs(pair.first->top - pair.second->top) > tolerance ||
           std::abs(pair.first->right - pair.second->right) > tolerance ||
           std::abs(pair.first->bottom - pair.second->bottom) > tolerance))
        return false;
    }
  }
  return true;
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--make-fixture") {
      std::cout << fixture(argv[2]) << '\n';
      return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "--recording") {
      PrepareOptions options;
      options.manifest_path = argv[2];
      if (argc > 3)
        options.start_seconds = std::stod(argv[3]);
      if (argc > 4)
        options.duration_seconds = std::stod(argv[4]);
      if (argc > 8)
        options.legacy_arena = hm::BBox(std::stof(argv[5]), std::stof(argv[6]), std::stof(argv[7]), std::stof(argv[8]));
      const auto session = ReplaySession::Prepare(options);
      check(session.ok(), session.status().ToString());
      std::cout << (*session)->provenance() << "; " << (*session)->original().size() << " samples\n";
      return 0;
    }
    char tmp[] = "/tmp/hstream-replay-test-XXXXXX";
    check(mkdtemp(tmp), "temporary directory");
    const fs::path directory(tmp);
    PrepareOptions options;
    options.manifest_path = fixture(directory / "exact");
    options.start_seconds = 5;
    options.duration_seconds = 15;
    auto session = ReplaySession::Prepare(options);
    check(session.ok(), session.status().ToString());
    check((*session)->original().size() == 150, "exclusive out sample boundary");
    check(
        (*session)->original().front().sample_id == 51 && (*session)->original().back().sample_id == 201,
        "sample gap is preserved rather than treated as a media frame");
    DsPlayTrackerRuntimeTuning unchanged;
    auto baseline = (*session)->RunTrial("baseline", unchanged);
    check(baseline.ok(), baseline.status().ToString());
    check(same_frames(baseline->frames, (*session)->baseline()), "unmodified trial reproduces baseline");
    DsPlayTrackerRuntimeTuning changed;
    changed.max_speed_x = 1;
    changed.max_accel_x = 0.25;
    changed.zoom_in_aggressiveness = 90;
    auto candidate = (*session)->RunTrial("candidate", changed);
    check(candidate.ok(), candidate.status().ToString());
    check(!same_frames(candidate->frames, baseline->frames), "camera parameter changes affect trajectory");
    auto repeated = (*session)->RunTrial("repeat", changed);
    check(repeated.ok() && same_frames(repeated->frames, candidate->frames), "repeated restore has no trial leakage");
    auto again = (*session)->RunTrial("baseline again", unchanged);
    check(again.ok() && same_frames(again->frames, baseline->frames), "candidate does not mutate saved start");
    check(
        !same_frames((*session)->original(), (*session)->baseline(), 0.02),
        "later recorded tuning does not overwrite fork configuration");
    std::atomic<bool> cancelled{true};
    check(absl::IsCancelled((*session)->RunTrial("cancelled", changed, &cancelled).status()), "trial cancellation");
    check(absl::IsCancelled(ReplaySession::Prepare(options, &cancelled).status()), "preparation cancellation");
    changed.max_speed_x = std::numeric_limits<float>::quiet_NaN();
    check(!(*session)->RunTrial("invalid", changed).ok(), "nonfinite tuning rejected transactionally");
    options.start_seconds = 24;
    options.duration_seconds = 2;
    check(!ReplaySession::Prepare(options).ok(), "reject reset-crossing range");
    options.start_seconds = 29;
    check(!ReplaySession::Prepare(options).ok(), "reject unavailable out point");
    options.start_seconds = 0;
    options.duration_seconds = 1;
    auto empty_start = ReplaySession::Prepare(options);
    check(empty_start.ok() && !(*empty_start)->original().front().follower, "initial empty frames stay empty");
    options.manifest_path = fixture(directory / "legacy", true);
    options.start_seconds = 5;
    options.duration_seconds = 15;
    check(!ReplaySession::Prepare(options).ok(), "legacy missing arena rejected");
    options.legacy_arena = hm::BBox(40, 0, 3800, 2160);
    auto legacy = ReplaySession::Prepare(options);
    check(legacy.ok(), legacy.status().ToString());
    check(
        same_frames((*legacy)->baseline(), (*session)->baseline(), 0.01),
        "legacy reconstruction agrees with exact checkpoint replay");
    options.legacy_arena = hm::BBox(500, 0, 3000, 2160);
    check(!ReplaySession::Prepare(options).ok(), "wrong arena fails measured parity");
    PrepareOptions cadence_options;
    cadence_options.manifest_path = fixture(directory / "cadence", false, true);
    cadence_options.start_seconds = 8;
    cadence_options.duration_seconds = 1;
    auto cadence = ReplaySession::Prepare(cadence_options);
    check(cadence.ok(), cadence.status().ToString());
    check(
        same_frames((*cadence)->original(), (*cadence)->baseline(), 0.02), "skipped policy tick holds previous camera");
    const std::string good_manifest = YAML::Dump(YAML::LoadFile(cadence_options.manifest_path));
    auto bad_manifest = YAML::Load(good_manifest);
    bad_manifest["completed"] = false;
    std::ofstream(cadence_options.manifest_path) << bad_manifest;
    check(!ReplaySession::Prepare(cadence_options).ok(), "incomplete recording rejected");
    bad_manifest = YAML::Load(good_manifest);
    bad_manifest["sidecars"]["replay"] = "../outside.jsonl";
    std::ofstream(cadence_options.manifest_path) << bad_manifest;
    check(!ReplaySession::Prepare(cadence_options).ok(), "artifact cannot escape recording directory");
    std::ofstream(cadence_options.manifest_path) << good_manifest;
    const auto replay_path = directory / "cadence" / "hstream_replay.jsonl";
    std::ifstream replay_input(replay_path);
    std::string replay_contents((std::istreambuf_iterator<char>(replay_input)), {});
    const auto field = replay_contents.find("\"sample_id\":1");
    check(field != std::string::npos, "find sidecar sample identity");
    replay_contents.replace(field, std::string("\"sample_id\":1").size(), "\"sample_id\":0");
    std::ofstream(replay_path) << replay_contents;
    check(!ReplaySession::Prepare(cadence_options).ok(), "sidecar/frame-index identity mismatch rejected");
    std::ofstream(directory / "panorama.mp4") << "test media identity";
    MediaBinding binding{(directory / "panorama.mp4").string(), 2000000000, 0, 3840, 2160};
    check(
        !(*session)->SaveTrial((*session)->manifest_path(), *candidate, binding).ok(),
        "cannot overwrite original telemetry manifest");
    check(!(*session)->SaveTrial(binding.path, *candidate, binding).ok(), "cannot overwrite bound source media");
    const auto saved = (*session)->SaveTrial((directory / "trial.yaml").string(), *candidate, binding);
    check(saved.ok(), saved.ToString());
    auto descriptor = YAML::LoadFile((directory / "trial.yaml").string());
    check(
        descriptor["frames"].size() == 150 &&
            descriptor["media"]["telemetry_origin_pts_ns"].as<uint64_t>() == 2000000000,
        "saved trial includes exact input range and manual media mapping");
    fs::remove_all(directory);
    std::cout << "ReplaySessionTest passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
