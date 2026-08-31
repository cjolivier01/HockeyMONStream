#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace hm::ui_internal {

struct TelemetryCsvPublicationResult {
  bool ok{false};
  QStringList published_paths;
  QString error;
};

// Fault controls used only by TelemetryCsvPublisherTest. Production callers
// should leave this null.
struct TelemetryCsvPublicationTestHooks {
  bool force_named_temporary_files{false};
  bool fail_tracking_commit_sync{false};
  QString rollback_identity_error_filename;
};

// Returns the suffix shared by a finalized Program video and its CSVs: an
// empty string for the first generation, or "-N" for later generations.
// An invalid or unrelated archive path returns a null QString.
QString finalized_archive_csv_suffix(const QString& archive_path, const QString& game_id);

// Returns true only when none of the six final CSV names for this suffix
// already exists in the game directory.
bool telemetry_csv_destination_paths_available(const QString& game_directory, const QString& destination_suffix);

// Copies a committed telemetry generation from its non-hidden working files
// into non-hidden, no-replace files in the game directory. Each copy is fully
// written and synced before it is linked without replacement; tracking is
// atomically linked last as HM's discovery marker. Filesystems that cannot
// create unnamed temporary files use strictly named, non-hidden staging files.
TelemetryCsvPublicationResult publish_telemetry_csvs(
    const QString& manifest_path,
    const QString& game_directory,
    const QString& destination_suffix,
    const TelemetryCsvPublicationTestHooks* test_hooks = nullptr);

} // namespace hm::ui_internal
