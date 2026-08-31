#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <sys/stat.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

bool write_file(const QString& path, const QByteArray& contents) {
  QFile output(path);
  return output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.write(contents) == contents.size() &&
      output.flush();
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

bool expect_no_staging_files(const QString& directory) {
  bool valid = true;
  for (const QString& entry : QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
    valid &= expect(!entry.startsWith('.'), "game directory must not retain hidden telemetry staging files");
    valid &= expect(
        !entry.startsWith("hstream-telemetry-stage-v1-"),
        "game directory must not retain owned telemetry staging directories");
  }
  return valid;
}

struct MarkerCloseRace {
  std::mutex mutex;
  std::condition_variable changed;
  bool marker_closed{false};
  bool second_waiting_for_lock{false};
  bool second_acquired_lock{false};
  bool second_was_blocked{false};
};

void pause_after_marker_close(void* context) {
  auto* race = static_cast<MarkerCloseRace*>(context);
  std::unique_lock<std::mutex> lock(race->mutex);
  race->marker_closed = true;
  race->changed.notify_all();
  if (!race->changed.wait_for(lock, std::chrono::seconds(5), [&]() { return race->second_waiting_for_lock; }))
    return;
  race->second_was_blocked =
      !race->changed.wait_for(lock, std::chrono::milliseconds(200), [&]() { return race->second_acquired_lock; });
}

void note_before_publication_lock(void* context) {
  auto* race = static_cast<MarkerCloseRace*>(context);
  std::lock_guard<std::mutex> lock(race->mutex);
  race->second_waiting_for_lock = true;
  race->changed.notify_all();
}

void note_after_publication_lock(void* context) {
  auto* race = static_cast<MarkerCloseRace*>(context);
  std::lock_guard<std::mutex> lock(race->mutex);
  race->second_acquired_lock = true;
  race->changed.notify_all();
}

} // namespace

