#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

namespace fs = std::filesystem;
using namespace hm::stitching;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_file(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  output.close();
  if (!output)
    throw std::runtime_error("Cannot write fixture " + path.string());
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read fixture " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

StitchingExperimentStore open_store(const fs::path& game) {
  fs::create_directories(game);
  auto store = OpenStitchingExperimentStore(game);
  if (!store.ok())
    throw std::runtime_error(store.status().ToString());
  return *store;
}

StoredStitchingExperiment make_record(
    const StitchingExperimentStore& store,
    const std::string& session,
    int sequence,
    int count = 2,
    const std::string& reference = "00:00:10") {
  StoredStitchingExperiment record;
  record.workspace.root = store.directory / "sessions" / session;
  record.workspace.game_id = "candidate-" + std::to_string(sequence);
  record.workspace.game_directory = record.workspace.root / record.workspace.game_id;
  record.workspace.invalidation_id = session + "-owner-" + std::to_string(sequence);
  record.workspace.settings = {
      30 + sequence, count, reference, std::array<double, 3>{0, 1.23456789123, -2.34567891234}};
  record.state = "queued";
  record.sequence = sequence;
  fs::create_directories(record.workspace.game_directory);
  YAML::Node config;
  config["stitching"]["calibration_frame_count"] = count;
  config["stitching"]["stitch_frame_time"] = reference;
  auto calibration = config["hstream_ui"]["stitching_calibration"];
  calibration["invalidation_id"] = record.workspace.invalidation_id;
  calibration["control_points"] = record.workspace.settings.control_points;
  calibration["frame_count"] = count;
  calibration["status"] = "pending";
  write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
  return record;
}

PlayerFrameSelectionPlan make_plan(const fs::path& root, int count, uint64_t anchor_ns = 10 * kPlayerFrameSecond) {
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = count;
  plan.context = {
      {"source_context", "frozen-camera-chapters-offsets"},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask-sha"},
      {"rink_mask_revision", "output:authority"},
      {"fieldmask_settings", "existing-program-mask-settings"},
      {"output_rotation_degrees", "0"},
      {"decode_anchor_ns", std::to_string(anchor_ns)}};
  for (const char* role : {"left", "right"}) {
    const fs::path path = root / (std::string(role) + ".mp4");
    if (!fs::exists(path))
      write_file(path, "camera-source-fixture");
    auto bound = BindPlayerFrameSource(path);
    if (!bound.ok())
      throw std::runtime_error(bound.status().ToString());
    plan.sources.push_back(*bound);
  }
  for (int index = 0; index < count; ++index) {
    PlayerFrameObservation observation;
    observation.pair.timeline_pts_ns = anchor_ns + index * 2 * kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera)
      observation.pair.cameras[camera] = {
          plan.sources[camera].path,
          observation.pair.timeline_pts_ns + camera * 100,
          static_cast<uint32_t>(camera),
          static_cast<uint64_t>(300 + index * 60)};
    if (index > 0) {
      observation.eligible_people = 1;
      observation.size_band_counts[0] = 1;
      observation.coverage = {static_cast<uint16_t>(index)};
      observation.quality = 0.9;
    }
    plan.selected.push_back(observation);
  }
  auto hash = PlayerFrameSelectionFingerprint(plan);
  if (!hash.ok())
    throw std::runtime_error(hash.status().ToString());
  plan.fingerprint = *hash;
  return plan;
}

void install_plan(StoredStitchingExperiment& record, const PlayerFrameSelectionPlan& plan) {
  const auto path = record.workspace.game_directory / "config.yaml";
  auto config = YAML::Load(read_file(path));
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  write_file(path, YAML::Dump(config));
  record.selection_fingerprint = plan.fingerprint;
}

struct InterruptCleanup {
  std::string previous;
  bool was_set{false};
  InterruptCleanup() {
    if (const char* value = std::getenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY")) {
      previous = value;
      was_set = true;
    }
    if (::setenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY", "1", 1) != 0)
      throw std::runtime_error("Cannot enable cleanup interruption fixture");
  }
  ~InterruptCleanup() {
    if (was_set)
      ::setenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY", previous.c_str(), 1);
    else
      ::unsetenv("HM_TEST_TRANSACTION_CLEANUP_INTERRUPT_AFTER_ENTRY");
  }
};

bool failed_attempt_removal(const fs::path& root) {
  bool ok = true;
  for (bool frozen : {false, true}) {
    auto store = open_store(root / (frozen ? "failed-frozen-removal" : "failed-reserved-removal"));
    auto owner = make_record(store, "session", 1);
    owner.reservation_token = "failed-owner";
    const auto reserved = ReserveStitchingExperimentFrameCount(
        store, 2, 10 * kPlayerFrameSecond, owner.workspace, owner.reservation_token);
    ok &= expect(reserved.ok() && !reserved->has_value(), "reserve failed-removal fixture");
    if (frozen)
      install_plan(owner, make_plan(store.game_directory, 2));
    owner.state = frozen ? "frozen" : "failed";
    ok &= expect(SaveStitchingExperiment(store, owner, frozen).ok(), "save stopped failed attempt");
    const auto key = owner.workspace.game_directory.lexically_relative(store.directory).generic_string();
    write_file(store.game_directory / "config.yaml", "main sentinel");
    fs::create_directory_symlink(store.game_directory, owner.workspace.game_directory / "linked-main");
    write_file(owner.workspace.game_directory / "runner.log", "failed runner");
    ok &= expect(
        RemoveStitchingExperiments(store, {key}).ok() && !fs::exists(owner.workspace.game_directory) &&
            read_file(store.game_directory / "config.yaml") == "main sentinel",
        "failed removal deletes private files without following media links into main");
    const auto loaded = LoadStitchingExperimentStore(store);
    ok &= expect(
        loaded.ok() && loaded->experiments.empty() && loaded->selected_by_count.empty() &&
            loaded->retained_by_count.empty(),
        "failed removal forgets frozen selections as well as history");
    auto fresh = make_record(store, "fresh", 1);
    const auto retry =
        ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, fresh.workspace, "new-attempt");
    ok &= expect(retry.ok() && !retry->has_value(), "removed attempts cannot block a new selection of that count");
    fresh.state = "quarantined";
    fresh.reservation_token = "new-attempt";
    ok &= expect(SaveStitchingExperiment(store, fresh).ok(), "save unconfirmed process ownership");
    ok &= expect(
        !RemoveStitchingExperiments(
             store, {fresh.workspace.game_directory.lexically_relative(store.directory).generic_string()})
                .ok() &&
            fs::exists(fresh.workspace.game_directory),
        "removal must reject quarantined attempts even without a recorded PID");
  }
  return ok;
}

