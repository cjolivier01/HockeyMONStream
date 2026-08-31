#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace hm::ui_internal {

struct TelemetryCsvPublicationResult {
  bool ok{false};
  QStringList published_paths;
  QString error;
};

// Returns the suffix shared by a finalized Program video and its CSVs: an
// empty string for the first generation, or "-N" for later generations.
// An invalid or unrelated archive path returns a null QString.
QString finalized_archive_csv_suffix(const QString& archive_path, const QString& game_id);

// Copies a committed telemetry generation from its non-hidden working files
// into non-hidden, no-replace files in the game directory. Every destination
// uses destination_suffix; tracking is copied last as HM's discovery marker.
TelemetryCsvPublicationResult publish_telemetry_csvs(
    const QString& manifest_path,
    const QString& game_directory,
    const QString& destination_suffix);

} // namespace hm::ui_internal