int main() {
  QTemporaryDir root;
  if (!expect(root.isValid(), "temporary directory must be available"))
    return 1;
  const QString working = QDir(root.path()).filePath("working/game");
  const QString game = QDir(root.path()).filePath("game");
  if (!expect(QDir().mkpath(working) && QDir().mkpath(game), "test directories must be created"))
    return 1;

  const std::array<QString, 6> stems = {
      "tracking", "detections", "camera", "camera_fast", "hstream_frame_index", "hstream_config_events"};
  for (const QString& stem : stems) {
    if (!expect(
            write_file(QDir(working).filePath(stem + "-7.csv"), (stem + " contents\n").toUtf8()),
            "working telemetry fixture must be written")) {
      return 1;
    }
  }
  const QByteArray manifest = R"json({
  "publication_state": "committed",
  "completed": true,
  "eligible_for_training": false,
  "hm_compatibility": {
    "tracking_csv": {"file": "tracking-7.csv"},
    "detections_csv": {"file": "detections-7.csv"},
    "camera_csv": {"file": "camera-7.csv"},
    "camera_fast_csv": {"file": "camera_fast-7.csv"}
  },
  "sidecars": {
    "frame_index": "hstream_frame_index-7.csv",
    "config_events": "hstream_config_events-7.csv"
  }
})json";
  const QString manifest_path = QDir(working).filePath("hstream_telemetry-7.json");
  if (!expect(write_file(manifest_path, manifest), "telemetry manifest fixture must be written"))
    return 1;

  bool valid = expect(
      hm::ui_internal::telemetry_csv_destination_paths_available(game, "-2"),
      "an unused telemetry suffix must be available before archive publication");
  hm::ui_internal::TelemetryCsvPublicationTestHooks named_fallback;
  named_fallback.force_named_temporary_files = true;
  const auto published = hm::ui_internal::publish_telemetry_csvs(manifest_path, game, "-2");
  valid &= expect(published.ok, published.error.toStdString().c_str()) &&
      expect(published.published_paths.size() == 6, "all six CSVs must be copied");
  valid &= expect(
      !hm::ui_internal::telemetry_csv_destination_paths_available(game, "-2"),
      "a suffix with existing telemetry inputs must be skipped when naming the archive");
  for (const QString& stem : stems) {
    const QString source = QDir(working).filePath(stem + "-7.csv");
    const QString destination = QDir(game).filePath(stem + "-2.csv");
    struct stat source_stat{};
    struct stat destination_stat{};
    valid &= expect(QFileInfo::exists(destination), "non-hidden game CSV must exist");
    valid &= expect(
        ::stat(QFile::encodeName(source).constData(), &source_stat) == 0 &&
            ::stat(QFile::encodeName(destination).constData(), &destination_stat) == 0 &&
            (source_stat.st_dev != destination_stat.st_dev || source_stat.st_ino != destination_stat.st_ino),
        "game CSV must be a copy, not a hard link to working storage");
  }
  valid &= expect(
      hm::ui_internal::finalized_archive_csv_suffix(
          QDir(game).filePath("sabercats-16a-tracking_output-with-audio-2.mp4"), "sabercats-16a") == "-2",
      "CSV suffix must match the finalized video suffix");
  valid &= expect(
      hm::ui_internal::finalized_archive_csv_suffix(
          QDir(game).filePath("sabercats-16a-tracking_output-with-audio.mp4"), "sabercats-16a") == "",
      "first video generation must use bare CSV names");
  valid &= expect(
      hm::ui_internal::finalized_archive_csv_suffix(QDir(game).filePath("unrelated.mp4"), "sabercats-16a").isNull(),
      "unrelated video names must not produce a CSV suffix");

  const QString fallback_game = QDir(root.path()).filePath("fallback-game");
  valid &= expect(QDir().mkpath(fallback_game), "named fallback game directory must be created");
  const auto fallback_published =
      hm::ui_internal::publish_telemetry_csvs(manifest_path, fallback_game, "-6", &named_fallback);
  valid &= expect(fallback_published.ok, fallback_published.error.toStdString().c_str()) &&
      expect(fallback_published.published_paths.size() == 6, "named fallback must copy all six CSVs");
  for (const QString& stem : stems) {
    valid &=
        expect(QFileInfo::exists(QDir(fallback_game).filePath(stem + "-6.csv")), "named fallback game CSV must exist");
  }
  valid &= expect_no_staging_files(fallback_game);

  const QString unowned_game = QDir(root.path()).filePath("unowned-game");
  const QString unowned_stage =
      QDir(unowned_game).filePath("hstream-telemetry-stage-v1-00000000000000000000000000000000");
  const QString unowned_marker = QDir(unowned_stage).filePath("ownership");
  valid &= expect(QDir().mkpath(unowned_stage), "unowned staging-like directory must be created");
  valid &= expect(write_file(unowned_marker, "unowned contents\n"), "unowned staging-like marker must be written");
  const auto preserved_unowned =
      hm::ui_internal::publish_telemetry_csvs(manifest_path, unowned_game, "-8", &named_fallback);
  QFile unowned_marker_file(unowned_marker);
  const bool unowned_marker_opened = unowned_marker_file.open(QIODevice::ReadOnly);
  valid &= expect(preserved_unowned.ok, preserved_unowned.error.toStdString().c_str());
  valid &= expect(
      unowned_marker_opened && unowned_marker_file.readAll() == "unowned contents\n",
      "publication recovery must preserve staging-like state without a valid ownership marker");

  const QString recovery_game = QDir(root.path()).filePath("recovery-game");
  valid &= expect(QDir().mkpath(recovery_game), "crash recovery game directory must be created");
  hm::ui_internal::TelemetryCsvPublicationTestHooks abandoned_staging = named_fallback;
  abandoned_staging.abandon_named_staging_after_copy = true;
  const auto abandoned =
      hm::ui_internal::publish_telemetry_csvs(manifest_path, recovery_game, "-6", &abandoned_staging);
  valid &= expect(!abandoned.ok, "simulated interruption must abandon owned staging state");
  valid &= expect(
      QDir(recovery_game)
              .entryList(QStringList{"hstream-telemetry-stage-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot)
              .size() == 1,
      "simulated interruption must leave one recoverable staging directory");
  const auto recovered = hm::ui_internal::publish_telemetry_csvs(manifest_path, recovery_game, "-7", &named_fallback);
  valid &= expect(recovered.ok, recovered.error.toStdString().c_str());
  valid &= expect_no_staging_files(recovery_game);
  for (const QString& stem : stems) {
    valid &= expect(
        !QFileInfo::exists(QDir(recovery_game).filePath(stem + "-6.csv")),
        "recovery must not publish the abandoned suffix");
    valid &= expect(
        QFileInfo::exists(QDir(recovery_game).filePath(stem + "-7.csv")),
        "new-suffix publication must succeed after stale staging recovery");
  }

  const QString concurrent_game = QDir(root.path()).filePath("concurrent-game");
  valid &= expect(QDir().mkpath(concurrent_game), "concurrent publication game directory must be created");
  MarkerCloseRace marker_close_race;
  hm::ui_internal::TelemetryCsvPublicationTestHooks first_concurrent = named_fallback;
  first_concurrent.callback_context = &marker_close_race;
  first_concurrent.after_named_marker_close = pause_after_marker_close;
  hm::ui_internal::TelemetryCsvPublicationTestHooks second_concurrent = named_fallback;
  second_concurrent.callback_context = &marker_close_race;
  second_concurrent.before_publication_lock = note_before_publication_lock;
  second_concurrent.after_publication_lock = note_after_publication_lock;
  hm::ui_internal::TelemetryCsvPublicationResult first_concurrent_result;
  hm::ui_internal::TelemetryCsvPublicationResult second_concurrent_result;
  std::thread first_publisher([&]() {
    first_concurrent_result =
        hm::ui_internal::publish_telemetry_csvs(manifest_path, concurrent_game, "-9", &first_concurrent);
  });
  {
    std::unique_lock<std::mutex> lock(marker_close_race.mutex);
    valid &= expect(
        marker_close_race.changed.wait_for(
            lock, std::chrono::seconds(5), [&]() { return marker_close_race.marker_closed; }),
        "first publisher must reach the closed-marker cleanup window");
  }
  std::thread second_publisher([&]() {
    second_concurrent_result =
        hm::ui_internal::publish_telemetry_csvs(manifest_path, concurrent_game, "-10", &second_concurrent);
  });
  first_publisher.join();
  second_publisher.join();
  valid &= expect(first_concurrent_result.ok, first_concurrent_result.error.toStdString().c_str());
  valid &= expect(second_concurrent_result.ok, second_concurrent_result.error.toStdString().c_str());
  valid &= expect(
      marker_close_race.second_was_blocked,
      "a second publisher must remain blocked while the first removes its closed ownership marker");
  struct stat publication_lock_info{};
  valid &= expect(
      ::stat(
          QFile::encodeName(QDir(concurrent_game).filePath("hstream-telemetry-publication.lock")).constData(),
          &publication_lock_info) == 0 &&
          S_ISREG(publication_lock_info.st_mode) && (publication_lock_info.st_mode & 0777) == (S_IRUSR | S_IWUSR) &&
          publication_lock_info.st_nlink == 1 && publication_lock_info.st_size == 0,
      "publication serialization must use a persistent, private regular lock file");
  valid &= expect_no_staging_files(concurrent_game);

  const auto collision = hm::ui_internal::publish_telemetry_csvs(manifest_path, game, "-2", &named_fallback);
  valid &= expect(!collision.ok, "copy publication must never replace an existing game CSV");
  valid &= expect_no_staging_files(game);

  const QString tracking_source = QDir(working).filePath("tracking-7.csv");
  const QString saved_tracking_source = tracking_source + ".saved";
  valid &= expect(QFile::rename(tracking_source, saved_tracking_source), "tracking source must be hidden from copier");
  const auto incomplete_copy = hm::ui_internal::publish_telemetry_csvs(manifest_path, game, "-3", &named_fallback);
  valid &= expect(!incomplete_copy.ok, "a missing source must fail named fallback publication");
  valid &= expect_no_staging_files(game);
  for (const QString& stem : stems) {
    valid &= expect(
        !QFileInfo::exists(QDir(game).filePath(stem + "-3.csv")), "a copy failure must not publish any game CSV");
  }
  valid &= expect(QFile::rename(saved_tracking_source, tracking_source), "tracking source fixture must be restored");

  const QString rollback_game = QDir(root.path()).filePath("rollback-game");
  valid &= expect(QDir().mkpath(rollback_game), "rollback test game directory must be created");
  const QString foreign_tracking = QDir(rollback_game).filePath("tracking-4.csv");
  valid &= expect(write_file(foreign_tracking, "foreign tracking\n"), "foreign tracking fixture must be written");
  const auto tracking_collision =
      hm::ui_internal::publish_telemetry_csvs(manifest_path, rollback_game, "-4", &named_fallback);
  QFile foreign_tracking_file(foreign_tracking);
  const bool foreign_tracking_opened = foreign_tracking_file.open(QIODevice::ReadOnly);
  valid &= expect(!tracking_collision.ok, "tracking collision must fail the whole CSV publication");
  valid &= expect(
      foreign_tracking_opened && foreign_tracking_file.readAll() == "foreign tracking\n",
      "tracking collision must preserve the existing discovery marker");
  for (const QString& stem : stems) {
    if (stem == "tracking")
      continue;
    valid &= expect(
        !QFileInfo::exists(QDir(rollback_game).filePath(stem + "-4.csv")),
        "tracking collision must roll back every copied companion CSV");
  }

  valid &= expect_no_staging_files(rollback_game);

  const QString ambiguous_game = QDir(root.path()).filePath("ambiguous-game");
  valid &= expect(QDir().mkpath(ambiguous_game), "ambiguous rollback game directory must be created");
  hm::ui_internal::TelemetryCsvPublicationTestHooks ambiguous_rollback = named_fallback;
  ambiguous_rollback.fail_tracking_commit_sync = true;
  ambiguous_rollback.rollback_identity_error_filename = "tracking-5.csv";
  const auto ambiguous =
      hm::ui_internal::publish_telemetry_csvs(manifest_path, ambiguous_game, "-5", &ambiguous_rollback);
  valid &= expect(!ambiguous.ok, "an ambiguous tracking rollback must report publication failure");
  valid &= expect(
      ambiguous.error.contains("retaining complete telemetry set"),
      "ambiguous tracking rollback must report that the complete set was retained");
  for (const QString& stem : stems) {
    valid &= expect(
        QFileInfo::exists(QDir(ambiguous_game).filePath(stem + "-5.csv")),
        "ambiguous tracking rollback must retain every companion CSV");
  }
  valid &= expect_no_staging_files(ambiguous_game);
  return valid ? 0 : 1;
}
