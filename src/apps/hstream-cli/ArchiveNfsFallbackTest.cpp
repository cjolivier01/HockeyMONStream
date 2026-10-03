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
  std::ofstream(work, std::ios::binary);
  g_setenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED", "1", TRUE);
  const auto simple_recovery = hm::configurator_internal::recover_stale_archive_work_files(configured);
  g_unsetenv("HSTREAM_CONFIGURATOR_TEST_FORCE_RENAME_NOREPLACE_UNSUPPORTED");
  if (!simple_recovery.ok() || !simple_recovery->empty() || fs::exists(work)) {
    std::cerr << "Empty archive recovery failed: " << simple_recovery.status() << '\n';
    remove_fixture();
    return 1;
  }

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
  remove_fixture();
  return passed ? 0 : 1;
}
