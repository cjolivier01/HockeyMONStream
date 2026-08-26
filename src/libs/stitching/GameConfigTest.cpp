#include "hstream/src/libs/stitching/GameConfig.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

int update_key(const fs::path& root, const std::string& key) {
  auto lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
  if (!lock.ok())
    return 1;
  YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
  config["writers"][key] = true;
  return hm::stitching::publish_game_config(root, YAML::Dump(config) + "\n").ok() ? 0 : 2;
}

} // namespace

int main() {
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("game-config-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  std::ofstream(root / "config.yaml") << "unrelated:\n  keep: true\n";

  auto first_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
  ok &= expect(first_lock.ok(), "first game-config transaction must lock");
  std::atomic<bool> reader_finished{false};
  std::atomic<bool> reader_succeeded{false};
  std::thread waiter([&]() {
    auto loaded = hm::stitching::load_game_config_file(root / "config.yaml");
    reader_succeeded = loaded.ok() && loaded->has_value();
    reader_finished = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ok &= expect(!reader_finished.load(), "a runtime config read must wait for an in-progress rink/config publication");
  if (first_lock.ok())
    first_lock->reset();
  waiter.join();
  ok &= expect(reader_succeeded.load(), "a waiting runtime config read must resume with a complete generation");

  const pid_t first = ::fork();
  if (first == 0)
    _exit(update_key(root, "first"));
  const pid_t second = ::fork();
  if (second == 0)
    _exit(update_key(root, "second"));
  int first_status = 0;
  int second_status = 0;
  ::waitpid(first, &first_status, 0);
  ::waitpid(second, &second_status, 0);
  ok &= expect(
      WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0 && WIFEXITED(second_status) &&
          WEXITSTATUS(second_status) == 0,
      "concurrent config writers must both publish successfully");
  YAML::Node merged = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      merged["unrelated"]["keep"].as<bool>() && merged["writers"]["first"].as<bool>() &&
          merged["writers"]["second"].as<bool>(),
      "serialized disjoint config updates must preserve every key");
  struct stat metadata{};
  ok &= expect(
      ::stat((root / "config.yaml").c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600,
      "atomically published private config must be owner-only");

  YAML::Node absent_baseline;
  YAML::Node first_save(YAML::NodeType::Map);
  first_save["pipeline"]["generated"] = true;
  YAML::Node concurrently_created(YAML::NodeType::Map);
  concurrently_created["hstream_ui"]["keep"] = true;
  concurrently_created["pipeline"]["remote"] = true;
  const YAML::Node first_merged =
      hm::stitching::apply_game_config_diff(absent_baseline, first_save, concurrently_created);
  ok &= expect(
      first_merged["pipeline"]["generated"].as<bool>() && first_merged["pipeline"]["remote"].as<bool>() &&
          first_merged["hstream_ui"]["keep"].as<bool>(),
      "a first save after an absent baseline must preserve concurrent siblings in the same map");

  YAML::Node removed_selection(YAML::NodeType::Map);
  removed_selection["hstream_ui"]["video_roles"]["right"] = YAML::Node(YAML::NodeType::Sequence);
  YAML::Node original_selection(YAML::NodeType::Map);
  original_selection["hstream_ui"]["video_roles"]["right"].push_back("A.mp4");
  original_selection["game"]["stitching"]["frame_offsets"]["right"] = 90;
  YAML::Node concurrent_selection = YAML::Clone(removed_selection);
  concurrent_selection["hstream_ui"]["video_roles"]["right"].push_back("B.mp4");
  concurrent_selection["game"]["stitching"]["frame_offsets"]["right"] = 91;
  concurrent_selection["concurrent"]["keep"] = true;
  const YAML::Node rollback_merged =
      hm::stitching::merge_game_config_rollback(removed_selection, original_selection, concurrent_selection);
  ok &= expect(
      rollback_merged["hstream_ui"]["video_roles"]["right"].size() == 2 &&
          rollback_merged["hstream_ui"]["video_roles"]["right"][0].as<std::string>() == "A.mp4" &&
          rollback_merged["hstream_ui"]["video_roles"]["right"][1].as<std::string>() == "B.mp4" &&
          rollback_merged["game"]["stitching"]["frame_offsets"]["right"].as<int>() == 91 &&
          rollback_merged["concurrent"]["keep"].as<bool>(),
      "rollback merging must restore removed sequence entries without overwriting same-path concurrent updates");

  YAML::Node auto_published(YAML::NodeType::Map);
  YAML::Node pre_auto(YAML::NodeType::Map);
  pre_auto["hstream_ui"]["video_roles"]["left"].push_back("A.mp4");
  YAML::Node post_auto(YAML::NodeType::Map);
  post_auto["hstream_ui"]["video_roles"]["left"].push_back("B.mp4");
  const YAML::Node absent_sequence_merged =
      hm::stitching::merge_game_config_rollback(auto_published, pre_auto, post_auto);
  ok &= expect(
      absent_sequence_merged["hstream_ui"]["video_roles"]["left"].size() == 2 &&
          absent_sequence_merged["hstream_ui"]["video_roles"]["left"][0].as<std::string>() == "A.mp4" &&
          absent_sequence_merged["hstream_ui"]["video_roles"]["left"][1].as<std::string>() == "B.mp4",
      "rollback merging must restore a sequence deleted by the baseline alongside a newer sequence");

  YAML::Node ordered_baseline(YAML::NodeType::Sequence);
  ordered_baseline.push_back("C.mp4");
  YAML::Node ordered_desired(YAML::NodeType::Sequence);
  ordered_desired.push_back("A.mp4");
  ordered_desired.push_back("C.mp4");
  YAML::Node ordered_latest(YAML::NodeType::Sequence);
  ordered_latest.push_back("B.mp4");
  ordered_latest.push_back("C.mp4");
  const YAML::Node ordered_merged =
      hm::stitching::merge_game_config_rollback(ordered_baseline, ordered_desired, ordered_latest);
  ok &= expect(
      ordered_merged.size() == 3 && ordered_merged[0].as<std::string>() == "B.mp4" &&
          ordered_merged[1].as<std::string>() == "A.mp4" && ordered_merged[2].as<std::string>() == "C.mp4",
      "rollback merging must preserve the latest sequence's relative order around restored entries");

  const fs::path interrupted = root / ".hstream-rink-interrupted";
  fs::create_directories(interrupted / "previous");
  std::ofstream(interrupted / "previous" / "config.yaml") << "recovered:\n  old: true\n";
  std::ofstream(interrupted / "previous" / "rink_mask_0.png") << "old-mask";
  std::ofstream(interrupted / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(interrupted / "state") << "PREPARED\n";
  std::ofstream(root / "config.yaml") << "interrupted: true\n";
  std::ofstream(root / "rink_mask_0.png") << "new-mask";
  auto recovered_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      recovered_read.ok() && recovered_read->has_value(),
      "recovery-aware config reads must recover a prepared rink transaction");
  if (recovered_read.ok() && recovered_read->has_value()) {
    auto recovered_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    ok &= expect(recovered_lock.ok(), "config update after recovery must lock");
    if (recovered_lock.ok()) {
      YAML::Node config = **recovered_read;
      config["after_recovery"] = true;
      ok &= expect(
          hm::stitching::publish_game_config(root, YAML::Dump(config) + "\n").ok(),
          "a config update after rink recovery must publish");
    }
  }
  YAML::Node recovered = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      recovered["recovered"]["old"].as<bool>() && recovered["after_recovery"].as<bool>() && !recovered["interrupted"],
      "rink recovery must precede and preserve the subsequent config mutation");
  ok &= expect(!fs::exists(interrupted), "recovered rink journal must be removed");

  std::ofstream(root / "config.yaml") << "generation: old\n";
  std::ofstream(root / "rink_mask_0.png") << "old-mask-zero";
  std::ofstream(root / "rink_mask_1.png") << "old-mask-one";
  std::ofstream(root / "s.png") << "old-stitched-snapshot";
  struct stat original_snapshot_metadata{};
  ok &= expect(
      ::stat((root / "s.png").c_str(), &original_snapshot_metadata) == 0,
      "rink invalidation snapshot fixture must have a stable inode");
  auto invalidation_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
  ok &= expect(invalidation_lock.ok(), "rink invalidation test must acquire the config transaction");
  if (invalidation_lock.ok()) {
    ::setenv("HM_TEST_RINK_INVALIDATION_FAIL_AFTER_REMOVE", "1", 1);
    const auto failed = hm::stitching::publish_game_config_without_rink_masks(
        root, "generation: new\n", /*remove_stitched_snapshot=*/true);
    ::unsetenv("HM_TEST_RINK_INVALIDATION_FAIL_AFTER_REMOVE");
    ok &= expect(!failed.ok(), "injected rink invalidation failure must abort publication");
    const YAML::Node rolled_back = YAML::LoadFile((root / "config.yaml").string());
    struct stat restored_snapshot_metadata{};
    ok &= expect(
        rolled_back["generation"].as<std::string>() == "old" && fs::is_regular_file(root / "rink_mask_0.png") &&
            fs::is_regular_file(root / "rink_mask_1.png") && fs::is_regular_file(root / "s.png") &&
            ::stat((root / "s.png").c_str(), &restored_snapshot_metadata) == 0 &&
            restored_snapshot_metadata.st_ino == original_snapshot_metadata.st_ino &&
            restored_snapshot_metadata.st_dev == original_snapshot_metadata.st_dev,
        "failed rink invalidation must restore the complete prior config/canvas generation without copying snapshots");
    const auto published = hm::stitching::publish_game_config_without_rink_masks(
        root, "generation: new\n", /*remove_stitched_snapshot=*/true);
    ok &= expect(
        published.ok() && *published == 3 &&
            YAML::LoadFile((root / "config.yaml").string())["generation"].as<std::string>() == "new" &&
            !fs::exists(root / "rink_mask_0.png") && !fs::exists(root / "rink_mask_1.png") &&
            !fs::exists(root / "s.png"),
        "successful canvas invalidation must atomically publish config and remove every canvas-relative artifact");
    struct stat invalidated_metadata{};
    ok &= expect(
        ::stat((root / "config.yaml").c_str(), &invalidated_metadata) == 0 &&
            (invalidated_metadata.st_mode & 0777) == 0600,
        "rink invalidation must publish the replacement private config as owner-only");

    ok &= expect(
        hm::stitching::validate_no_pending_live_stitched_output_authorization_file_locked(root / "config.yaml").ok(),
        "Hugin publication validation must allow a config without a live authorization");
    ok &= expect(
        hm::stitching::publish_game_config(root, "rink:\n  stitched_output_pending_generation: pending-generation\n")
            .ok(),
        "incomplete live authorization fixture must publish");
    ok &= expect(
        absl::IsInvalidArgument(
            hm::stitching::validate_no_pending_live_stitched_output_authorization_file_locked(root / "config.yaml")),
        "Hugin publication validation must reject an incomplete live authorization");
    const auto live_owner = hm::stitching::current_live_stitched_output_owner_process();
    ok &= expect(live_owner.ok(), "live authorization fixture must identify its owner process");
    const size_t boot_separator = live_owner.ok() ? live_owner->rfind(':') : std::string::npos;
    ok &= expect(
        boot_separator != std::string::npos && boot_separator != live_owner->find(':') &&
            hm::stitching::live_stitched_output_owner_process_is_active(*live_owner).value_or(false),
        "the current process identity must include and match the Linux boot identity");
    if (boot_separator != std::string::npos) {
      const std::string legacy_owner = live_owner->substr(0, boot_separator);
      const std::string wrong_boot_owner = legacy_owner + ":different-linux-boot";
      ok &= expect(
          !hm::stitching::live_stitched_output_owner_process_is_active(legacy_owner).value_or(true),
          "a legacy owner identity without a boot ID must not retain publication authority");
      ok &= expect(
          !hm::stitching::live_stitched_output_owner_process_is_active(wrong_boot_owner).value_or(true),
          "an owner identity from another Linux boot must not retain publication authority");
    }
    int owner_pipe[2] = {-1, -1};
    const bool owner_pipe_created = ::pipe(owner_pipe) == 0;
    const pid_t zombie_owner = owner_pipe_created ? ::fork() : -1;
    if (zombie_owner == 0) {
      ::close(owner_pipe[0]);
      const auto identity = hm::stitching::current_live_stitched_output_owner_process();
      const ssize_t written =
          identity.ok() ? ::write(owner_pipe[1], identity->data(), identity->size()) : static_cast<ssize_t>(-1);
      _exit(identity.ok() && written == static_cast<ssize_t>(identity->size()) ? 0 : 1);
    }
    std::string zombie_identity;
    if (zombie_owner > 0) {
      ::close(owner_pipe[1]);
      char buffer[256];
      ssize_t read_size = 0;
      while ((read_size = ::read(owner_pipe[0], buffer, sizeof(buffer))) > 0)
        zombie_identity.append(buffer, static_cast<size_t>(read_size));
      ::close(owner_pipe[0]);
    } else if (owner_pipe_created) {
      ::close(owner_pipe[0]);
      ::close(owner_pipe[1]);
    }
    const auto zombie_active = zombie_identity.empty()
        ? absl::StatusOr<bool>(absl::InternalError("Zombie owner fixture did not report an identity"))
        : hm::stitching::live_stitched_output_owner_process_is_active(zombie_identity);
    int zombie_status = 0;
    if (zombie_owner > 0)
      ::waitpid(zombie_owner, &zombie_status, 0);
    ok &= expect(
        zombie_owner > 0 && zombie_active.ok() && !*zombie_active && WIFEXITED(zombie_status) &&
            WEXITSTATUS(zombie_status) == 0,
        "an exited but unreaped owner process must not retain publication authority");
    ok &= expect(
        hm::stitching::publish_game_config(
            root,
            "rink:\n  stitched_output_pending_generation: pending-generation\n"
            "  stitched_output_pending_authorization_id: pending-authorization\n"
            "  stitched_output_pending_owner_process: " +
                (live_owner.ok() ? *live_owner : std::string("invalid")) + "\n")
            .ok(),
        "complete live authorization fixture must publish");
    ok &= expect(
        absl::IsAborted(
            hm::stitching::validate_no_pending_live_stitched_output_authorization_file_locked(root / "config.yaml")),
        "Hugin publication validation must fence artifact replacement during a live authorization");
    ok &= expect(
        hm::stitching::publish_game_config(
            root,
            "rink:\n  stitched_output_pending_generation: pending-generation\n"
            "  stitched_output_pending_authorization_id: pending-authorization\n"
            "  stitched_output_pending_owner_process: 999999999:1\n")
            .ok(),
        "crashed live authorization fixture must publish");
    ok &= expect(
        hm::stitching::validate_no_pending_live_stitched_output_authorization_file_locked(root / "config.yaml").ok(),
        "a dead owner process must not permanently fence artifact publication");
  }

  fs::remove_all(root);
  return ok ? 0 : 1;
}