bool completed_result_removal(const fs::path& root) {
  bool ok = true;
  auto store = open_store(root / "completed-removal");
  auto owner = make_record(store, "owner-session", 1);
  const auto key = [&](const StoredStitchingExperiment& record) {
    return record.workspace.game_directory.lexically_relative(store.directory).generic_string();
  };
  owner.reservation_token = "complete-owner";
  const auto reserved =
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, owner.workspace, owner.reservation_token);
  ok &= expect(reserved.ok() && !reserved->has_value(), "reserve completed selection owner");
  install_plan(owner, make_plan(store.game_directory, 2));
  owner.state = "complete";
  owner.artifact_generation_id = "completed-generation";
  ok &= expect(SaveStitchingExperiment(store, owner, true).ok(), "publish completed selection owner");
  auto child = make_record(store, "child-session", 1);
  child.state = "complete";
  child.artifact_generation_id = "child-generation";
  child.selection_owner_sequence = owner.sequence;
  child.selection_owner_workspace_key = key(owner);
  auto unrelated = make_record(store, "unrelated-session", 1, 3);
  unrelated.state = "complete";
  unrelated.artifact_generation_id = "unrelated-generation";
  ok &= expect(
      SaveStitchingExperiment(store, child).ok() && SaveStitchingExperiment(store, unrelated).ok(),
      "save completed dependent and unrelated result");
  const auto original = read_file(store.directory / "index.yaml");
  ok &= expect(
      !RemoveStitchingExperiments(store, {key(owner)}).ok() && read_file(store.directory / "index.yaml") == original &&
          fs::exists(owner.workspace.game_directory),
      "completed removal must preserve a surviving dependent's inputs");
  for (int64_t session_id : {0, 12345}) {
    child.process_session_id = session_id;
    child.process_token = "preview-intent";
    ok &= expect(SaveStitchingExperiment(store, child).ok(), "save completed preview ownership");
    const auto active = read_file(store.directory / "index.yaml");
    ok &= expect(
        !RemoveStitchingExperiments(store, {key(owner), key(child)}).ok() &&
            read_file(store.directory / "index.yaml") == active && fs::exists(child.workspace.game_directory),
        "preview intent or an active process prevents completed deletion");
  }
  child.process_session_id = 0;
  child.process_token.clear();
  ok &= expect(SaveStitchingExperiment(store, child).ok(), "release preview ownership");
  write_file(store.game_directory / "config.yaml", "main calibration sentinel");
  fs::create_directories(store.game_directory / "player-frame-inputs");
  write_file(store.game_directory / "player-frame-inputs" / "promoted.png", "promoted input sentinel");
  fs::create_directory_symlink(store.game_directory, owner.workspace.game_directory / "linked-main");
  write_file(owner.workspace.game_directory / "runner.log", "completed runner");
  ok &= expect(
      RemoveStitchingExperiments(store, {key(owner), key(child)}).ok() && !fs::exists(owner.workspace.game_directory) &&
          !fs::exists(child.workspace.game_directory) && fs::exists(unrelated.workspace.game_directory) &&
          read_file(store.game_directory / "config.yaml") == "main calibration sentinel" &&
          read_file(store.game_directory / "player-frame-inputs" / "promoted.png") == "promoted input sentinel",
      "completed cascade removes only owned results and preserves main's promoted inputs");
  const auto loaded = LoadStitchingExperimentStore(store);
  ok &= expect(
      loaded.ok() && loaded->experiments.size() == 1 && loaded->selected_by_count.empty() &&
          !loaded->retained_by_count.count(2),
      "completed owner removal forgets its frame-count selection");
  auto fresh = make_record(store, "fresh-session", 1);
  const auto retry =
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, fresh.workspace, "new-selection");
  ok &= expect(retry.ok() && !retry->has_value(), "removed completed count can choose new frames");
  return ok;
}

