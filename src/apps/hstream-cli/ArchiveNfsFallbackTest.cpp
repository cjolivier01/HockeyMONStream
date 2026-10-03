#include "src/apps/hstream-cli/configurator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <glib.h>
#include <gst/gst.h>
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
  remove_fixture();
  return recorded_passed && passed && restore_passed && guarded_passed && rescue_passed ? 0 : 1;
}
