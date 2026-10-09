#include "src/apps/hstream-ui/ArchiveCatalog.h"

#include <iostream>

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_file(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

} // namespace

int main() {
  using hm::ui::ArchiveEntry;
  using hm::ui::ArchiveKindDisplayName;
  using hm::ui::ArchiveSidecarPath;
  using hm::ui::DiscoverArchives;
  using hm::ui::LoadArchiveSidecar;
  using hm::ui::SaveArchiveSidecar;

  bool ok = true;
  QTemporaryDir directory;
  ok &= expect(directory.isValid(), "temporary test directory must be available");
  if (!directory.isValid())
    return 1;
  const auto path_for = [&directory](const QString& name) { return directory.filePath(name); };

  ok &= expect(
      write_file(path_for("gse-16a-tracking_output-with-audio-1.mp4"), "a") &&
          write_file(path_for("gse-16a-tracking_output-with-audio-2.mp4"), "a") &&
          write_file(path_for("gse-16a-program_4k_output-with-audio-1.mp4"), "a") &&
          write_file(path_for("gse-16a-stitched_output.mp4"), "a") &&
          write_file(path_for("gse-16a-tracking_output-with-audio-9.mp4"), "a") &&
          write_file(path_for("gse-16a-tracking_output-with-audio-9.mp4.hstream-pin"), "pinned") &&
          write_file(path_for("gse-16a-tracking_output-with-audio-8.mp4"), "") &&
          write_file(path_for("gse-16a-tracking_output-partial.mp4"), "a") &&
          write_file(path_for("other-game-tracking_output-with-audio-1.mp4"), "a"),
      "archive fixtures must be writable");

  QVector<ArchiveEntry> archives = DiscoverArchives(directory.path(), "gse-16a");
  ok &= expect(archives.size() == 4, "only well-formed, unpinned, non-empty archives for this game may be discovered");
  if (archives.size() == 4) {
    ok &= expect(
        archives.at(0).kind == "program" && archives.at(0).generation == 2,
        "the newest full-resolution program archive must sort first");
    ok &= expect(
        archives.at(1).kind == "program" && archives.at(1).generation == 1,
        "older generations of the same kind must follow the newest");
    ok &= expect(
        archives.at(2).kind == "program_4k" && archives.at(2).generation == 1,
        "the 4K reduction must rank below the full-resolution program");
    ok &= expect(
        archives.at(3).kind == "stitched" && archives.at(3).generation == 0,
        "an archive without a generation suffix must read as generation zero");
    ok &= expect(
        archives.at(0).game_id == "gse-16a" && archives.at(0).path == path_for("gse-16a-tracking_output-with-audio-2.mp4"),
        "discovery must report the full path and the requested game id");
    ok &= expect(
        !archives.at(0).start_time_known && archives.at(0).start_time_ms == 0 && archives.at(0).duration_ms == -1,
        "an archive with no sidecar must report an unknown start time");
  }

  ok &= expect(
      ArchiveSidecarPath("/games/x.mp4") == "/games/x.mp4.hstream-archive.json",
      "the sidecar must sit beside the archive under a predictable name");

  ArchiveEntry saved;
  saved.path = path_for("gse-16a-program_4k_output-with-audio-1.mp4");
  saved.game_id = "gse-16a";
  saved.kind = "program_4k";
  saved.generation = 1;
  saved.start_time_ms = 596000;
  saved.start_time_known = true;
  saved.duration_ms = 172757;
  saved.width = 3840;
  saved.height = 2160;
  QString error;
  ok &= expect(SaveArchiveSidecar(saved, &error), "a sidecar must save durably beside its archive");

  ArchiveEntry loaded;
  ok &= expect(LoadArchiveSidecar(saved.path, &loaded, &error), "a freshly written sidecar must load");
  ok &= expect(
      loaded.start_time_known && loaded.start_time_ms == 596000 && loaded.duration_ms == 172757 &&
          loaded.width == 3840 && loaded.height == 2160 && loaded.game_id == "gse-16a" && loaded.kind == "program_4k",
      "the sidecar round trip must preserve the run offset, duration, and geometry");

  QFile sidecar(ArchiveSidecarPath(saved.path));
  ok &= expect(sidecar.open(QIODevice::ReadOnly), "the written sidecar must be readable");
  const QJsonObject written = QJsonDocument::fromJson(sidecar.readAll()).object();
  sidecar.close();
  ok &= expect(
      written.value("start_time").toString() == "00:09:56",
      "the sidecar must carry a human-readable start time alongside the milliseconds");

  archives = DiscoverArchives(directory.path(), "gse-16a");
  ok &= expect(
      archives.size() == 4 && archives.at(2).start_time_known && archives.at(2).start_time_ms == 596000 &&
          archives.at(2).duration_ms == 172757,
      "discovery must fold a present sidecar into the entry");
  ok &= expect(
      !QFile::exists(ArchiveSidecarPath(saved.path) + ".partial"),
      "a durable sidecar write must leave no partial file behind");

  ok &= expect(
      write_file(
          ArchiveSidecarPath(saved.path),
          QJsonDocument(QJsonObject{{"schema", 2}, {"start_time_ms", 1}}).toJson()),
      "an unsupported-schema fixture must be writable");
  ArchiveEntry rejected;
  ok &= expect(
      !LoadArchiveSidecar(saved.path, &rejected, &error) && !error.isEmpty() && !rejected.start_time_known,
      "an unsupported sidecar schema must be refused rather than guessed at");
  ok &= expect(
      write_file(ArchiveSidecarPath(saved.path), QJsonDocument(QJsonObject{{"schema", 1}}).toJson()),
      "a start-time-less fixture must be writable");
  ok &= expect(
      !LoadArchiveSidecar(saved.path, &rejected, &error) && !rejected.start_time_known,
      "a sidecar without a start time must not pass as a known offset");
  ok &= expect(
      !LoadArchiveSidecar(path_for("gse-16a-stitched_output.mp4"), &rejected, &error),
      "a missing sidecar must fail rather than report a zero offset as known");

  ok &= expect(
      write_file(path_for("a_b-tracking_output-with-audio-1.mp4"), "a") &&
          DiscoverArchives(directory.path(), "a/b").size() == 1,
      "game ids containing separators must map onto the same sanitized filenames publication writes");
  ok &= expect(
      DiscoverArchives(directory.path(), "").isEmpty() && DiscoverArchives("", "gse-16a").isEmpty(),
      "discovery without a game or directory must find nothing");

  ok &= expect(
      ArchiveKindDisplayName("program") == "Program" && ArchiveKindDisplayName("program_4k") == "4K Program" &&
          ArchiveKindDisplayName("stitched") == "Stitched" && ArchiveKindDisplayName("other") == "other",
      "kind display names must stay readable and pass unknown kinds through");
  return ok ? 0 : 1;
}