bool deletion_recovery(const fs::path& root) {
  bool ok = true;
  auto queued_store = open_store(root / "queued-deletion-recovery");
  auto removed = make_record(queued_store, "session", 1);
  auto retained = make_record(queued_store, "session", 2);
  ok &= expect(
      SaveStitchingExperiment(queued_store, removed).ok() && SaveStitchingExperiment(queued_store, retained).ok(),
      "prepare independent queued-removal recovery rows");
  write_file(removed.workspace.game_directory / "first.png", "first private captured input");
  write_file(removed.workspace.game_directory / "second.png", "second private captured input");
  absl::Status removed_status;
  {
    InterruptCleanup interrupt;
    removed_status = RemoveStitchingExperiments(
        queued_store, {removed.workspace.game_directory.lexically_relative(queued_store.directory).generic_string()});
  }
  ok &= expect(
      !removed_status.ok() && fs::exists(removed.workspace.game_directory / "config.yaml"),
      "failed queued cleanup retains ownership of the remaining private files");
  auto remaining = LoadStitchingExperimentStore(queued_store);
  ok &= expect(
      remaining.ok() && remaining->experiments.size() == 1 &&
          remaining->experiments.front().workspace.game_directory == retained.workspace.game_directory,
      "queued removal commits a valid surviving catalog before deleting files");
  ok &= expect(
      !SaveStitchingExperiment(queued_store, removed).ok(),
      "an old queued-row handle cannot resurrect a partially removed row");
  ok &= expect(
      DiscardStitchingExperimentStore(queued_store).ok() && !fs::exists(queued_store.directory),
      "explicit whole-cache discard cleans leftover files from a failed queued removal");

  auto full_store = open_store(root / "full-deletion-recovery");
  auto candidate = make_record(full_store, "session", 1);
  ok &= expect(SaveStitchingExperiment(full_store, candidate).ok(), "prepare interrupted whole-cache discard");
  const fs::path main_input = full_store.game_directory / "left.png";
  write_file(main_input, "main calibration must survive discard");
  absl::Status discard_status;
  {
    InterruptCleanup interrupt;
    discard_status = DiscardStitchingExperimentStore(full_store);
  }
  ok &= expect(!discard_status.ok(), "injected whole-cache deletion failure is reported");
  const auto pending_marker = read_file(full_store.directory / "owner.yaml");
  ok &= expect(
      YAML::Load(pending_marker)["discard_pending"].as<bool>(false),
      "discard intent is durably retained before its first deletion");
  auto pending = OpenStitchingExperimentStore(full_store.game_directory);
  if (!expect(pending.ok() && pending->discard_pending, "reopening exposes interrupted discard for explicit retry"))
    return false;
  ok &= expect(
      read_file(full_store.directory / "owner.yaml") == pending_marker && fs::exists(full_store.directory),
      "ordinary reopen never resumes deletion automatically");
  ok &= expect(
      !LoadStitchingExperimentStore(*pending).ok() && !SaveStitchingExperiment(*pending, candidate).ok(),
      "pending discard blocks ordinary catalog operations");
  ok &= expect(
      DiscardStitchingExperimentStore(*pending).ok() && !fs::exists(full_store.directory),
      "explicit retry finishes discard without relying on the deleted index or workspace");
  ok &= expect(
      read_file(main_input) == "main calibration must survive discard", "discard recovery preserves main artifacts");
  auto fresh = OpenStitchingExperimentStore(full_store.game_directory);
  ok &= expect(fresh.ok() && !fresh->discard_pending, "a new store is available after explicit cleanup completes");
  return ok;
}

bool stopped_process_with_corrupt_config(const fs::path& root) {
  auto store = open_store(root);
  auto owner = make_record(store, "interrupted", 1);
  owner.state = "scan";
  owner.process_session_id = 2147483647;
  owner.process_token = "stopped-process";
  owner.reservation_token = "retain-corrupt-selection";
  bool ok = expect(SaveStitchingExperiment(store, owner).ok(), "persist interrupted process ownership");
  const auto reservation =
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, owner.workspace, owner.reservation_token);
  ok &= expect(reservation.ok() && !reservation->has_value(), "retain the interrupted selection reservation");
  write_file(owner.workspace.game_directory / "config.yaml", "stitching: [invalid yaml");
  ok &= expect(!DiscardStitchingExperimentStore(store).ok(), "unreconciled process intent prevents discard");
  auto wrong_process = owner;
  wrong_process.process_token = "different-process";
  ok &= expect(
      absl::IsAborted(MarkStitchingExperimentProcessStopped(store, wrong_process, "stopped")),
      "shutdown proof must match the exact persisted process identity");
  auto stale = owner;
  ok &= expect(
      MarkStitchingExperimentProcessStopped(store, owner, "Selection metadata is corrupt").ok(),
      "confirmed shutdown can be recorded despite an unreadable config");
  const auto stopped_index = read_file(store.directory / "index.yaml");
  ok &= expect(
      absl::IsAborted(MarkStitchingExperimentProcessStopped(store, stale, "stale shutdown")) &&
          read_file(store.directory / "index.yaml") == stopped_index,
      "stale shutdown reconciliation cannot overwrite newer row state");
  const auto restored = LoadStitchingExperimentStore(store);
  ok &= expect(
      restored.ok() && restored->experiments.size() == 1 && restored->experiments[0].state == "failed" &&
          restored->experiments[0].process_session_id == 0 && restored->experiments[0].process_token.empty() &&
          restored->experiments[0].reservation_token == "retain-corrupt-selection" &&
          YAML::Load(stopped_index)["reservations"].size() == 1,
      "clearing process intent preserves the blocked count's reservation");
  write_file(store.game_directory / "main-artifact", "main survives explicit discard");
  ok &= expect(
      DiscardStitchingExperimentStore(store).ok() &&
          read_file(store.game_directory / "main-artifact") == "main survives explicit discard",
      "explicit discard removes corrupt stopped history while preserving main data");
  return ok;
}

