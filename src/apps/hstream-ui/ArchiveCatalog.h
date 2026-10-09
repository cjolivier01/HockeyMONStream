#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

namespace hm::ui {

// A published destination archive in a game directory, together with whatever
// the publication-time sidecar recorded about the run that produced it.
struct ArchiveEntry {
  QString path;
  QString game_id;
  // "program", "program_4k" or "stitched". The on-disk filename spells the
  // program route "tracking"; this is the name the UI uses.
  QString kind;
  qint64 generation{0};
  // Game time of the archive's first frame. Archives do not carry the run's
  // --start-time, so this is only meaningful when start_time_known is set.
  qint64 start_time_ms{0};
  bool start_time_known{false};
  qint64 duration_ms{-1};
  int width{0};
  int height{0};
};

// Label for an archive kind, as shown in the UI.
QString ArchiveKindDisplayName(const QString& kind);

// Path of the sidecar that records an archive's game-time origin.
QString ArchiveSidecarPath(const QString& archive_path);

// Published archives for game_id under game_dir, best first: program, then
// program_4k, then stitched, each with the newest generation first. Entries
// still guarded by a .hstream-pin reservation are skipped.
QVector<ArchiveEntry> DiscoverArchives(const QString& game_dir, const QString& game_id);

// Fills the sidecar-backed fields of entry. Returns false and sets error when
// no sidecar exists or it cannot be read; the entry is left untouched in that
// case, so callers that only want the extra detail can ignore the failure.
bool LoadArchiveSidecar(const QString& archive_path, ArchiveEntry* entry, QString* error = nullptr);

// Writes entry.path's sidecar durably: temporary file, fsync, rename, then an
// fsync of the containing directory.
bool SaveArchiveSidecar(const ArchiveEntry& entry, QString* error = nullptr);

} // namespace hm::ui
