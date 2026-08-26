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
  auto invalidation_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
  ok &= expect(invalidation_lock.ok(), "rink invalidation test must acquire the config transaction");
  if (invalidation_lock.ok()) {
    ::setenv("HM_TEST_RINK_INVALIDATION_FAIL_AFTER_REMOVE", "1", 1);
    const auto failed = hm::stitching::publish_game_config_without_rink_masks(
        root, "generation: new\n", /*remove_stitched_snapshot=*/true);
    ::unsetenv("HM_TEST_RINK_INVALIDATION_FAIL_AFTER_REMOVE");
    ok &= expect(!failed.ok(), "injected rink invalidation failure must abort publication");
    const YAML::Node rolled_back = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        rolled_back["generation"].as<std::string>() == "old" && fs::is_regular_file(root / "rink_mask_0.png") &&
            fs::is_regular_file(root / "rink_mask_1.png") && fs::is_regular_file(root / "s.png"),
        "failed rink invalidation must restore the complete prior config/canvas generation");
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
  }

  fs::remove_all(root);
  return ok ? 0 : 1;
}