bool retained_main_counts(const fs::path& root) {
  bool ok = true;
  for (const std::string scenario : {"single", "newer", "reserved"}) {
    auto store = open_store(root / scenario);
    auto plan = make_plan(store.game_directory, 2);
    auto first = make_record(store, "session-a", 1);
    first.saved_selection_fingerprint = plan.fingerprint;
    install_plan(first, plan);
    auto contender = make_record(store, "reservation", 1);
    if (scenario == "reserved") {
      const auto reservation = ReserveStitchingExperimentFrameCount(
          store, 2, 10 * kPlayerFrameSecond, contender.workspace, "existing-selection");
      ok &= expect(
          reservation.ok() && !reservation->has_value(), "reserve a fresh count before another snapshot is saved");
    }
    ok &= expect(SaveStitchingExperiment(store, first).ok(), "queue a frozen main-derived row before any solve");
    YAML::Node main;
    main["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(make_plan(store.game_directory, 3));
    write_file(store.game_directory / "config.yaml", YAML::Dump(main));
    const auto first_key = first.workspace.game_directory.lexically_relative(store.directory).generic_string();
    auto restored = LoadStitchingExperimentStore(store);
    ok &= expect(
        restored.ok() && restored->selected_by_count.empty() &&
            (scenario == "reserved" ? restored->retained_by_count.empty()
                                    : restored->retained_by_count.at(2) == first_key),
        "unstarted frozen frames remain discoverable unless a reservation already exists");
    if (scenario != "reserved") {
      const auto before_start = read_file(store.directory / "index.yaml");
      const auto stale_start =
          ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, contender.workspace, "stale-search");
      ok &= expect(
          stale_start.ok() && stale_start->has_value() && (**stale_start).selection_fingerprint == plan.fingerprint &&
              read_file(store.directory / "index.yaml") == before_start,
          "starting an older queued search must return retained frames without creating a reservation");
    }
    if (scenario == "newer") {
      auto replacement = plan;
      replacement.selected[1].pair.cameras[0].source_pts_ns += 123;
      replacement.fingerprint = *PlayerFrameSelectionFingerprint(replacement);
      auto second = make_record(store, "session-b", 1);
      second.saved_selection_fingerprint = replacement.fingerprint;
      install_plan(second, replacement);
      ok &=
          expect(SaveStitchingExperiment(store, second).ok(), "queue a later main-derived snapshot of the same count");
      const auto second_key = second.workspace.game_directory.lexically_relative(store.directory).generic_string();
      first.state = "failed";
      ok &= expect(
          SaveStitchingExperiment(store, first, true, plan.fingerprint).ok(),
          "the older snapshot can finish after a newer snapshot was queued");
      restored = LoadStitchingExperimentStore(store);
      ok &= expect(
          restored.ok() && restored->selected_by_count.empty() && restored->retained_by_count.at(2) == second_key,
          "late revisions cannot revive the older snapshot or replace the newest queued fallback");
      second.state = "failed";
      ok &= expect(
          SaveStitchingExperiment(store, second, true, replacement.fingerprint).ok(),
          "the latest retained snapshot fills its count's otherwise empty default");
      ok &=
          expect(SaveStitchingExperiment(store, first, true, plan.fingerprint).ok(), "older history remains writable");
      restored = LoadStitchingExperimentStore(store);
      ok &= expect(
          restored.ok() && restored->selected_by_count.at(2) == second_key && restored->retained_by_count.empty(),
          "an existing newer default cannot be overwritten by stale-main completion");
    } else if (scenario == "reserved") {
      first.state = "failed";
      ok &= expect(
          SaveStitchingExperiment(store, first, true, plan.fingerprint).ok(),
          "stale completion remains durable beside an existing count reservation");
      restored = LoadStitchingExperimentStore(store);
      ok &= expect(
          restored.ok() && restored->selected_by_count.empty() && restored->retained_by_count.empty(),
          "reserved counts cannot lend stale fallback frames or receive an old default");
      ok &= expect(
          !ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, first.workspace, "old-selection")
               .ok(),
          "stale completion preserves the existing reservation");
    } else {
      first.state = "failed";
      ok &= expect(
          SaveStitchingExperiment(store, first, true, plan.fingerprint).ok(),
          "completion preserves a retained count when main now selects a different count");
      restored = LoadStitchingExperimentStore(store);
      ok &= expect(
          restored.ok() && restored->selected_by_count.at(2) == first_key,
          "completion publishes the retained count without requiring it to be current main");
    }
  }
  return ok;
}

bool resolution_settings(const fs::path& root) {
  auto store = open_store(root);
  auto legacy = make_record(store, "resolution", 1);
  bool ok = expect(SaveStitchingExperiment(store, legacy).ok(), "legacy rows without image size remain saveable");
  int sequence = 1;
  for (const std::string size : {"native", "1k", "2k"}) {
    auto record = make_record(store, "resolution", ++sequence);
    record.workspace.settings.control_point_resolution = size;
    YAML::Node config = YAML::LoadFile((record.workspace.game_directory / "config.yaml").string());
    config["stitching"]["control_point_resolution"] = size;
    write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
    ok &= expect(SaveStitchingExperiment(store, record).ok(), "each explicit image size persists independently");
    auto changed = record;
    changed.workspace.settings.control_point_resolution = size == "native" ? "1k" : "native";
    config["stitching"]["control_point_resolution"] = *changed.workspace.settings.control_point_resolution;
    write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
    ok &= expect(
        !SaveStitchingExperiment(store, changed).ok(),
        "updating an existing candidate cannot change its frozen image size even when config agrees");
    ok &= expect(
        !SaveStitchingExperiment(store, record).ok(), "a changed workspace size cannot retain the old catalog setting");
    config["stitching"]["control_point_resolution"] = size;
    write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
  }
  const auto restored = LoadStitchingExperimentStore(store);
  if (!expect(restored.ok() && restored->experiments.size() == 4, "image size variants survive history reload"))
    return false;
  ok &= expect(
      !restored->experiments[0].workspace.settings.control_point_resolution &&
          restored->experiments[1].workspace.settings.control_point_resolution == "native" &&
          restored->experiments[2].workspace.settings.control_point_resolution == "1k" &&
          restored->experiments[3].workspace.settings.control_point_resolution == "2k",
      "history distinguishes absent legacy image size from every explicit size");
  auto invalid = make_record(store, "resolution", ++sequence);
  invalid.workspace.settings.control_point_resolution = "bad-size";
  ok &= expect(!SaveStitchingExperiment(store, invalid).ok(), "catalog rejects invalid image-size settings");
  return ok;
}

