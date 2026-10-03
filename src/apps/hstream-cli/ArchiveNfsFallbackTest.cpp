#include "src/apps/hstream-cli/configurator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <glib.h>
#include <gst/gst.h>
#include <sys/stat.h>
#include <unistd.h>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace fs = std::filesystem;

int main() {
  const fs::path root = fs::temp_directory_path() / ("archive-nfs-fallback-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  const auto remove_fixture = [&] { fs::remove_all(root); };
  const fs::path configured = root / "recording.mkv";
  const fs::path work = root / "recording.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-aabbccddeeff.mkv";
  const fs::path work_lock = work.string() + ".hstream-owner-lock";
  std::ofstream(work, std::ios::binary);
  std::ofstream(work_lock, std::ios::binary);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto simple_recovery = hm::configurator_internal::recover_stale_archive_work_files(configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  if (!simple_recovery.ok() || !simple_recovery->empty() || fs::exists(work) || fs::exists(work_lock)) {
    std::cerr << "Empty archive recovery failed: " << simple_recovery.status() << '\n';
    remove_fixture();
    return 1;
  }

  const fs::path recorded_configured = root / "recorded.mkv";
  const fs::path recorded_work =
      root / "recorded.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-aabbccddeeff.mkv";
  const fs::path recorded_lock = recorded_work.string() + ".hstream-owner-lock";
  const fs::path recorded_log = recorded_work.string() + ".log";
  const fs::path recorded_video = root / "recorded-finalization-failed.mkv";
  std::ofstream(recorded_work, std::ios::binary) << "recorded video";
  std::ofstream(recorded_lock, std::ios::binary);
  std::ofstream(recorded_log, std::ios::binary) << "recorded log";
  fs::create_hard_link(recorded_log, recorded_log.string() + ".hstream-pin");
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto recorded_recovery = hm::configurator_internal::recover_stale_archive_work_files(recorded_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream recorded_video_stream(recorded_video, std::ios::binary);
  const std::string recorded_video_content{
      std::istreambuf_iterator<char>(recorded_video_stream), std::istreambuf_iterator<char>()};
  recorded_video_stream.close();
  std::ifstream recorded_log_stream(recorded_video.string() + ".log", std::ios::binary);
  const std::string recorded_log_content{
      std::istreambuf_iterator<char>(recorded_log_stream), std::istreambuf_iterator<char>()};
  recorded_log_stream.close();
  const bool recorded_passed = recorded_recovery.ok() && recorded_recovery->size() == 1 &&
      recorded_recovery->front() == recorded_video && recorded_video_content == "recorded video" &&
      recorded_log_content == "recorded log" && !fs::exists(recorded_work) && !fs::exists(recorded_lock);
  if (!recorded_passed)
    std::cerr << "Lock-bearing archive recovery failed: " << recorded_recovery.status() << '\n';

  // Match the two interrupted cleanup records and nested public pins left by
  // an earlier NFS run. Reconciliation must not prevent the next Play.
  const fs::path chained_work =
      root / "chained.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-aabbccddeeff.mkv";
  const fs::path chained_configured = root / "chained.mkv";
  std::ofstream(chained_work, std::ios::binary);
  const fs::path first_pin = chained_work.string() + ".hstream-cleanup-pin";
  const fs::path second_pin = first_pin.string() + ".hstream-cleanup-pin";
  fs::create_hard_link(chained_work, first_pin);
  fs::create_hard_link(chained_work, second_pin);
  const auto add_cleanup_record = [&](const fs::path& directory, const fs::path& target) {
    fs::create_directory(directory);
    const std::string name = target.filename().string();
    gchar* encoded = g_base64_encode(reinterpret_cast<const guchar*>(name.data()), name.size());
    std::ofstream(directory / "owner", std::ios::binary) << "hstream-cleanup-v2\n" << encoded;
    g_free(encoded);
  };
  const fs::path first_cleanup = root / "hstream-cleanup-v2-aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  const fs::path second_cleanup = root / "hstream-cleanup-v2-ffffffff-eeee-4ddd-8ccc-bbbbbbbbbbbb";
  add_cleanup_record(first_cleanup, chained_work);
  add_cleanup_record(second_cleanup, first_pin);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto chained_recovery = hm::configurator_internal::recover_stale_archive_work_files(chained_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  const bool passed = chained_recovery.ok() && chained_recovery->empty() && !fs::exists(chained_work) &&
      !fs::exists(first_pin) && !fs::exists(second_pin) && !fs::exists(first_cleanup) && !fs::exists(second_cleanup);
  if (!passed)
    std::cerr << "Interrupted NFS cleanup recovery failed: " << chained_recovery.status() << '\n';

  // A crash can leave the private fallback as the only publication. Restoring
  // it to a public name uses the hard-link branch when rename flags fail.
  const fs::path restored_configured = root / "restored.mkv";
  const fs::path restored_source =
      root / "restored.hstream-run-v3-99999999-88888888-00112233-4455-6677-8899-aabbccddeeff.mkv";
  const fs::path restored_video = root / "restored-finalization-failed.mkv";
  const fs::path restore_cleanup = root / "hstream-cleanup-v2-11111111-2222-4333-8444-555555555555";
  add_cleanup_record(restore_cleanup, restored_source);
  std::ofstream(restore_cleanup / "guard", std::ios::binary) << "trusted interrupted video";
  fs::create_hard_link(restore_cleanup / "guard", restore_cleanup / "fallback");
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto restored_recovery = hm::configurator_internal::recover_stale_archive_work_files(restored_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream restored_stream(restored_video, std::ios::binary);
  const std::string restored_content{std::istreambuf_iterator<char>(restored_stream), std::istreambuf_iterator<char>()};
  restored_stream.close();
  const bool restore_passed = restored_recovery.ok() && restored_recovery->size() == 1 &&
      restored_recovery->front() == restored_video && restored_content == "trusted interrupted video" &&
      !fs::exists(restored_source) && !fs::exists(restore_cleanup);
  if (!restore_passed)
    std::cerr << "Public NFS fallback restoration failed: " << restored_recovery.status() << '\n';

  // The source may already be retired while the recovery pair's identity
  // guards remain. Their open descriptors must move to the published names.
  const fs::path guarded_configured = root / "guarded.mkv";
  const fs::path guarded_video = root / "guarded-finalization-failed.mkv";
  const fs::path guarded_log = guarded_video.string() + ".log";
  const fs::path guarded_video_pin = guarded_video.string() + ".hstream-pin";
  const fs::path guarded_log_pin = guarded_log.string() + ".hstream-pin";
  std::ofstream(guarded_video, std::ios::binary) << "guarded video";
  std::ofstream(guarded_log, std::ios::binary) << "guarded log";
  fs::create_hard_link(guarded_video, guarded_video_pin);
  fs::create_hard_link(guarded_log, guarded_log_pin);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto guarded_recovery = hm::configurator_internal::recover_stale_archive_work_files(guarded_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  const bool guarded_passed = guarded_recovery.ok() && guarded_recovery->size() == 1 &&
      guarded_recovery->front() == guarded_video && fs::exists(guarded_video) && fs::exists(guarded_log) &&
      !fs::exists(guarded_video_pin) && !fs::exists(guarded_log_pin);
  if (!guarded_passed)
    std::cerr << "Interrupted guarded archive recovery failed: " << guarded_recovery.status() << '\n';

  // A foreign file can occupy the original recovery name after an
  // interruption. Rescue publishes the guarded inode under the next name.
  const fs::path rescue_configured = root / "rescued.mkv";
  const fs::path occupied_recovery = root / "rescued-finalization-failed.mkv";
  const fs::path rescued_video = root / "rescued-finalization-failed-1.mkv";
  const fs::path rescue_guard = occupied_recovery.string() + ".hstream-pin";
  std::ofstream(occupied_recovery, std::ios::binary) << "foreign video";
  std::ofstream(rescue_guard, std::ios::binary) << "rescued video";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto rescue_recovery = hm::configurator_internal::recover_stale_archive_work_files(rescue_configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream rescued_stream(rescued_video, std::ios::binary);
  const std::string rescued_content{std::istreambuf_iterator<char>(rescued_stream), std::istreambuf_iterator<char>()};
  rescued_stream.close();
  std::ifstream occupied_stream(occupied_recovery, std::ios::binary);
  const std::string occupied_content{std::istreambuf_iterator<char>(occupied_stream), std::istreambuf_iterator<char>()};
  occupied_stream.close();
  const bool rescue_passed = rescue_recovery.ok() && rescue_recovery->size() == 1 &&
      rescue_recovery->front() == rescued_video && rescued_content == "rescued video" &&
      occupied_content == "foreign video" && !fs::exists(rescue_guard);
  if (!rescue_passed)
    std::cerr << "Guarded NFS rescue failed: " << rescue_recovery.status() << '\n';

  // If the source disappears after the durable fallback is published, a
  // failed quarantine must leave that last link for the next reconciliation.
  const fs::path vanished_source = root / "vanished.mkv";
  const fs::path vanished_fallback = vanished_source.string() + ".hstream-cleanup-pin";
  std::ofstream(vanished_source, std::ios::binary) << "retained after disappearance";
  struct stat vanished_stat{};
  const bool vanished_stat_ok = ::lstat(vanished_source.c_str(), &vanished_stat) == 0;
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REMOVE_SOURCE_BEFORE_ARCHIVE_QUARANTINE", vanished_source.c_str(), TRUE);
  const absl::Status vanished_first = vanished_stat_ok
      ? hm::configurator_internal::remove_archive_entry_if_owned_for_test(
            vanished_source, static_cast<uintmax_t>(vanished_stat.st_dev), static_cast<uintmax_t>(vanished_stat.st_ino))
      : absl::InternalError("disappearing source fixture is unavailable");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REMOVE_SOURCE_BEFORE_ARCHIVE_QUARANTINE");
  const bool fallback_retained = !vanished_first.ok() && !fs::exists(vanished_source) && fs::exists(vanished_fallback);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto vanished_restart = hm::configurator_internal::recover_stale_archive_work_files(vanished_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream vanished_stream(vanished_source, std::ios::binary);
  const std::string vanished_content{std::istreambuf_iterator<char>(vanished_stream), std::istreambuf_iterator<char>()};
  vanished_stream.close();
  const bool vanished_passed = fallback_retained && vanished_restart.ok() && vanished_restart->empty() &&
      vanished_content == "retained after disappearance" && !fs::exists(vanished_fallback);
  if (!vanished_passed)
    std::cerr << "Disappearing source recovery failed: " << vanished_first << "; " << vanished_restart.status() << '\n';

  // Rollback removes the published log after a failed source quarantine. An
  // open descriptor on that log leaves a .nfs* entry in the private cleanup
  // directory, preventing the directory from being retired.
  const fs::path rollback_dir = root / "rollback";
  fs::create_directory(rollback_dir);
  const fs::path rollback_source = rollback_dir / "recording.mkv";
  const fs::path rollback_log = rollback_source.string() + ".log";
  const fs::path rollback_recovery = rollback_dir / "recording-finalization-failed.mkv";
  const fs::path rollback_recovery_log = rollback_recovery.string() + ".log";
  std::ofstream(rollback_source, std::ios::binary) << "trusted rollback video";
  std::ofstream(rollback_log, std::ios::binary) << "trusted rollback log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE", "1", TRUE);
  const auto rollback = hm::configurator_internal::preserve_existing_archive_work_file(rollback_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_REPLACE_ARCHIVE_AFTER_QUARANTINE");
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream rollback_video_stream(rollback_source, std::ios::binary);
  const std::string rollback_video_content{
      std::istreambuf_iterator<char>(rollback_video_stream), std::istreambuf_iterator<char>()};
  rollback_video_stream.close();
  std::ifstream rollback_log_stream(rollback_log, std::ios::binary);
  const std::string rollback_log_content{
      std::istreambuf_iterator<char>(rollback_log_stream), std::istreambuf_iterator<char>()};
  rollback_log_stream.close();
  bool rollback_cleanup_retired = true;
  for (const auto& entry : fs::directory_iterator(rollback_dir)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("hstream-cleanup-v2-", 0) == 0 || name.rfind(".nfs", 0) == 0)
      rollback_cleanup_retired = false;
  }
  const bool rollback_passed = !rollback.ok() && rollback_video_content == "trusted rollback video" &&
      rollback_log_content == "trusted rollback log" && fs::exists(rollback_recovery) &&
      !fs::exists(rollback_recovery_log) && rollback_cleanup_retired;
  if (!rollback_passed)
    std::cerr << "NFS log rollback failed: " << rollback.status() << '\n';

  // A late cleanup error can arrive after the video source name is gone.
  // Retain the published pair and guards so restart can finish the log move.
  const fs::path late_dir = root / "late-error";
  fs::create_directory(late_dir);
  const fs::path late_source = late_dir / "recording.mkv";
  const fs::path late_log = late_source.string() + ".log";
  const fs::path late_recovery = late_dir / "recording-finalization-failed.mkv";
  const fs::path late_recovery_log = late_recovery.string() + ".log";
  std::ofstream(late_source, std::ios::binary) << "late video";
  std::ofstream(late_log, std::ios::binary) << "late log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_ARCHIVE_PRIVATE_GUARD_UNLINK_FAILURE", late_source.c_str(), TRUE);
  const auto late_first = hm::configurator_internal::preserve_existing_archive_work_file(late_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_ARCHIVE_PRIVATE_GUARD_UNLINK_FAILURE");
  const bool late_pair_retained = !late_first.ok() && !fs::exists(late_source) && fs::exists(late_recovery) &&
      fs::exists(late_recovery_log) && fs::exists(late_recovery.string() + ".hstream-pin") &&
      fs::exists(late_recovery_log.string() + ".hstream-pin");
  const auto late_restart = hm::configurator_internal::recover_stale_archive_work_files(late_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream late_video_stream(late_recovery, std::ios::binary);
  const std::string late_video_content{
      std::istreambuf_iterator<char>(late_video_stream), std::istreambuf_iterator<char>()};
  late_video_stream.close();
  std::ifstream late_log_stream(late_recovery_log, std::ios::binary);
  const std::string late_log_content{std::istreambuf_iterator<char>(late_log_stream), std::istreambuf_iterator<char>()};
  late_log_stream.close();
  const bool late_passed = late_pair_retained && late_restart.ok() && late_restart->size() == 1 &&
      late_restart->front() == late_recovery && late_video_content == "late video" && late_log_content == "late log" &&
      !fs::exists(late_log) && !fs::exists(late_log.string() + ".hstream-pin") &&
      !fs::exists(late_source.string() + ".hstream-pin") && !fs::exists(late_recovery.string() + ".hstream-pin") &&
      !fs::exists(late_recovery_log.string() + ".hstream-pin");
  if (!late_passed)
    std::cerr << "Late archive cleanup recovery failed: " << late_first.status() << "; " << late_restart.status()
              << '\n';

  // Recovery guards can be durable while the configured source is still
  // present. Restart must leave that transaction for source preservation.
  const fs::path early_dir = root / "early-guards";
  fs::create_directory(early_dir);
  const fs::path early_source = early_dir / "recording.mkv";
  const fs::path early_log = early_source.string() + ".log";
  const fs::path early_recovery = early_dir / "recording-finalization-failed.mkv";
  const fs::path early_recovery_log = early_recovery.string() + ".log";
  std::ofstream(early_source, std::ios::binary) << "early video";
  std::ofstream(early_log, std::ios::binary) << "early log";
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_INTERRUPT_AFTER_ARCHIVE_GUARD_PUBLICATION", "1", TRUE);
  const auto early_first = hm::configurator_internal::preserve_existing_archive_work_file(early_source);
  const auto early_restart = hm::configurator_internal::recover_stale_archive_work_files(early_source);
  const bool early_source_retained = !early_first.ok() && early_restart.ok() && early_restart->empty() &&
      fs::exists(early_source) && fs::exists(early_log) && fs::exists(early_source.string() + ".hstream-pin") &&
      fs::exists(early_log.string() + ".hstream-pin");
  const auto early_final = hm::configurator_internal::preserve_existing_archive_work_file(early_source);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  std::ifstream early_video_stream(early_recovery, std::ios::binary);
  const std::string early_video_content{
      std::istreambuf_iterator<char>(early_video_stream), std::istreambuf_iterator<char>()};
  early_video_stream.close();
  std::ifstream early_log_stream(early_recovery_log, std::ios::binary);
  const std::string early_log_content{
      std::istreambuf_iterator<char>(early_log_stream), std::istreambuf_iterator<char>()};
  early_log_stream.close();
  const bool early_passed = early_source_retained && early_final.ok() && early_final->has_value() &&
      early_final->value() == early_recovery && early_video_content == "early video" &&
      early_log_content == "early log" && !fs::exists(early_source) && !fs::exists(early_log);
  if (!early_passed)
    std::cerr << "Early archive guard recovery failed: " << early_first.status() << "; " << early_restart.status()
              << "; " << early_final.status() << '\n';
  remove_fixture();
  return recorded_passed && passed && restore_passed && guarded_passed && rescue_passed && vanished_passed &&
          rollback_passed && late_passed && early_passed
      ? 0
      : 1;
}
