#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <sys/stat.h>

#include <array>
#include <iostream>

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

  const auto published = hm::ui_internal::publish_telemetry_csvs(manifest_path, game, "-2");
  bool valid = expect(published.ok, published.error.toStdString().c_str()) &&
      expect(published.published_paths.size() == 6, "all six CSVs must be copied");
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

  const auto collision = hm::ui_internal::publish_telemetry_csvs(manifest_path, game, "-2");
  valid &= expect(!collision.ok, "copy publication must never replace an existing game CSV");
  for (const QString& entry : QDir(game).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
    valid &= expect(!entry.startsWith('.'), "game directory must not retain hidden telemetry staging files");
  }

  const QString rollback_game = QDir(root.path()).filePath("rollback-game");
  valid &= expect(QDir().mkpath(rollback_game), "rollback test game directory must be created");
  const QString foreign_tracking = QDir(rollback_game).filePath("tracking-4.csv");
  valid &= expect(write_file(foreign_tracking, "foreign tracking\n"), "foreign tracking fixture must be written");
  const auto tracking_collision = hm::ui_internal::publish_telemetry_csvs(manifest_path, rollback_game, "-4");
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
  return valid ? 0 : 1;
}