bool manual_match_settings(const fs::path& root) {
  auto store = open_store(root);
  auto record = make_record(store, "manual", 1);
  record.workspace.settings.manual_control_points = std::string(64, 'a');
  YAML::Node config = YAML::LoadFile((record.workspace.game_directory / "config.yaml").string());
  config["stitching"]["manual_control_points"] = *record.workspace.settings.manual_control_points;
  write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
  bool ok = expect(SaveStitchingExperiment(store, record).ok(), "manual match identity must persist");
  const auto restored = LoadStitchingExperimentStore(store);
  ok &= expect(
      restored.ok() && restored->experiments.size() == 1 &&
          restored->experiments[0].workspace.settings.manual_control_points ==
              record.workspace.settings.manual_control_points,
      "manual match identity must survive catalog reload");
  auto changed = record;
  changed.workspace.settings.manual_control_points = std::string(64, 'b');
  config["stitching"]["manual_control_points"] = *changed.workspace.settings.manual_control_points;
  write_file(record.workspace.game_directory / "config.yaml", YAML::Dump(config));
  ok &= expect(
      !SaveStitchingExperiment(store, changed).ok(), "an existing candidate cannot replace its manual match identity");
  ok &= expect(!SaveStitchingExperiment(store, record).ok(), "catalog rejects a mismatched workspace manual identity");
  return ok;
}

