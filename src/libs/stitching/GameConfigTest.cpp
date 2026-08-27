#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
  ::setenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN", "1", 1);
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("game-config-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  std::ofstream(root / "config.yaml") << "unrelated:\n  keep: true\n";

  YAML::Node projection_config;
  projection_config["stitching"]["projection_parameters"]["general-panini"].push_back(100);
  projection_config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  projection_config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  auto panini_parameters = hm::stitching::read_stitch_projection_parameters(
      projection_config, hm::stitching::StitchProjection::kGeneralPanini);
  ok &= expect(
      panini_parameters.ok() && *panini_parameters == std::vector<double>({100.0, 0.0, 0.0}),
      "General Panini projection parameters must parse in Hugin order");
  YAML::Node missing_projection_config;
  auto default_panini_parameters = hm::stitching::read_stitch_projection_parameters(
      missing_projection_config, hm::stitching::StitchProjection::kGeneralPanini);
  ok &= expect(
      default_panini_parameters.ok() && *default_panini_parameters == std::vector<double>({100.0, 0.0, 0.0}),
      "older configs must migrate to General Panini's 100,0,0 defaults");
  YAML::Node invalid_projection_config = YAML::Clone(projection_config);
  invalid_projection_config["stitching"]["projection_parameters"]["general-panini"][0] = 151;
  ok &= expect(
      !hm::stitching::read_stitch_projection_parameters(
           invalid_projection_config, hm::stitching::StitchProjection::kGeneralPanini)
           .ok(),
      "out-of-range General Panini projection parameters must be rejected");
  invalid_projection_config = YAML::Clone(projection_config);
  invalid_projection_config["stitching"]["projection_parameters"]["general-panini"].remove(2);
  ok &= expect(
      !hm::stitching::read_stitch_projection_parameters(
           invalid_projection_config, hm::stitching::StitchProjection::kGeneralPanini)
           .ok(),
      "General Panini projection parameters with the wrong count must be rejected");
  invalid_projection_config = YAML::Clone(projection_config);
  invalid_projection_config["stitching"]["projection_parameters"]["panini"].push_back(1);
  ok &= expect(
      !hm::stitching::read_stitch_projection_parameters(
           invalid_projection_config, hm::stitching::StitchProjection::kGeneralPanini)
           .ok(),
      "fixed Panini projections must reject unsupported adjustable parameters");
  invalid_projection_config = YAML::Node(YAML::NodeType::Map);
  invalid_projection_config["stitching"]["projection_parameters"]["general_panini"] = YAML::Load("[100, 0, 0]");
  ok &= expect(
      !hm::stitching::read_stitch_projection_parameters(
           invalid_projection_config, hm::stitching::StitchProjection::kGeneralPanini)
           .ok(),
      "projection parameter maps must reject non-canonical keys instead of silently ignoring their values");

  auto first_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
  ok &= expect(first_lock.ok(), "first game-config transaction must lock");
  const auto unavailable_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(root);
  ok &= expect(
      first_lock.ok() && absl::IsUnavailable(unavailable_lock.status()),
      "nonblocking config transactions must report an active publisher");
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
  auto available_lock = hm::stitching::GameConfigTransactionLock::TryAcquire(root);
  ok &= expect(available_lock.ok(), "nonblocking config transactions must recover and lock when idle");
  if (available_lock.ok())
    available_lock->reset();
  const fs::path symlinked_game_root = root.parent_path() / (root.filename().string() + "-symlink");
  fs::create_directory_symlink(root, symlinked_game_root);
  const auto symlinked_game_read = hm::stitching::load_game_config_file(symlinked_game_root / "config.yaml");
  ok &= expect(
      symlinked_game_read.ok() && symlinked_game_read->has_value(),
      "config recovery must follow and pin a caller-selected symlinked game directory");
  fs::remove(symlinked_game_root);

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
  const fs::path rollback_state_target = root / "rink-rollback-state-target";
  std::ofstream(rollback_state_target) << "preserved\n";
  fs::create_symlink(rollback_state_target, interrupted / "state.rolled_back");
  ::setenv("HM_TEST_RINK_DISABLE_LINK_CLONE", "1", 1);
  auto recovered_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ::unsetenv("HM_TEST_RINK_DISABLE_LINK_CLONE");
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
  ok &= expect(
      [&]() {
        std::ifstream input(rollback_state_target, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) == "preserved\n";
      }(),
      "rink rollback state publication must replace a journal symlink without following it");
  ok &= expect(!fs::exists(interrupted), "recovered rink journal must be removed");
  fs::remove(rollback_state_target);

  const fs::path symlinked_transaction = root / ".hstream-rink-symlinked-transaction";
  const fs::path transaction_target = root / "rink-transaction-symlink-target";
  fs::create_directories(transaction_target / "previous");
  fs::copy_file(root / "config.yaml", transaction_target / "previous" / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", transaction_target / "previous" / "rink_mask_0.png");
  std::ofstream(transaction_target / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(transaction_target / "state") << "PREPARED\n";
  fs::create_directory_symlink(transaction_target, symlinked_transaction);
  const std::string config_before_symlinked_transaction = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  const auto symlinked_transaction_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsFailedPrecondition(symlinked_transaction_read.status()) &&
          fs::is_symlink(fs::symlink_status(symlinked_transaction)) &&
          [&]() {
            std::ifstream input(transaction_target / "state", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) == "PREPARED\n";
          }() &&
          [&]() {
            std::ifstream input(root / "config.yaml", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                config_before_symlinked_transaction;
          }(),
      "config recovery must reject a symlinked transaction directory without mutating either generation");
  fs::remove(symlinked_transaction);
  fs::remove_all(transaction_target);

  const fs::path symlinked_previous = root / ".hstream-rink-symlinked-previous";
  const fs::path previous_target = root / "rink-previous-symlink-target";
  fs::create_directories(symlinked_previous);
  fs::create_directories(previous_target);
  fs::copy_file(root / "config.yaml", previous_target / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", previous_target / "rink_mask_0.png");
  fs::create_directory_symlink(previous_target, symlinked_previous / "previous");
  std::ofstream(symlinked_previous / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(symlinked_previous / "state") << "PREPARED\n";
  const auto symlinked_previous_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsFailedPrecondition(symlinked_previous_read.status()) && fs::exists(symlinked_previous) &&
          fs::is_symlink(fs::symlink_status(symlinked_previous / "previous")) &&
          fs::is_regular_file(previous_target / "config.yaml"),
      "config recovery must reject a symlinked rollback directory without consuming its target");
  fs::remove_all(symlinked_previous);
  fs::remove_all(previous_target);

  const auto root_config_contents = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const fs::path missing_previous = root / ".hstream-rink-missing-previous";
  fs::create_directories(missing_previous);
  std::ofstream(missing_previous / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(missing_previous / "state") << "PREPARED\n";
  const std::string config_before_missing_previous = root_config_contents();
  const auto missing_previous_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsFailedPrecondition(missing_previous_read.status()) && fs::exists(missing_previous) &&
          root_config_contents() == config_before_missing_previous,
      "config recovery must reject a prepared journal without backups before mutating committed files");
  fs::remove_all(missing_previous);

  const fs::path unmanifested_backup = root / ".hstream-rink-unmanifested-backup";
  fs::create_directories(unmanifested_backup / "previous");
  fs::copy_file(root / "config.yaml", unmanifested_backup / "previous" / "config.yaml");
  std::ofstream(unmanifested_backup / "previous" / "s.png") << "unmanifested";
  std::ofstream(unmanifested_backup / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(unmanifested_backup / "state") << "PREPARED\n";
  const std::string config_before_unmanifested_backup = root_config_contents();
  const auto unmanifested_backup_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsInvalidArgument(unmanifested_backup_read.status()) && fs::exists(unmanifested_backup) &&
          root_config_contents() == config_before_unmanifested_backup && !fs::exists(root / "s.png"),
      "config recovery must reject an unmanifested validly named backup before root mutation");
  fs::remove_all(unmanifested_backup);

  const fs::path oversized_backup = root / ".hstream-rink-oversized-backup";
  fs::create_directories(oversized_backup / "previous");
  fs::copy_file(root / "config.yaml", oversized_backup / "previous" / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", oversized_backup / "previous" / "rink_mask_0.png");
  const bool oversized_backup_created =
      ::truncate((oversized_backup / "previous" / "config.yaml").c_str(), 16LL * 1024LL * 1024LL + 1) == 0;
  std::ofstream(oversized_backup / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(oversized_backup / "state") << "PREPARED\n";
  const std::string config_before_oversized_backup = root_config_contents();
  const auto oversized_backup_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      oversized_backup_created && absl::IsFailedPrecondition(oversized_backup_read.status()) &&
          fs::exists(oversized_backup) && root_config_contents() == config_before_oversized_backup,
      "config recovery must reject an oversized backup before deleting the committed generation");
  fs::remove_all(oversized_backup);

  const fs::path over_count = root / ".hstream-rink-over-count";
  fs::create_directories(over_count / "previous");
  {
    std::ofstream manifest(over_count / "new-files");
    manifest << "config.yaml\n";
    for (size_t index = 0; index < 65; ++index)
      manifest << "rink_mask_" << index << ".png\n";
  }
  std::ofstream(over_count / "state") << "PREPARED\n";
  const std::string config_before_over_count = root_config_contents();
  const auto over_count_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsResourceExhausted(over_count_read.status()) && fs::exists(over_count) &&
          root_config_contents() == config_before_over_count,
      "config recovery must reject over-count rink journals before staging any restore");
  fs::remove_all(over_count);

  const fs::path symlinked_manifest = root / ".hstream-rink-symlinked-manifest";
  const fs::path manifest_target = root / "rink-manifest-symlink-target";
  fs::create_directories(symlinked_manifest / "previous");
  fs::copy_file(root / "config.yaml", symlinked_manifest / "previous" / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", symlinked_manifest / "previous" / "rink_mask_0.png");
  std::ofstream(manifest_target) << "config.yaml\nrink_mask_0.png\n";
  fs::create_symlink(manifest_target, symlinked_manifest / "new-files");
  std::ofstream(symlinked_manifest / "state") << "PREPARED\n";
  const std::string config_before_symlinked_manifest = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  const auto symlinked_manifest_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsFailedPrecondition(symlinked_manifest_read.status()) && fs::exists(symlinked_manifest) &&
          fs::is_symlink(fs::symlink_status(symlinked_manifest / "new-files")) &&
          [&]() {
            std::ifstream input(root / "config.yaml", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                config_before_symlinked_manifest;
          }(),
      "config recovery must reject a symlinked manifest before removing committed artifacts");
  fs::remove_all(symlinked_manifest);
  fs::remove(manifest_target);

  const fs::path unreadable = root / ".hstream-rink-unreadable";
  fs::create_directories(unreadable / "previous");
  fs::copy_file(root / "config.yaml", unreadable / "previous" / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", unreadable / "previous" / "rink_mask_0.png");
  std::ofstream(unreadable / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(unreadable / "state") << "PREPARED\n";
  const std::string config_before_unreadable_recovery = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  fs::permissions(unreadable / "previous", fs::perms::owner_exec, fs::perm_options::replace);
  const auto unreadable_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      !unreadable_read.ok() && fs::exists(unreadable) && fs::is_regular_file(root / "config.yaml") &&
          [&]() {
            std::ifstream input(root / "config.yaml", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                config_before_unreadable_recovery;
          }(),
      "failed config backup enumeration must preserve the journal and committed config");
  fs::permissions(unreadable / "previous", fs::perms::owner_all, fs::perm_options::replace);
  fs::remove_all(unreadable);

  const fs::path symlinked_backup = root / ".hstream-rink-symlinked-backup";
  const fs::path symlinked_backup_target = root / "config-backup-symlink-target.yaml";
  fs::create_directories(symlinked_backup / "previous");
  fs::copy_file(root / "config.yaml", symlinked_backup_target);
  fs::create_symlink(symlinked_backup_target, symlinked_backup / "previous" / "config.yaml");
  fs::copy_file(root / "rink_mask_0.png", symlinked_backup / "previous" / "rink_mask_0.png");
  std::ofstream(symlinked_backup / "new-files") << "config.yaml\nrink_mask_0.png\n";
  std::ofstream(symlinked_backup / "state") << "PREPARED\n";
  const std::string config_before_symlinked_backup = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  const auto symlinked_backup_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      !symlinked_backup_read.ok() && fs::exists(symlinked_backup) &&
          fs::is_symlink(fs::symlink_status(symlinked_backup / "previous" / "config.yaml")) &&
          [&]() {
            std::ifstream input(root / "config.yaml", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                config_before_symlinked_backup;
          }(),
      "config recovery must reject a symlinked rollback backup before mutating the committed config");
  fs::remove_all(symlinked_backup);
  fs::remove(symlinked_backup_target);

  const fs::path fifo_state = root / ".hstream-rink-fifo-state";
  fs::create_directories(fifo_state);
  ok &= expect(::mkfifo((fifo_state / "state").c_str(), 0600) == 0, "FIFO transaction-state fixture must be created");
  const auto fifo_state_read = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsFailedPrecondition(fifo_state_read.status()) && fs::exists(fifo_state),
      "config recovery must reject a FIFO state without blocking");
  fs::remove_all(fifo_state);

  std::ofstream(root / "config.yaml") << "generation: old\n";
  std::ofstream(root / "rink_mask_0.png") << "old-mask-zero";
  std::ofstream(root / "rink_mask_1.png") << "old-mask-one";
  std::ofstream(root / "s.png") << "old-stitched-snapshot";
  absl::StatusOr<size_t> oversized_invalidation = absl::FailedPreconditionError("fixture lock unavailable");
  const bool oversized_mask_created = ::truncate((root / "rink_mask_0.png").c_str(), 128LL * 1024LL * 1024LL + 1) == 0;
  {
    auto oversized_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    if (oversized_lock.ok()) {
      ::setenv("HM_TEST_RINK_DISABLE_LINK_CLONE", "1", 1);
      oversized_invalidation = hm::stitching::publish_game_config_without_rink_masks(
          root, "generation: oversized-must-not-publish\n", /*remove_stitched_snapshot=*/false);
      ::unsetenv("HM_TEST_RINK_DISABLE_LINK_CLONE");
    }
  }
  ok &= expect(
      oversized_mask_created && absl::IsFailedPrecondition(oversized_invalidation.status()) &&
          YAML::LoadFile((root / "config.yaml").string())["generation"].as<std::string>() == "old",
      "rink rollback publication must reject oversized masks before portable copying");
  std::ofstream(root / "rink_mask_0.png", std::ios::trunc) << "old-mask-zero";
  const fs::path invalidation_symlink_target = root / "snapshot-symlink-target.png";
  std::error_code invalidation_symlink_error;
  fs::rename(root / "s.png", invalidation_symlink_target, invalidation_symlink_error);
  fs::create_symlink(invalidation_symlink_target, root / "s.png", invalidation_symlink_error);
  absl::StatusOr<size_t> symlinked_invalidation = absl::FailedPreconditionError("fixture lock unavailable");
  {
    auto symlinked_invalidation_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    if (symlinked_invalidation_lock.ok()) {
      symlinked_invalidation = hm::stitching::publish_game_config_without_rink_masks(
          root, "generation: new\n", /*remove_stitched_snapshot=*/true);
    }
  }
  ok &= expect(
      !invalidation_symlink_error && absl::IsFailedPrecondition(symlinked_invalidation.status()) &&
          fs::is_symlink(fs::symlink_status(root / "s.png")) &&
          [&]() {
            std::ifstream input(invalidation_symlink_target, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                "old-stitched-snapshot";
          }() &&
          YAML::LoadFile((root / "config.yaml").string())["generation"].as<std::string>() == "old",
      "rink invalidation must reject a symlinked snapshot without mutating its target or config");
  fs::remove(root / "s.png", invalidation_symlink_error);
  fs::rename(invalidation_symlink_target, root / "s.png", invalidation_symlink_error);
  ok &= expect(!invalidation_symlink_error, "symlinked invalidation fixture must restore the regular snapshot");
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
            restored_snapshot_metadata.st_ino != original_snapshot_metadata.st_ino &&
            restored_snapshot_metadata.st_dev == original_snapshot_metadata.st_dev &&
            [&]() {
              std::ifstream input(root / "s.png", std::ios::binary);
              return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()) ==
                  "old-stitched-snapshot";
            }(),
        "failed rink invalidation must restore an independent copy of the complete prior config/canvas generation");
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
    siginfo_t zombie_info{};
    int zombie_wait_result = -1;
    if (zombie_owner > 0) {
      do {
        zombie_wait_result = ::waitid(P_PID, zombie_owner, &zombie_info, WEXITED | WNOWAIT);
      } while (zombie_wait_result != 0 && errno == EINTR);
    }
    const auto zombie_active = zombie_identity.empty()
        ? absl::StatusOr<bool>(absl::InternalError("Zombie owner fixture did not report an identity"))
        : hm::stitching::live_stitched_output_owner_process_is_active(zombie_identity);
    int zombie_status = 0;
    if (zombie_owner > 0)
      ::waitpid(zombie_owner, &zombie_status, 0);
    ok &= expect(
        zombie_owner > 0 && zombie_wait_result == 0 && zombie_info.si_pid == zombie_owner && zombie_active.ok() &&
            !*zombie_active && WIFEXITED(zombie_status) && WEXITSTATUS(zombie_status) == 0,
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
  if (invalidation_lock.ok())
    invalidation_lock->reset();

  YAML::Node backend_generation(YAML::NodeType::Map);
  backend_generation["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  backend_generation["stitching"]["mapping_backend"] = "opencv-magsac";
  backend_generation["stitching"]["projection"] = "rectilinear";
  backend_generation["stitching"]["run_autooptimizer"] = false;
  backend_generation["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  backend_generation["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "backend-generation-a";
  YAML::Node legacy_backend_claim = backend_generation["hstream_ui"]["stitching_calibration"]["backend_generation"];
  legacy_backend_claim["invalidation_id"] = "backend-generation-a";
  legacy_backend_claim["control_point_matcher"] = "superpoint-lightglue";
  legacy_backend_claim["mapping_backend"] = "opencv-magsac";
  legacy_backend_claim["run_autooptimizer"] = false;
  ok &= expect(
      hm::stitching::publish_game_config(root, YAML::Dump(backend_generation) + "\n").ok(),
      "backend-generation fixture must publish");
  const hm::stitching::StitchingBackendChoices magsac_choices{
      "superpoint-lightglue", "opencv-magsac", "rectilinear", false};
  const hm::stitching::StitchingBackendChoices affine_choices{
      "superpoint-lightglue", "opencv-affine-ransac", "rectilinear", false};
  const absl::Status reserved =
      hm::stitching::reserve_stitching_backend_generation(root, "backend-generation-a", magsac_choices);
  auto claimed = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      reserved.ok() && claimed.ok() && claimed->has_value() &&
          (**claimed)["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection"].as<std::string>() ==
              "rectilinear" &&
          hm::stitching::validate_stitching_backend_generation(**claimed, "backend-generation-a", magsac_choices).ok(),
      "a calibration generation must migrate, reserve, and validate one immutable worker backend tuple");
  const absl::Status conflicting_reservation =
      hm::stitching::reserve_stitching_backend_generation(root, "backend-generation-a", affine_choices);
  auto after_conflict = hm::stitching::load_game_config_file(root / "config.yaml");
  ok &= expect(
      absl::IsAborted(conflicting_reservation) && after_conflict.ok() && after_conflict->has_value() &&
          (**after_conflict)["hstream_ui"]["stitching_calibration"]["backend_generation"]["mapping_backend"]
                  .as<std::string>() == "opencv-magsac",
      "a competing backend tuple must not replace the generation's first reservation");
  if (after_conflict.ok() && after_conflict->has_value()) {
    YAML::Node worker_mismatch = YAML::Clone(**after_conflict);
    worker_mismatch["stitching"]["projection"] = "general-panini";
    ok &= expect(
        absl::IsAborted(
            hm::stitching::validate_stitching_backend_generation(
                worker_mismatch, "backend-generation-a", magsac_choices)),
        "generation validation must reject a worker-visible projection that differs from the immutable claim");
  }

  YAML::Node parameter_generation(YAML::NodeType::Map);
  parameter_generation["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  parameter_generation["stitching"]["mapping_backend"] = "nona";
  parameter_generation["stitching"]["projection"] = "general-panini";
  parameter_generation["stitching"]["run_autooptimizer"] = true;
  parameter_generation["stitching"]["projection_parameters"]["general-panini"].push_back(100);
  parameter_generation["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  parameter_generation["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  parameter_generation["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  parameter_generation["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "backend-generation-b";
  const hm::stitching::StitchingBackendChoices panini_choices{
      "superpoint-lightglue", "nona", "general-panini", true, {100.0, 0.0, 0.0}};
  const absl::Status parameter_reserved = hm::stitching::reserve_stitching_backend_generation_in_config(
      parameter_generation, "backend-generation-b", panini_choices);
  YAML::Node changed_parameters = YAML::Clone(parameter_generation);
  changed_parameters["stitching"]["projection_parameters"]["general-panini"][0] = 120;
  ok &= expect(
      parameter_reserved.ok() &&
          absl::IsAborted(
              hm::stitching::validate_stitching_backend_generation(
                  changed_parameters, "backend-generation-b", panini_choices)),
      "a projection-parameter-only worker change must be fenced by the immutable calibration generation claim");

  const fs::path writer_bounds_root = root.parent_path() / (root.filename().string() + "-writer-bounds");
  fs::remove_all(writer_bounds_root);
  fs::create_directories(writer_bounds_root);
  std::ofstream(writer_bounds_root / "config.yaml") << "generation: old\n";
  for (size_t index = 0; index < hm::stitching::kMaximumRinkTransactionArtifacts; ++index)
    std::ofstream(writer_bounds_root / ("rink_mask_" + std::to_string(index) + ".png")) << "mask\n";
  absl::StatusOr<size_t> over_count_invalidation = absl::FailedPreconditionError("fixture lock unavailable");
  absl::StatusOr<size_t> bounded_invalidation = absl::FailedPreconditionError("fixture lock unavailable");
  auto writer_bounds_lock = hm::stitching::GameConfigTransactionLock::Acquire(writer_bounds_root);
  if (writer_bounds_lock.ok()) {
    over_count_invalidation = hm::stitching::publish_game_config_without_rink_masks(
        writer_bounds_root, "generation: rejected\n", /*remove_stitched_snapshot=*/false);
    fs::remove(
        writer_bounds_root /
        ("rink_mask_" + std::to_string(hm::stitching::kMaximumRinkTransactionArtifacts - 1) + ".png"));
    bounded_invalidation = hm::stitching::publish_game_config_without_rink_masks(
        writer_bounds_root, "generation: accepted\n", /*remove_stitched_snapshot=*/false);
  }
  ok &= expect(
      writer_bounds_lock.ok() && absl::IsResourceExhausted(over_count_invalidation.status()) &&
          bounded_invalidation.ok() && *bounded_invalidation == hm::stitching::kMaximumRinkTransactionArtifacts - 1 &&
          YAML::LoadFile((writer_bounds_root / "config.yaml").string())["generation"].as<std::string>() == "accepted",
      "rink invalidation writer must reject over-limit manifests and accept the exact artifact-count boundary");
  if (writer_bounds_lock.ok())
    writer_bounds_lock->reset();
  fs::remove_all(writer_bounds_root);

  fs::remove_all(root);
  return ok ? 0 : 1;
}