bool run(const fs::path& root) {
  bool ok = true;
  ok &= manual_match_settings(root / "manual-settings");
  ok &= resolution_settings(root / "resolution-settings");
  ok &= stopped_process_with_corrupt_config(root / "stopped-corrupt-config");
  ok &= retained_main_counts(root / "retained-main-counts");
  {
    auto interrupted = open_store(root / "partial-preparation");
    auto baseline = make_record(interrupted, "session-a", 1);
    auto players = make_record(interrupted, "session-a", 2);
    players.baseline_sequence = baseline.sequence;
    players.selection_owner_sequence = players.sequence;
    players.selection_owner_workspace_key =
        players.workspace.game_directory.lexically_relative(interrupted.directory).generic_string();
    const auto baseline_config = read_file(baseline.workspace.game_directory / "config.yaml");
    write_file(baseline.workspace.game_directory / "config.yaml", "{}\n");
    ok &= expect(!SaveStitchingExperiment(interrupted, baseline).ok(), "inject a baseline preparation save failure");
    ok &= expect(
        !SaveStitchingExperiment(interrupted, players).ok(),
        "a dependent sequence without its workspace key cannot be saved");
    players.baseline_workspace_key =
        baseline.workspace.game_directory.lexically_relative(interrupted.directory).generic_string();
    ok &= expect(
        !SaveStitchingExperiment(interrupted, players).ok(),
        "an existing workspace from a failed baseline save cannot satisfy a durable dependency");
    auto reopened = LoadStitchingExperimentStore(interrupted);
    ok &= expect(
        reopened.ok() && reopened->experiments.empty(),
        "reopening partial preparation cannot restore an orphan Players row");
    write_file(baseline.workspace.game_directory / "config.yaml", baseline_config);
    ok &= expect(
        SaveStitchingExperiment(interrupted, baseline).ok() && SaveStitchingExperiment(interrupted, players).ok(),
        "durably saved baseline and self-owned Players dependencies remain supported");
    auto partial_catalog = YAML::Load(read_file(interrupted.directory / "index.yaml"));
    YAML::Node orphan(YAML::NodeType::Sequence);
    orphan.push_back(YAML::Clone(partial_catalog["experiments"][1]));
    partial_catalog["experiments"] = orphan;
    write_file(interrupted.directory / "index.yaml", YAML::Dump(partial_catalog));
    ok &= expect(
        !LoadStitchingExperimentStore(interrupted).ok(), "reopening rejects a catalog whose baseline row disappeared");
  }
  auto store = open_store(root / "games-a" / "same-name");
  auto other_game = open_store(root / "games-b" / "same-name");
  ok &= expect(
      store.directory == fs::canonical(store.game_directory) / "stitching-experiments",
      "all experiment data lives inside its canonical game");
  ok &= expect(store.directory != other_game.directory, "same-named games never share history");
  fs::create_directory_symlink(store.game_directory, root / "game-alias");
  auto alias = OpenStitchingExperimentStore(root / "game-alias");
  ok &= expect(alias.ok() && alias->directory == store.directory, "a game alias resolves to the same owned store");

  auto ordinary = make_record(store, "session-a", 1);
  ordinary.state = "complete";
  ordinary.artifact_generation_id = "ordinary-artifact-generation";
  ok &=
      expect(SaveStitchingExperiment(store, ordinary).ok() && ordinary.revision == 1, "save a completed ordinary row");
  auto stale = ordinary;
  ordinary.state = "failed";
  ordinary.failure = "Retained diagnostic after a later operation";
  ok &=
      expect(SaveStitchingExperiment(store, ordinary).ok() && ordinary.revision == 2, "new row revisions are durable");
  stale.state = "queued";
  ok &= expect(absl::IsAborted(SaveStitchingExperiment(store, stale)), "another dialog cannot overwrite a newer row");
  auto additional = make_record(store, "session-b", 1);
  ok &= expect(
      SaveStitchingExperiment(store, additional).ok(), "a second session merges its new row into current history");
  auto restored = LoadStitchingExperimentStore(store);
  if (!expect(restored.ok() && restored->experiments.size() == 2, "reopen restores all existing rows"))
    return false;
  ok &= expect(
      restored->experiments[0].workspace.settings.rink_rotation_degrees ==
          ordinary.workspace.settings.rink_rotation_degrees,
      "persistent settings preserve calibrated floating-point precision");
  ok &= expect(
      restored->experiments[0].artifact_generation_id == ordinary.artifact_generation_id &&
          restored->experiments[0].failure == ordinary.failure,
      "restored rows retain artifact identity and failure diagnostics");

  auto owner = make_record(store, "session-a", 2);
  owner.baseline_sequence = 1;
  owner.baseline_workspace_key = ordinary.workspace.game_directory.lexically_relative(store.directory).generic_string();
  owner.reservation_token = "count-group-nonce";
  auto contender = make_record(store, "session-b", 2);
  const auto reserve =
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, owner.workspace, owner.reservation_token);
  ok &= expect(reserve.ok() && !reserve->has_value(), "reserve a new count before baseline or scan launch");
  ok &= expect(
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, owner.workspace, owner.reservation_token)
          .ok(),
      "the same count owner can resume its reservation idempotently");
  ok &= expect(
      !ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, contender.workspace, "other-group").ok(),
      "another dialog cannot duplicate an unfinished count's baseline or scan");
  ok &= expect(
      !ReleaseStitchingExperimentFrameCount(store, 2, owner.workspace, "wrong-token").ok(),
      "a count reservation cannot be released without its owner nonce");
  ok &= expect(SaveStitchingExperiment(store, owner).ok(), "queued owner persists its reservation nonce");
  owner.state = "scan";
  owner.process_token = "runner-process-token";
  owner.process_session_id = ::getpid();
  ok &= expect(SaveStitchingExperiment(store, owner).ok(), "persist process intent before launching a scan");
  ok &= expect(!DiscardStitchingExperimentStore(store).ok(), "active process intent blocks discard");
  auto during_scan = LoadStitchingExperimentStore(store);
  ok &= expect(
      during_scan.ok() && during_scan->experiments.back().process_token == owner.process_token &&
          during_scan->experiments.back().reservation_token == owner.reservation_token,
      "runner and count-group ownership survive reopening independently");
  owner.state = "frozen";
  owner.process_token.clear();
  owner.process_session_id = 0;
  const auto plan = make_plan(store.game_directory, 2);
  install_plan(owner, plan);
  auto wrong_nonce = owner;
  wrong_nonce.reservation_token = "wrong-token";
  ok &= expect(!SaveStitchingExperiment(store, wrong_nonce, true).ok(), "freezing cannot steal a count reservation");
  ok &= expect(
      SaveStitchingExperiment(store, owner, true).ok(),
      "publish a stopped frozen count owner and consume its reservation");
  auto reuse =
      ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, contender.workspace, "another-group");
  ok &= expect(
      reuse.ok() && reuse->has_value() && (**reuse).selection_fingerprint == plan.fingerprint,
      "the same count returns the existing selection after reopening");
  auto wrong_reference = make_record(store, "session-c", 1, 2, "00:00:11");
  ok &= expect(
      !ReserveStitchingExperimentFrameCount(
           store, 2, 11 * kPlayerFrameSecond, wrong_reference.workspace, "different-reference")
           .ok(),
      "reference conflicts never create an alternate same-count selection");
  owner.state = "failed";
  owner.failure = "Matcher model failed after inputs were frozen";
  ok &= expect(SaveStitchingExperiment(store, owner).ok(), "a failed solve retains its frozen plan");
  reuse = ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, contender.workspace, "another-group");
  ok &= expect(reuse.ok() && reuse->has_value(), "stopped failed owners remain reusable");
  owner.state = "quarantined";
  ok &= expect(SaveStitchingExperiment(store, owner).ok(), "persist an uncertain process owner honestly");
  ok &= expect(
      !ReserveStitchingExperimentFrameCount(store, 2, 10 * kPlayerFrameSecond, contender.workspace, "another-group")
           .ok(),
      "quarantined owners cannot lend their frame sets");
  ok &= expect(!DiscardStitchingExperimentStore(store).ok(), "quarantined workspaces cannot be discarded");
  owner.state = "failed";
  ok &= expect(SaveStitchingExperiment(store, owner).ok(), "proven shutdown permits a stopped owner state");

  auto count_three = make_record(store, "session-c", 2, 3);
  count_three.state = "frozen";
  install_plan(count_three, make_plan(store.game_directory, 3));
  ok &=
      expect(SaveStitchingExperiment(store, count_three, true).ok(), "a second count keeps its independent selection");
  restored = LoadStitchingExperimentStore(store);
  ok &= expect(restored.ok() && restored->selected_by_count.size() == 2, "reopening retains both count selections");

  auto replacement = make_record(store, "session-d", 1);
  replacement.state = "frozen";
  auto replacement_plan = plan;
  replacement_plan.selected[1].pair.cameras[0].source_pts_ns += 123;
  replacement_plan.fingerprint = *PlayerFrameSelectionFingerprint(replacement_plan);
  install_plan(replacement, replacement_plan);
  const auto before_conflict = read_file(store.directory / "index.yaml");
  ok &= expect(
      !SaveStitchingExperiment(store, replacement, true).ok() && replacement.revision == 0,
      "history cannot silently replace an established count's selection");
  ok &= expect(
      read_file(store.directory / "index.yaml") == before_conflict,
      "failed selection updates preserve exact catalog bytes");
  YAML::Node main;
  main["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(replacement_plan);
  write_file(store.game_directory / "config.yaml", YAML::Dump(main));
  ok &= expect(
      !SaveStitchingExperiment(store, replacement, true, plan.fingerprint).ok(),
      "main authority must match the current main plan");
  ok &= expect(
      SaveStitchingExperiment(store, replacement, true, replacement_plan.fingerprint).ok(),
      "an explicitly changed main plan supersedes the historical count default");
  restored = LoadStitchingExperimentStore(store);
  ok &= expect(
      restored.ok() && restored->experiments.size() == 5 && restored->selected_by_count.size() == 2,
      "changing count authority retains older rows and other counts");
  const auto current_authorities = restored->selected_by_count;
  owner.failure = "Retained solve finished after main selected another frame set";
  ok &= expect(
      SaveStitchingExperiment(store, owner, true, plan.fingerprint).ok(),
      "a superseded main-derived solve still saves its result");
  restored = LoadStitchingExperimentStore(store);
  ok &= expect(
      restored.ok() && restored->experiments.size() == 5 && restored->selected_by_count == current_authorities &&
          std::any_of(
              restored->experiments.begin(),
              restored->experiments.end(),
              [&](const auto& record) {
                return record.workspace.game_directory == owner.workspace.game_directory &&
                    record.failure == owner.failure && record.selection_fingerprint == plan.fingerprint;
              }),
      "late completion preserves both immutable histories and the newer count authority");
  main["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(make_plan(store.game_directory, 3));
  write_file(store.game_directory / "config.yaml", YAML::Dump(main));
  ok &= expect(
      SaveStitchingExperiment(store, owner, true, plan.fingerprint).ok(),
      "a main frame-count change also permits completion of the retained solve");
  restored = LoadStitchingExperimentStore(store);
  ok &= expect(
      restored.ok() && restored->selected_by_count == current_authorities,
      "a late result cannot republish authority after main changes frame count");

  // Competing processes must merge while holding the store lock, not publish
  // independent copies of the catalog read before taking that lock.
  auto merge_a = make_record(store, "concurrent-a", 1, 4);
  auto merge_b = make_record(store, "concurrent-b", 1, 4);
  std::array<pid_t, 2> children;
  for (size_t index = 0; index < children.size(); ++index) {
    children[index] = ::fork();
    if (children[index] == 0) {
      auto record = index == 0 ? merge_a : merge_b;
      ::_exit(SaveStitchingExperiment(store, record).ok() ? 0 : 1);
    }
    if (children[index] < 0)
      throw std::runtime_error("Cannot start concurrent catalog fixture");
  }
  for (pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) < 0)
      throw std::runtime_error("Cannot wait for concurrent catalog fixture");
    ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "concurrent row update succeeds");
  }
  restored = LoadStitchingExperimentStore(store);
  ok &= expect(restored.ok() && restored->experiments.size() == 7, "concurrent writers preserve both new records");
  int successful_reservations = 0;
  for (size_t index = 0; index < children.size(); ++index) {
    children[index] = ::fork();
    if (children[index] == 0) {
      const auto& record = index == 0 ? merge_a : merge_b;
      auto result = ReserveStitchingExperimentFrameCount(
          store, 4, 10 * kPlayerFrameSecond, record.workspace, index == 0 ? "reservation-a" : "reservation-b");
      ::_exit(result.ok() ? 0 : 1);
    }
    if (children[index] < 0)
      throw std::runtime_error("Cannot start competing reservation fixture");
  }
  for (pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) < 0)
      throw std::runtime_error("Cannot wait for competing reservation fixture");
    successful_reservations += WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  ok &= expect(successful_reservations == 1, "only one process reserves a fresh count");

  const auto valid_index = read_file(store.directory / "index.yaml");
  const auto invalid_index = [&](const std::string& contents, const std::string& description) {
    write_file(store.directory / "index.yaml", contents);
    const bool rejected = !LoadStitchingExperimentStore(store).ok();
    const bool preserved = read_file(store.directory / "index.yaml") == contents;
    write_file(store.directory / "index.yaml", valid_index);
    return expect(rejected && preserved, description);
  };
  ok &= invalid_index("[unterminated", "corrupt catalog is reported without resetting history");
  ok &= invalid_index(
      std::string(kMaximumStitchingExperimentCatalogBytes + 1, ' '), "catalog byte bound rejects oversized reads");
  auto invalid = YAML::Load(valid_index);
  invalid["experiments"][0]["key"] = "sessions/../outside";
  ok &= invalid_index(YAML::Dump(invalid), "catalog record paths cannot escape their owned session");
  invalid = YAML::Load(valid_index);
  invalid["selected_by_count"]["2"] = "sessions/missing/candidate";
  ok &= invalid_index(YAML::Dump(invalid), "selected counts cannot silently lose their owner");
  invalid = YAML::Load(valid_index);
  invalid["version"] = 99;
  ok &= invalid_index(YAML::Dump(invalid), "unknown catalog versions fail explicitly");
  invalid = YAML::Load(valid_index);
  while (invalid["experiments"].size() <= kMaximumStoredStitchingExperiments)
    invalid["experiments"].push_back(YAML::Clone(invalid["experiments"][0]));
  ok &= invalid_index(YAML::Dump(invalid), "oversized row history fails without eviction");

  const auto owner_file = store.directory / "owner.yaml";
  const auto valid_owner = read_file(owner_file);
  auto foreign_owner = YAML::Load(valid_owner);
  foreign_owner["game_directory"] = other_game.game_directory.string();
  write_file(owner_file, YAML::Dump(foreign_owner));
  ok &= expect(
      !LoadStitchingExperimentStore(store).ok() && !DiscardStitchingExperimentStore(store).ok(),
      "wrong game ownership blocks loading and destructive discard");
  write_file(owner_file, valid_owner);
  fs::rename(store.directory / "index.yaml", store.directory / "index.saved");
  fs::create_symlink(store.directory / "index.saved", store.directory / "index.yaml");
  ok &= expect(!LoadStitchingExperimentStore(store).ok(), "catalog manifests may not be symlinks");
  fs::remove(store.directory / "index.yaml");
  fs::rename(store.directory / "index.saved", store.directory / "index.yaml");
  fs::rename(store.directory / "sessions", store.directory / "sessions.saved");
  fs::create_directory_symlink(store.directory / "sessions.saved", store.directory / "sessions");
  ok &= expect(!LoadStitchingExperimentStore(store).ok(), "session directories may not redirect through symlinks");
  fs::remove(store.directory / "sessions");
  fs::rename(store.directory / "sessions.saved", store.directory / "sessions");
  write_file(store.directory / ".write-abandoned", "incomplete replacement");
  ok &= expect(
      LoadStitchingExperimentStore(store).ok(), "an interrupted staging write leaves the committed index readable");

  auto remove_parent = make_record(store, "remove-parent", 1);
  auto remove_child = make_record(store, "remove-child", 1);
  remove_child.baseline_sequence = remove_parent.sequence;
  remove_child.baseline_workspace_key =
      remove_parent.workspace.game_directory.lexically_relative(store.directory).generic_string();
  ok &= expect(
      SaveStitchingExperiment(store, remove_parent).ok() && SaveStitchingExperiment(store, remove_child).ok(),
      "queued dependencies can span retained sessions");
  const auto parent_key = remove_parent.workspace.game_directory.lexically_relative(store.directory).generic_string();
  const auto child_key = remove_child.workspace.game_directory.lexically_relative(store.directory).generic_string();
  ok &= expect(
      !RemoveStitchingExperiments(store, {parent_key}).ok(), "a surviving dependent prevents removing its input owner");
  ok &= expect(
      RemoveStitchingExperiments(store, {parent_key, child_key}).ok() &&
          !fs::exists(remove_parent.workspace.game_directory) && !fs::exists(remove_child.workspace.game_directory),
      "explicit queued removal discards the selected rows and their private files together");
  ok &= expect(
      !RemoveStitchingExperiments(
           store,
           {merge_a.workspace.game_directory.lexically_relative(store.directory).generic_string(),
            merge_b.workspace.game_directory.lexically_relative(store.directory).generic_string()})
           .ok(),
      "queued count reservations must be released before their workspaces can be removed");

  const fs::path promoted = store.game_directory / "player-frame-inputs" / plan.fingerprint;
  fs::create_directories(promoted);
  write_file(promoted / "main-input.png", "promoted input sentinel");
  const auto main_config = read_file(store.game_directory / "config.yaml");
  const fs::path stable_lock = store.game_directory / ".stitching-experiments.lock";
  // Both Open and Discard must serialize on an inode outside the directory
  // being removed. Locking an old, subsequently unlinked cache lock is unsafe.
  for (bool discard : {false, true}) {
    const int held_lock = ::open(stable_lock.c_str(), O_RDWR | O_CLOEXEC);
    if (held_lock < 0 || ::flock(held_lock, LOCK_EX) != 0)
      throw std::runtime_error("Cannot hold stable game-level store lock");
    int notification[2];
    if (::pipe(notification) != 0)
      throw std::runtime_error("Cannot create lock serialization fixture");
    const pid_t child = ::fork();
    if (child == 0) {
      ::close(held_lock);
      ::close(notification[0]);
      const char ready = 'r';
      if (::write(notification[1], &ready, 1) != 1)
        ::_exit(2);
      const bool success = discard ? DiscardStitchingExperimentStore(store).ok()
                                   : OpenStitchingExperimentStore(store.game_directory).ok();
      const char done = success ? 'y' : 'n';
      if (::write(notification[1], &done, 1) != 1)
        ::_exit(2);
      ::_exit(success ? 0 : 1);
    }
    if (child < 0)
      throw std::runtime_error("Cannot start lock serialization fixture");
    ::close(notification[1]);
    char state = 0;
    if (::read(notification[0], &state, 1) != 1 || state != 'r')
      throw std::runtime_error("Concurrent cache operation did not start");
    pollfd poll_state{notification[0], POLLIN, 0};
    const int early_result = ::poll(&poll_state, 1, 100);
    ::flock(held_lock, LOCK_UN);
    ::close(held_lock);
    ok &= expect(
        early_result == 0,
        discard ? "discard waits for the stable game-level lock" : "reopen waits for the stable game-level lock");
    ok &= expect(
        ::read(notification[0], &state, 1) == 1 && state == 'y',
        "serialized cache operation completes after lock release");
    ::close(notification[0]);
    int status = 0;
    ok &= expect(
        ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "serialized cache worker exits successfully");
  }
  ok &= expect(
      !fs::exists(store.directory) && fs::is_regular_file(stable_lock),
      "explicit discard removes cache data while preserving the stable lock inode");
  ok &= expect(
      read_file(promoted / "main-input.png") == "promoted input sentinel" &&
          read_file(store.game_directory / "config.yaml") == main_config,
      "discard preserves promoted main inputs and selected main configuration");
  auto reopened = OpenStitchingExperimentStore(store.game_directory);
  if (!expect(reopened.ok(), "opening after explicit discard creates a fresh store"))
    return false;
  auto empty = LoadStitchingExperimentStore(*reopened);
  ok &= expect(
      empty.ok() && empty->experiments.empty() && empty->selected_by_count.empty(),
      "discarded cache does not resurrect deleted rows");
  ok &= expect(
      !SaveStitchingExperiment(*reopened, ordinary).ok(),
      "a stale pre-discard row cannot write into a new empty history");
  ok &= deletion_recovery(root);
  ok &= failed_attempt_removal(root);
  ok &= completed_result_removal(root);
  return ok;
}

} // namespace

int main() {
  std::string pattern = (fs::temp_directory_path() / "stitch-experiment-store-test-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* created = ::mkdtemp(writable.data());
  if (!created)
    return 1;
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  } cleanup{created};
  try {
    return run(cleanup.root) ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Experiment store fixture failed: " << error.what() << '\n';
    return 1;
  }
}
